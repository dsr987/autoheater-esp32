# Autoheater ESP32 — протокол китайской дизельной автономки

Реверс-инжиниринг шины данных между пультом управления и основным блоком автономного дизельного отопителя, + прошивка ESP32 для дистанционного управления через веб-интерфейс.

**Схема подключения пульта:** 3 провода — красный **+5 В** (питание), синий **DATA** (одна линия, полудуплекс), чёрный **GND**.
**Декодирование в Logic 2:** Async Serial, **9600 бод, 8N1**, hex.

---

## 1. Физический уровень (подтверждено)

- Полудуплексный **UART по одному проводу**: TX и RX обоих устройств сидят на одной линии (открытый сток с подтяжкой к +5 В).
- Логика **5 В**; лог. 1 — высокий уровень, лог. 0 — притянут к земле.
- Мастер — пульт, слейв — печка. Цикл опроса пульта: ~**1,12–1,14 с**.
- Ответ слейва приходит через ~**8–14 мс** после запроса; между кадрами внутри троек команд — ~21 мс; пауза после ответа до следующего запроса — ~90–100 мс.
- Подтверждение скорости: минимальный битовый интервал ~104 мкс (1/104 мкс ≈ 9615 бод); межбайтовая пауза внутри кадра 1,03 мс = 10 битовых интервалов ровно.
- Одиночные битовые сбои на линии случаются (помехи от щелчков насоса); протокол переживает их: слейв корректно отвечает даже на запрос с 1 битым битом. CRC ловит порчу.

## 2. Формат кадра (подтверждено на всех логах)

```
[SYNC][CMD][LEN_HI][LEN_LO][DATA × LEN][CRC8]
```

| Поле | Значение |
|---|---|
| SYNC | `0xA5` — пульт (мастер), `0xA0` — печка (слейв) |
| CMD  | `0x30` — чтение регистра; `0x01` — запись параметра; `0x20` — телеметрия пульта; `0x00` — ответ слейва |
| LEN  | длина DATA, 16-бит big-endian (`00 01`, `00 02`, `00 04`, `00 05`) |
| CRC  | **CRC-8/MAXIM** (полином 0x31, LSB-first 0x8C, init 0x00, без xorout — семейство Dallas 1-Wire) |

CRC считается по всем байтам от SYNC до конца DATA включительно.

Валидность CRC на тестовых логах: 98,8–100 % кадров. Структура заголовка — ровно 4 байта (история: в ранней версии анализа предполагалось 5, проверка на всех логах дала 4).

### Кадры мастера

**Чтение регистра (LEN=1):**
```
A5 30 00 01 [reg] [CRC]
```

**Запись float32 (LEN=5), little-endian:**
```
A5 01 00 05 [reg][f0][f1][f2][f3] [CRC]
```

**Запись байта (LEN=2):**
```
A5 01 00 02 [reg][val] [CRC]
```

**Телеметрия пульта (LEN=5):**
```
A5 20 00 05 [idx][float32 LE] [CRC]
```
- idx `0x00` — температура датчика пульта (комната), °C
- idx `0x01` — всегда 0.0 (вероятно, второй датчик не подключён)

**Ответ слейва (LEN=1 или 4):**
```
A0 00 00 [LEN] [DATA] [CRC]
```

### Команды управления (шлются троекратно, интервал ~21 мс)

| Действие | Кадры |
|---|---|
| **Старт** | `A5 01 00 02 02 00 [CRC]` ×3 (reg 0x02 = 0), затем `A5 01 00 02 03 01 [CRC]` ×3 (reg 0x03 = 1) |
| **Стоп** | `A5 01 00 02 03 00 [CRC]` ×3 (reg 0x03 = 0) |
| **Мощность** | `A5 01 00 05 01 [float32 LE] [CRC]` — ступени **1000 / 1900 / 2800 / 3700 / 4600 Вт** (P1–P5) |

ВАЖНО: пульт **перезаписывает мощность (reg 0x01) каждым циклом опроса** своим текущим значением (подтверждено в двух логах). Поэтому чужая запись мощности бесполезна, если не перебивать её своей после каждой чужой записи («оверрайд»).

## 3. Опрос пульта (порядок регистров в цикле)

```
0x0B → 0x0C → 0x50 → 0x20 → 0x23 → 0x24 → 0x10 → 0x16
```
При подаче питания пульт дополнительно опрашивает: 0x01, 0x05, 0x03, 0x04, 0x0C, 0x10, 0x66 (восстановление сохранённых настроек; в reg 0x03 читалась последняя сохранённая мощность).

## 4. Таблица регистров (рабочие гипотезы, уверенность по данным)

| Рег | Тип (ответ) | Смысл | Уверенность |
|---|---|---|---|
| `0x01` | — | мощность, Вт (пишет мастер, float; 1000…4600) | **высокая** |
| `0x02` | байт | запись 0 перед стартом (назначение неизвестно) | низкая |
| `0x03` | байт | старт/стоп: 1 = старт, 0 = стоп | **высокая** |
| `0x04` | float | читается при подаче питания (1000,0) — вероятно, сохранённая мощность | низкая |
| `0x05` | float | 0,0219 → 0,0 между сессиями | низкая |
| `0x0B` | байт | код ошибки (0xFF = норма) | средняя |
| `0x0C` | u32 | номер шага программы горения (2→4→5→6→7→13; 13 = ровное горение). Ранння версия «уставка температуры» — снята | **высокая** |
| `0x10` | u32 | счётчик доз топлива (персистентный, живёт между сессиями); тикает только в активных фазах; темп зависит от мощности и теплового состояния | средняя |
| `0x16` | u32 | обороты вентилятора, об/мин: 2400 (P1) … 4400 (P5); меняется плавно, постоянная времени — десятки секунд | **высокая** |
| `0x20` | float | температура узла печки, °C (комната ~10–20; розжиг 50; горение 176–253) | **высокая** |
| `0x23` | u32 | всегда 0 | низкая |
| `0x24` | float | напряжение питания, В (13,0–13,8; просадка под свечой) | **высокая** |
| `0x50` | байт | машина состояний: 0x20 свеча → 0x24 розжиг → 0x25 пламя → 0x27 набор → 0x2F выход на режим → 0x2E установившееся горение | **высокая** |
| `0x66` | ? | всегда 0 | низкая |

Тайминг дозирования 0x10 (доз/с) по данным логов:

| Мощность | Темп |
|---|---|
| 1000 Вт | 1,72 … 2,29 (растёт по мере выхода на режим) |
| 1900 Вт | ~2,2 … 3,08 |
| 2800 Вт | ~2,4 … 3,72 |
| 3700 Вт | ~2,8 … 4,35 |
| 4600 Вт | ~3,8 … 4,6 |

Точная величина зависит от стадии разогрева: холодная печка качает чаще. В фазах свечи/розжига счётчик заморожен.

## 5. Машина состояний по логу полного пуска (мощность 5, суммарно ~430 с)

Все времена от записи «старт»:

| t, с | 0x50 | шаг 0x0C | Наблюдение на пульте / по регистрам |
|---|---|---|---|
| +0,2 | 0x20 | 2 | иконка свечи (прогрев свечи) |
| +3,1 | — | — | вентилятор 0 → ~1500 за ~1 с |
| +3,6 | 0x24 | 5 | свеча прогрелась, включился насос (щелчки), «пунктир» на экране |
| +8 | 0x25 | 6 | прогрев камеры, индикация контроля пламени |
| +98 | 0x27 | 7–8 | добавляет обороты, щелчки чаще |
| +128 | **0x2F** | 9–11 | вентилятор ползёт 1500 → 4400; свеча ещё горит (напряжение ~13,0 В) |
| +261 | 0x2E | 11 | свеча потухла (напряжение восстанавливается до 13,4), температура достигнута |
| +321 | 0x2E | 13 | активный режим работы |

## 6. Выключение

- Команда стоп — `reg 0x03 = 0` ×3.
- Пульт после стопа делает один последний опрос `0x0B` и **замолкает навсегда до следующего старта**. Продувка/выбег вентилятора по шине с родным пультом НЕ видны физически (мастер молчит — слейв молчит).
- Следствие: для наблюдения фазы остановки нужен активный мастер (ESP32), опрашивающий регистры после стопа — формат запросов известен.
- Печка принимает запросы на молчащей шине — подтверждено (в логах ответ слейва приходил на запрос, отправленный вне цикла пульта). Кадры команд после «усыпления» пульта — непроверенное допущение, проверяется вживую.

## 7. Электрический интерфейс (ESP32 ↔ шина)

- **RX (приём)**: делитель с синего провода: 10 кОм последовательно + 20 кОм на GND → ~3,3 В на GPIO.
- **TX (передача)**: НЕ втыкать пин в линию напрямую. Диод 1N4148: **анод к синему проводу, катод к TX**. Когда TX = 0 — диод открывается, тянет линию к ~0,7 В (честный лог.0 для 5-вольтовой логики); когда TX = 3,3 В — диод заперт, линия висит на подтяжке пульта. Физически исключает конфликт двух мастеров.
- GND — общий с пультом. Питание ESP32 — от +5 В шины (VIN) или свой DC-DC.
- На ESP32 UART настраивается с RX и TX на одном пине (штатная полудуплексная конфигурация) — транзистор направления не нужен.

## 8. Прошивка v0.1 (Arduino, один файл)

Зависимость: библиотека **WebSockets** (Markus Sattler). ElegantOTA — опционально, включается константой `ENABLE_OTA` (библиотека ayushsharma82/ElegantOTA; для неё выбрать в Arduino IDE схему разделов с крупным APP, например «No OTA (2MB APP/2MB SPIFFS)»).

Пины: RX GPIO16 (через делитель), TX GPIO17 (через диод).

```cpp
/* Autoheater ESP32 v0.1 — слушатель с правом голоса
 * Пульт остаётся мастером. ESP32 читает шину, показывает состояние в веб-интерфейсе
 * и в паузах врезает команды: пуск/стоп/мощность (с оверрайдом мощности).
 */
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#define ENABLE_OTA 0            // 1 = добавить библиотеку ElegantOTA и включить
#if ENABLE_OTA
  #include <ElegantOTA.h>
#endif

#define PIN_RX 16
#define PIN_TX 17
#define AP_SSID "Heater"
#define AP_PASS "heater123"
#define SILENT_POLL 1           // опрашивать печку, когда пульт замолчал (после стопа)

const float POWER_LEVELS[5] = {1000,1900,2800,3700,4600};

/* ---------- CRC-8/MAXIM (init 0x00, отражённый полином 0x8C) ---------- */
uint8_t crc8_maxim(const uint8_t *p, size_t n){
  uint8_t c=0;
  while(n--){ c^=*p++; for(uint8_t i=0;i<8;i++) c=(c&1)?(c>>1)^0x8C:(c>>1); }
  return c;
}

/* ---------- Построение кадров ---------- */
uint8_t makeRead(uint8_t reg, uint8_t *out){
  out[0]=0xA5; out[1]=0x30; out[2]=0x00; out[3]=0x01; out[4]=reg;
  out[5]=crc8_maxim(out,5); return 6;
}
uint8_t makeWriteByte(uint8_t reg, uint8_t val, uint8_t *out){
  out[0]=0xA5; out[1]=0x01; out[2]=0x00; out[3]=0x02; out[4]=reg; out[5]=val;
  out[6]=crc8_maxim(out,6); return 7;
}
uint8_t makeWriteFloat(uint8_t reg, float val, uint8_t *out){
  out[0]=0xA5; out[1]=0x01; out[2]=0x00; out[3]=0x05; out[4]=reg;
  memcpy(out+5,&val,4);
  out[9]=crc8_maxim(out,9); return 10;
}

/* ---------- Парсер ---------- */
struct Parsed{ uint8_t sync, cmd; uint16_t len; const uint8_t *data; bool crcOk; };
class Parser{
  uint8_t buf[64]; uint16_t n=0;
public:
  bool feed(uint8_t b, Parsed &out){
    if(n==0){ if(b==0xA5||b==0xA0) buf[n++]=b; return false; }
    buf[n++]=b;
    if(n<4) return false;
    uint16_t len=(buf[2]<<8)|buf[3];
    if(len>32){ n=0; return false; }                 // мусор — ресинк по SYNC
    uint16_t total=4+len+1;
    if(n<total) return false;
    out.sync=buf[0]; out.cmd=buf[1]; out.len=len; out.data=buf+4;
    out.crcOk=(crc8_maxim(buf,total-1)==buf[total-1]);
    n=0; return true;
  }
} parser;

/* ---------- Состояние для UI ---------- */
struct HeaterState{
  uint8_t state=0, step=0, err=0xFF;
  float nodeTemp=0, volt=0, panelTemp=0;
  uint32_t fan=0, doses=0;
  uint32_t lastSeen=0;
} H;
float lastPanelPower=-1, desiredPower=1000;
bool powerOverride=false, busEverSeen=false;
uint8_t pendingReg=0xFF;

float f32(const uint8_t *p){ float v; memcpy(&v,p,4); return v; }
uint32_t u32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

/* ---------- Очередь передач ---------- */
struct TxF{ uint8_t buf[16]; uint8_t len; };
TxF txQ[24]; uint8_t txHead=0, txTail=0;
uint32_t suppressUntil=0, lastRxMs=0, nextTxAt=0;

bool txEmpty(){ return txHead==txTail; }
bool txFull(){ return ((txHead+1)%24)==txTail; }
void pushTx(const uint8_t *f, uint8_t n){
  if(txFull()) return;
  memcpy(txQ[txHead].buf,f,n); txQ[txHead].len=n;
  txHead=(txHead+1)%24;
}
void enqByte(uint8_t reg, uint8_t val){ uint8_t f[8]; pushTx(f, makeWriteByte(reg,val,f)); }
void enqFloat(uint8_t reg, float val){ uint8_t f[12]; pushTx(f, makeWriteFloat(reg,val,f)); }

/* ---------- Разбор кадров ---------- */
void onFrame(const Parsed &f){
  if(f.sync==0xA5){
    if(f.cmd==0x30 && f.len==1) pendingReg=f.data[0];        // запрос — ждём ответ на этот регистр
    else if(f.cmd==0x01 && f.len==5 && f.data[0]==0x01){     // пульт записал мощность
      float v=f32(f.data+1); lastPanelPower=v;
      if(powerOverride){
        if(fabs(v-desiredPower)>0.5) enqFloat(0x01,desiredPower);  // перезаписал — вернём своё
        else powerOverride=false;
      }
    }
    else if(f.cmd==0x20 && f.len==5 && f.data[0]==0x00)
      H.panelTemp=f32(f.data+1);                             // телеметрия пульта
  } else {                                                   // слейв: печка
    H.lastSeen=millis(); busEverSeen=true;
    if(!f.crcOk) return;
    switch(pendingReg){
      case 0x0B: H.err    = f.data[0];              break;
      case 0x0C: H.step   = (uint8_t)u32(f.data);   break;
      case 0x50: H.state  = f.data[0];              break;
      case 0x20: H.nodeTemp = f32(f.data);          break;
      case 0x24: H.volt   = f32(f.data);            break;
      case 0x10: H.doses  = u32(f.data);            break;
      case 0x16: H.fan    = u32(f.data);            break;
    }
    pendingReg=0xFF;
  }
}

/* ---------- Врезка в паузы ---------- */
void injectTask(){
  if(txEmpty()) return;
  uint32_t now=millis();
  if(now<nextTxAt) return;
  if(now-lastRxMs<15) return;                       // окно: шина свободна >= 15 мс
  TxF &f=txQ[txTail];
  suppressUntil = now + (uint32_t)f.len*12 + 4;     // глушим эхо собственной передачи
  Serial1.write(f.buf, f.len);
  txTail=(txTail+1)%24;
  nextTxAt = millis()+21;                           // темп, как у троек пульта
}

void pumpRx(){
  while(Serial1.available()>0){
    uint8_t b=Serial1.read();
    if(millis()<suppressUntil) continue;            // своё эхо — не разбираем
    lastRxMs=millis();
    Parsed f;
    if(parser.feed(b,f)) onFrame(f);
  }
}

/* ---------- Команды ---------- */
void cmdStart(){
  for(int i=0;i<3;i++) enqByte(0x02,0x00);
  for(int i=0;i<3;i++) enqByte(0x03,0x01);
}
void cmdStop(){ for(int i=0;i<3;i++) enqByte(0x03,0x00); }
void cmdPower(int dir){
  float cur = powerOverride ? desiredPower : (lastPanelPower>0?lastPanelPower:1000);
  int i=0; float best=1e9;
  for(int k=0;k<5;k++){ float df=fabs(POWER_LEVELS[k]-cur); if(df<best){best=df;i=k;} }
  i=constrain(i+dir,0,4);
  desiredPower=POWER_LEVELS[i]; powerOverride=true;
  enqFloat(0x01,desiredPower);
}

/* ---------- Веб ---------- */
WebServer server(80);
WebSocketsServer ws(81);

const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Автономка</title><style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:0;padding:16px}
.card{background:#1c1c1c;border-radius:12px;max-width:420px;margin:0 auto 12px;padding:14px}
#st{font-size:1.5em;font-weight:bold} #err{color:#f66;min-height:1.2em}
.val{display:inline-block;margin:0 10px} .val b{display:block;font-size:1.3em;color:#7fd}
button{font-size:1.2em;padding:12px 18px;margin:4px;border:0;border-radius:10px;background:#333;color:#eee;width:44%}
#off{background:#a33} #dot{font-size:0.8em;color:#888}
</style></head><body>
<div class="card"><div id="dot">…</div><div id="st">…</div><div id="err"></div>
<div><span class="val"><b id="t">–</b>°C узел</span><span class="val"><b id="u">–</b>В</span></div>
<div><span class="val"><b id="f">–</b>об/мин</span><span class="val"><b id="p">–</b>Вт пульт</span></div>
</div>
<div class="card">
<button onclick="send('start')">Пуск</button><button id="off" onclick="send('stop')">Стоп</button>
<button onclick="send('pup')">Мощность +</button><button onclick="send('pdown')">Мощность −</button>
</div>
<script>
const ST={32:'Свеча (прогрев)',36:'Розжиг',37:'Пламя поймано',39:'Выход на режим',47:'Разгон',46:'Горение'};
let ws=new WebSocket('ws://'+location.hostname+':81');
ws.onmessage=e=>{ const d=JSON.parse(e.data);
  st.textContent=ST[d.st]||('Код 0x'+d.st.toString(16));
  err.textContent=d.err=='FF'?'':'Ошибка 0x'+d.err+' — проверьте отопитель';
  t.textContent=(+d.t).toFixed(1); u.textContent=(+d.v).toFixed(2);
  f.textContent=d.fan; p.textContent=d.pw;
  dot.textContent=d.live?'● связь есть':'○ связи с печкой нет';
};
function send(c){ ws.send(c); }
</script></body></html>)HTML";

void pushState(){
  char js[320];
  snprintf(js,sizeof(js),
    "{\"st\":%u,\"step\":%u,\"err\":\"%02X\",\"t\":%.1f,\"v\":%.2f,\"fan\":%lu,\"dose\":%lu,\"tp\":%.1f,\"pw\":%.0f,\"live\":%s}",
    H.state,H.step,H.err,H.nodeTemp,H.volt,(unsigned long)H.fan,(unsigned long)H.doses,
    H.panelTemp, lastPanelPower<0?0:lastPanelPower,
    (millis()-H.lastSeen<5000)?"true":"false");
  ws.broadcastTXT(js);
}

void wsEvent(uint8_t, WStype_t type, uint8_t *payload, size_t){
  if(type!=WStype_TEXT) return;
  String s((char*)payload);
  if(s=="start") cmdStart();
  else if(s=="stop") cmdStop();
  else if(s=="pup") cmdPower(+1);
  else if(s=="pdown") cmdPower(-1);
}

void setup(){
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
  WiFi.softAP(AP_SSID, AP_PASS);          // потом можно перейти на домашний Wi-Fi
  server.on("/", [](){ server.send_P(200,"text/html",PAGE); });
  server.begin(); ws.begin(); ws.onEvent(wsEvent);
#if ENABLE_OTA
  ElegantOTA.begin(&server);              // обновление: http://<ip>/update
#endif
  Serial.printf("AP %s, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop(){
  pumpRx();
  injectTask();
#if SILENT_POLL
  /* Пульт после стопа замолкает — держим шину живой своим опросом,
     иначе веб-интерфейс ослепнет и следующий пуск не увидим */
  static uint32_t lastSilent=0; static bool tog=false;
  if(busEverSeen && txEmpty() && millis()-lastRxMs>2000 && millis()-lastSilent>1000){
    lastSilent=millis(); tog=!tog;
    uint8_t f[8]; pushTx(f, makeRead(tog?0x50:0x24, f));
  }
#endif
  server.handleClient();
  ws.loop();
#if ENABLE_OTA
  ElegantOTA.loop();
#endif
  static uint32_t lastPush=0;
  if(millis()-lastPush>500){ lastPush=millis(); pushState(); }
}
```

### Как включить ElegantOTA

1. В Arduino IDE: Скетч → Подключить библиотеку → Менеджер библиотек → `ElegantOTA` (ayushsharma82).
2. В коде поставить `#define ENABLE_OTA 1`.
3. Инструменты → Partition Scheme → «No OTA (2MB APP/2MB SPIFFS)» (или схему, где APP ≥ 2 МБ).
4. После прошивки обновления по воздуху: `http://<IP>/update`.

## 9. Чек-лист первого пуска

1. **Только сниффер**: ничего не нажимать в веб-интерфейсе; запустить печку родным пультом и сверить показания веба с ожидаемыми (ступени, температура, обороты — по таблице выше). Проверка парсера вживую.
2. **Пуск через веб** на холодной печке, пульт включён: слушать щелчки насоса, смотреть 0x50.
3. **Мощность ±**: по оборотам 0x16 убедиться, что печка меняет режим и оверрайд держит значение.
4. **Стоп через веб**: пульт продолжит опрос — в вебе должны появиться коды продувки в 0x50 (бонус: заодно добудем фазу остановки).
5. **Пуск на молчащей шине** (пульт «уснул» после своего стопа): проверить, принимает ли печка команды без активного пульта.

## 10. Тонкие места прошивки (куда смотреть при отладке)

- Порог тишины 15 мс (окно врезки): при коллизиях поднять до 30 мс.
- Поведение печки на врезки вне цикла пульта — непроверено; логирование ответов покажет.
- Проверить, что печка принимает тройку «старт» без предшествующей записи `0x02 = 0`.
- Код ошибки 0x0B: отображается как «Ошибка — проверьте отопитель»; точные коды добавятся после логов с реальными ошибками.

## 11. Открытые вопросы / дальнейшие эксперименты

- Коды ошибок в 0x0B (нехватка топлива, срыв пламени) — нужен лог с реальной ошибкой.
- Назначение регистров 0x02, 0x04, 0x05, 0x23, 0x66.
- Напряжение 0x24 и логика просадки при работе свечи.
- Фаза остановки (продувка, коды 0x50) — только через ESP32-мастера после стопа.
- Проверка: принимает ли печка кадры на «мёртвой» шине (пульт молчит).

## 12. Ожидаемый индикационный словарь для UI (0x50)

| Код | Стадия |
|---|---|
| 0x20 | Свеча (прогрев) |
| 0x24 | Розжиг |
| 0x25 | Пламя поймано / прогрев камеры |
| 0x27 | Набор мощности (выход на режим) |
| 0x2F | Разгон (свеча ещё горит) |
| 0x2E | Установившееся горение |

Коды, не попавшие в словарь, показывать как «Код 0xXX» — по мере сбора логов пополнять.
