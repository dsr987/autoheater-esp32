#!/usr/bin/env python3
"""Готовит временную Arduino-папку из App.ino и index.html."""

from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: prepare_sketch.py OUTPUT_DIRECTORY")

    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "App.ino", output / "Autoheater.ino")
    shutil.copy2(ROOT / "partitions.csv", output / "partitions.csv")
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "embed_web_page.py"),
            str(ROOT / "index.html"),
            str(output / "web_page.h"),
        ],
        check=True,
    )
    print(f"Arduino sketch prepared: {output}")


if __name__ == "__main__":
    main()
