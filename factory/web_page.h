#pragma once
#include <Arduino.h>
static const char PAGE_HTML[] PROGMEM = R"AUTOHEATER_HTML(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101820">
<title>Autoheater ESP32</title>
<style>
:root{color-scheme:dark;--bg:#101820;--line:#2b3c47;--text:#f2f6f8;--muted:#92a4b0;--green:#47c58a}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;min-height:100vh;padding:env(safe-area-inset-top) 16px calc(22px + env(safe-area-inset-bottom));font:16px system-ui,-apple-system,"Segoe UI",sans-serif;background:radial-gradient(ellipse at top,#1b2b36 0%,var(--bg) 65%);color:var(--text)}
main{width:100%;max-width:480px;margin:0 auto;min-height:calc(100vh - 22px);display:flex;flex-direction:column;gap:16px}
header{padding:16px 4px 12px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center}
h1{font-size:17px;letter-spacing:.12em;margin:0}header small{display:block;color:var(--muted);font-size:9px;letter-spacing:.2em;margin-top:3px}
#connection{font-size:11px;color:var(--muted);text-align:right}
.card{background:linear-gradient(145deg,#1c2b36,#15212a);border:1px solid var(--line);border-radius:18px;padding:16px;box-shadow:0 10px 25px #0002}
.state{font-size:21px;font-weight:750;margin:0 0 14px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:14px 10px}
.label{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}
.value{font-size:23px;font-weight:750;margin-top:3px;white-space:nowrap}
.value small{font-size:12px;color:var(--muted);font-weight:500}
.section-title{font-size:11px;color:var(--muted);font-weight:700;letter-spacing:.14em;text-transform:uppercase;margin:0 0 12px}
.controls,.power-controls{display:grid;grid-template-columns:1fr 1fr;gap:9px}
.power-controls{margin-top:9px}
button{border:1px solid #344955;background:#263944;color:var(--text);border-radius:13px;min-height:54px;padding:12px 8px;font:700 15px system-ui;cursor:pointer;touch-action:manipulation}
button:active:not(:disabled){transform:scale(.98);background:#344955}
button:disabled{opacity:.35;cursor:not-allowed}
#start{background:var(--green);color:#092217;border-color:var(--green)}
#stop{background:#8f3838;border-color:#a94b4b}
#power-level{text-align:center;font-size:14px;color:var(--muted);margin-top:12px}
.note{font-size:11px;color:var(--muted);line-height:1.5;margin:12px 2px 0}
footer{margin-top:auto;text-align:center;font-size:10px;color:#60737e;padding:8px}
a{color:#8ee5c2}.error{color:#ff9b91}
</style>
</head>
<body>
<main>
<header><div><h1>AUTOHEATER</h1><small>ESP32 CONTROL</small></div><div id="connection">Подключение…</div></header>
<section class="card">
  <div class="section-title">Состояние отопителя</div><div class="state" id="state">Ожидание данных</div>
  <div class="grid">
    <div><div class="label">Температура узла</div><div class="value" id="temperature">— <small>°C</small></div></div>
    <div><div class="label">Напряжение</div><div class="value" id="voltage">— <small>В</small></div></div>
    <div><div class="label">Вентилятор</div><div class="value" id="fan">— <small>об/мин</small></div></div>
    <div><div class="label">Мощность</div><div class="value" id="power">— <small>Вт</small></div></div>
    <div><div class="label">Шаг программы</div><div class="value" id="step">—</div></div>
    <div><div class="label">Ошибка</div><div class="value" id="error">—</div></div>
  </div>
</section>
<section class="card">
  <div class="section-title">Управление</div>
  <div class="controls"><button id="start">ПУСК</button><button id="stop">СТОП</button></div>
  <div class="power-controls"><button id="power-down" disabled>Мощность −</button><button id="power-up" disabled>Мощность +</button></div>
  <div id="power-level">Управление мощностью недоступно без связи с отопителем</div>
  <p class="note">Команды отправляются в паузах шины. После остановки штатного пульта ESP32 продолжает опрашивать регистры отопителя. Кнопка «Стоп» остаётся доступной даже при потере телеметрии.</p>
</section>
<section class="card">
  <div class="section-title">Система</div>
  <div class="grid">
    <div><div class="label">Связь</div><div class="value" id="bus">Нет</div></div>
    <div><div class="label">Оверрайд мощности</div><div class="value" id="override">Нет</div></div>
    <div><div class="label">Валидные кадры</div><div class="value" id="valid">0</div></div>
    <div><div class="label">Ошибки CRC</div><div class="value" id="invalid">0</div></div>
  </div>
  <p class="note"><a href="/update">Обновление прошивки (OTA)</a></p>
</section>
<footer>LOCAL INTERFACE · AUTOHEATER ESP32</footer>
</main>
<script>
(() => {
  const $ = id => document.getElementById(id);
  const stateNames = {32:"Свеча / прогрев",36:"Розжиг",37:"Пламя поймано",39:"Набор мощности",47:"Выход на режим",46:"Установившееся горение",56:"Переход"};
  let socket;
  let reconnectTimer;
  let lastDataAt = 0;

  function setConnection(text, live) {
    $("connection").textContent = text;
    $("connection").style.color = live ? "var(--green)" : "var(--muted)";
  }
  function send(command) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(command);
  }
  function connect() {
    clearTimeout(reconnectTimer);
    socket = new WebSocket("ws://" + location.host + "/ws");
    socket.onopen = () => setConnection("Веб подключён", false);
    socket.onclose = () => {
      setConnection("Переподключение…", false);
      reconnectTimer = setTimeout(connect, 1500);
    };
    socket.onerror = () => socket.close();
    socket.onmessage = event => {
      let data;
      try { data = JSON.parse(event.data); } catch (_) { return; }
      lastDataAt = Date.now();
      const live = Boolean(data.live);
      const power = Number(data.pw);
      setConnection(live ? "Отопитель на связи" : "Нет свежей телеметрии", live);
      $("state").textContent = stateNames[data.st] || ("Состояние 0x" + Number(data.st).toString(16).toUpperCase().padStart(2,"0"));
      $("temperature").innerHTML = Number(data.t).toFixed(1) + " <small>°C</small>";
      $("voltage").innerHTML = Number(data.v).toFixed(2) + " <small>В</small>";
      $("fan").innerHTML = Number(data.fan).toLocaleString("ru-RU") + " <small>об/мин</small>";
      $("power").innerHTML = (Number.isFinite(power) && power > 0 ? power.toFixed(0) : "—") + " <small>Вт</small>";
      $("step").textContent = data.step;
      $("error").textContent = data.err === "FF" ? "Нет" : "0x" + data.err;
      $("error").classList.toggle("error", data.err !== "FF");
      $("bus").textContent = live ? "Есть" : "Нет";
      $("override").textContent = data.override ? "Активен" : "Нет";
      $("valid").textContent = data.valid;
      $("invalid").textContent = data.invalid;
      $("power-up").disabled = !live;
      $("power-down").disabled = !live;
      $("power-level").textContent = live
        ? "Текущая уставка: " + (Number.isFinite(power) ? power.toFixed(0) : "—") + " Вт"
        : "Управление мощностью недоступно без связи с отопителем";
    };
  }

  $("start").addEventListener("click", () => send("start"));
  $("stop").addEventListener("click", () => send("stop"));
  $("power-up").addEventListener("click", () => send("pup"));
  $("power-down").addEventListener("click", () => send("pdown"));
  setInterval(() => {
    if (lastDataAt && Date.now() - lastDataAt > 6000) {
      setConnection("Телеметрия устарела", false);
      $("power-up").disabled = true;
      $("power-down").disabled = true;
    }
  }, 1000);
  connect();
})();
</script>
</body>
</html>
)AUTOHEATER_HTML";
