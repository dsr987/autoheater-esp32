#!/usr/bin/env python3
"""Validate PR release metadata and apply version/changelog updates to app.cpp."""
from __future__ import annotations
import json, os, re, sys, uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_CPP = ROOT / "firmware" / "app" / "app.cpp"
STATE_PATH = ROOT / ".github" / "release-automation" / "state.json"
REPOSITORY_URL = "https://github.com/dsr987/autoheater-esp32"
RELEASE_TYPES = {
    "release:patch": "patch", "release:minor": "minor",
    "release:major": "major", "release:stable": "stable",
    "release:none": "none",
}
VERSION_PATTERN = re.compile(r"^\s*\*\s*CHANGELOG_VERSION:\s*(\d+\.\d+\.\d+)\s*$", re.MULTILINE)
CHANGELOG_BLOCK_PATTERN = re.compile(r"(?ms)^(\s*\*\s*CHANGELOG_START\s*\n).*?^(\s*\*\s*CHANGELOG_END\s*)$")

class ReleaseMetadataError(ValueError): pass

def parse_release_type(raw_labels: str) -> str:
    labels = [x.strip() for x in raw_labels.split(",") if x.strip()]
    release_labels = [x for x in labels if x.startswith("release:")]
    unknown = [x for x in release_labels if x not in RELEASE_TYPES]
    if unknown:
        raise ReleaseMetadataError("Unknown release label(s): " + ", ".join(unknown))
    if len(release_labels) != 1:
        raise ReleaseMetadataError("A PR must have exactly one release label: " + ", ".join(RELEASE_TYPES))
    return RELEASE_TYPES[release_labels[0]]

def current_version(text: str) -> tuple[int,int,int]:
    found = VERSION_PATTERN.findall(text)
    if len(found) != 1: raise ReleaseMetadataError(f"Expected exactly one CHANGELOG_VERSION marker, found {len(found)}")
    return tuple(int(x) for x in found[0].split("."))

def fmt(v): return ".".join(str(x) for x in v)

def bump(v, kind):
    major, minor, patch = v
    if kind == "none": return None
    if kind == "patch": return major, minor, patch + 1
    if kind == "minor": return major, minor + 1, 0
    if kind == "stable":
        if major != 0: raise ReleaseMetadataError("release:stable is only for promoting 0.x.y to 1.0.0")
        return 1, 0, 0
    if kind == "major":
        if major == 0: raise ReleaseMetadataError("Project is in 0.x beta; use release:stable for the first stable release")
        return major + 1, 0, 0
    raise ReleaseMetadataError("Unsupported release type: " + kind)

def extract_bullets(body: str) -> list[str]:
    m = re.search(r"(?ims)^##[ \t]+changelog[ \t]*\r?\n(.*?)(?=^##[ \t]+|\Z)", body)
    if not m: return []
    result=[]
    for raw in m.group(1).splitlines():
        b=re.match(r"^(?:[-*+]\s+|\d+[.)]\s+)(.+)$",raw.strip())
        if b:
            value=re.sub(r"\s+"," ",b.group(1)).strip()
            if value: result.append(value)
    return result

def validate_bullets(bullets):
    if not bullets: raise ReleaseMetadataError("Add ## Changelog with at least one bullet to the PR description")
    if any(len(x)>400 for x in bullets): raise ReleaseMetadataError("Each changelog bullet must be <=400 characters")

def write_output(**values):
    path=os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path,"a",encoding="utf-8") as f:
            for k,v in values.items(): f.write(f"{k}={v}\n")

def write_env(name,value):
    path=os.environ.get("GITHUB_ENV")
    if not path: raise ReleaseMetadataError("GITHUB_ENV is required")
    delimiter="END_"+uuid.uuid4().hex
    with open(path,"a",encoding="utf-8") as f: f.write(f"{name}<<{delimiter}\n{value}\n{delimiter}\n")

def load_state():
    if not STATE_PATH.exists(): return {"schema_version":1,"labels_initialized":False,"processed_prs":[],"release_prs":[]}
    try: state=json.loads(STATE_PATH.read_text(encoding="utf-8"))
    except (OSError,json.JSONDecodeError) as e: raise ReleaseMetadataError(f"Cannot read automation state: {e}") from e
    if state.get("schema_version") != 1 or not isinstance(state.get("processed_prs"),list):
        raise ReleaseMetadataError("Unsupported release automation state schema")
    state.setdefault("release_prs",[])
    if not isinstance(state["release_prs"],list): raise ReleaseMetadataError("release_prs must be a list")
    return state

def prepare_push(event_path):
    try:
        prs=json.loads(Path(event_path).read_text(encoding="utf-8"))
        if not isinstance(prs,list): raise ReleaseMetadataError("Associated PR response must be a list")
        candidates=[p for p in prs if isinstance(p,dict) and p.get("number") and p.get("merged_at")
          and (p.get("base") or {}).get("ref")=="main"
          and not str(((p.get("head") or {}).get("ref") or "")).startswith("automation/release-metadata-pr-")]
        if not candidates:
            print("No merged source PR associated with this main push; skipped.")
            write_output(process="false"); return 0
        if len(candidates)!=1: raise ReleaseMetadataError(f"Expected one merged PR, found {len(candidates)}; refusing to guess")
        pr=candidates[0]; number=pr.get("number")
        if not isinstance(number,int) or number<1: raise ReleaseMetadataError("Invalid associated PR number")
        labels=",".join(str(x.get("name","")) for x in pr.get("labels",[]) if isinstance(x,dict) and x.get("name"))
        state=load_state()
        if number not in {int(x) for x in state["processed_prs"]}:
            kind=parse_release_type(labels)
            validate_bullets(extract_bullets(str(pr.get("body") or "")))
        else: kind="already-processed"
        write_env("PR_NUMBER",str(number)); write_env("PR_LABELS",labels)
        write_env("PR_BODY",str(pr.get("body") or ""))
        write_output(process="true",release_type=kind)
        print(f"Resolved merged PR #{number}.")
        return 0
    except (OSError,ValueError,TypeError,ReleaseMetadataError) as e:
        print("ERROR: "+str(e),file=sys.stderr); return 1

def apply():
    try:
        state=load_state()
        raw=os.environ.get("PR_NUMBER","")
        if not raw.isdigit() or int(raw)<1: raise ReleaseMetadataError("PR_NUMBER must be a positive integer")
        number=int(raw); processed={int(x) for x in state["processed_prs"]}
        released={int(x) for x in state.get("release_prs",[])}
        if number in processed:
            write_output(release="true" if number in released else "false",changed="false",version=str(state.get("last_version","") if number in released else ""))
            print(f"PR #{number} already processed; idempotent no-op."); return 0
        kind=parse_release_type(os.environ.get("PR_LABELS",""))
        bullets=extract_bullets(os.environ.get("PR_BODY","")); validate_bullets(bullets)
        state["labels_initialized"]=True
        if kind=="none":
            state["processed_prs"]=sorted(processed|{number}); state["last_processed_pr"]=number
            state["last_release_type"]="none"; state["last_version"]=fmt(current_version(APP_CPP.read_text(encoding="utf-8")))
            STATE_PATH.parent.mkdir(parents=True,exist_ok=True)
            STATE_PATH.write_text(json.dumps(state,indent=2)+"\n",encoding="utf-8")
            write_output(release="false",changed="true",version="")
            print(f"PR #{number}: release:none; version unchanged."); return 0
        text=APP_CPP.read_text(encoding="utf-8")
        old=fmt(current_version(text)); new=fmt(bump(current_version(text),kind))
        text,n1=re.subn(r"(^\s*\*\s*CHANGELOG_VERSION:\s*)\d+\.\d+\.\d+(\s*$)",rf"\g<1>{new}\g<2>",text,count=1,flags=re.MULTILINE)
        if n1!=1: raise ReleaseMetadataError("Could not update CHANGELOG_VERSION")
        text,n2=re.subn(r"(^\s*\*\s*Версия разработки:\s*)\d+\.\d+\.\d+",rf"\g<1>{new}",text,count=1,flags=re.MULTILINE)
        if n2!=1: raise ReleaseMetadataError("Could not update development-version header")
        match=CHANGELOG_BLOCK_PATTERN.search(text)
        if not match: raise ReleaseMetadataError("Could not find CHANGELOG_START/CHANGELOG_END block")
        safe=[re.sub(r"\s+"," ",b).strip().replace("*/","* /") for b in bullets]
        safe[0]+=f" ([PR #{number}]({REPOSITORY_URL}/pull/{number}))"
        lines="".join(" * - "+b+"\n" for b in safe)
        text=text[:match.start()]+match.group(1)+lines+match.group(2)+text[match.end():]
        APP_CPP.write_text(text,encoding="utf-8",newline="\n")
        state["processed_prs"]=sorted(processed|{number}); state["release_prs"]=sorted(released|{number})
        state["last_processed_pr"]=number; state["last_release_type"]=kind; state["last_version"]=new
        STATE_PATH.parent.mkdir(parents=True,exist_ok=True); STATE_PATH.write_text(json.dumps(state,indent=2)+"\n",encoding="utf-8")
        write_output(release="true",changed="true",version=new)
        print(f"PR #{number}: {old} -> {new} ({kind})"); return 0
    except (OSError,ValueError,TypeError,ReleaseMetadataError) as e:
        print("ERROR: "+str(e),file=sys.stderr); return 1

def validate_pr():
    try:
        kind=parse_release_type(os.environ.get("PR_LABELS",""))
        bullets=extract_bullets(os.environ.get("PR_BODY","")); validate_bullets(bullets)
        old=fmt(current_version(APP_CPP.read_text(encoding="utf-8")))
        new=bump(current_version(APP_CPP.read_text(encoding="utf-8")),kind)
        print(f"OK: {kind}; current={old}; proposed={fmt(new) if new else 'unchanged'}")
        return 0
    except (OSError,ValueError,ReleaseMetadataError) as e:
        print("ERROR: "+str(e),file=sys.stderr); return 1

def main():
    if len(sys.argv)==2 and sys.argv[1]=="apply": return apply()
    if len(sys.argv)==2 and sys.argv[1]=="validate-pr": return validate_pr()
    if len(sys.argv)==3 and sys.argv[1]=="prepare-push": return prepare_push(sys.argv[2])
    print("Usage: release_automation.py {validate-pr|apply|prepare-push associated-prs.json}",file=sys.stderr); return 2
if __name__=="__main__": raise SystemExit(main())
