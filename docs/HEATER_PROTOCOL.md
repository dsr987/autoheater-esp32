# HEATER_PROTOCOL.md — протокол шины автономного отопителя (реверс)

Канонический документ проекта Autoheater ESP32. Статус: структура кадра, CRC и
основные команды подтверждены на 6 логах (~10 000 кадров). Семантика регистров —
рабочие гипотезы с указанием уверенности.

## 1. Физический уровень

- Полудуплексный UART по одному проводу, **9600 бод, 8N1**.
  Подтверждения: бит 104 мкс; межбайтовая пауза 1,032 мс = 10 бит; декодирование
  без framing errors; CRC сходится.
- Мастер — пульт (SYNC 0xA5), слейв — плата отопителя (SYNC 0xA0).
- Цикл опроса пульта: ~1,12-1,14 с (8 регистров подряд + телеметрия).
- Ответ слейва: через ~8-14 мс после запроса.
- Пауза мастера между кадрами: ~60-100 мс (окно для врезки).
- Записи пульт шлёт **троекратно** с интервалом ~21 мс.

## 2. Формат кадра

```
[SYNC][CMD][LEN_HI][LEN_LO][DATA x LEN][CRC8]
```

| Поле | Значение |
|---|---|
| SYNC | 0xA5 пульт, 0xA0 печка |
| CMD | 0x30 чтение регистра, 0x01 запись, 0x20 телеметрия пульта, 0x00 ответ слейва |
| LEN | длина DATA, 16 бит big-endian (00 01 / 00 02 / 00 04 / 00 05) |
| CRC | CRC-8/MAXIM: полином 0x31 (LSB-first 0x8C), init 0x00, без xorout; считается по байтам SYNC..конец DATA |

Кадры:
- чтение:   `A5 30 00 01 [reg] [CRC]`
- ответ:    `A0 00 00 XX [data] [CRC]` (XX = 01 или 04)
- запись:   `A5 01 00 05 [reg][float32 LE] [CRC]` (мощность) или
            `A5 01 00 02 [reg][байт] [CRC]` (старт/стоп)
- телеметрия пульта: `A5 20 00 05 [idx][float32 LE] [CRC]`, idx 0x00 = датчик пульта (°C), idx 0x01 = второй параметр (всегда 0.0)

## 3. Команды управления

- **Старт**: `0x02 = 00` x3, затем `0x03 = 01` x3
- **Стоп**:  `0x03 = 00` x3
- **Мощность**: запись `0x01`, float32 LE, ватты; ступени 1000/1900/2800/3700/4600
- **Важно**: пульт перезаписывает мощность (0x01) **каждый цикл опроса**, даже без
  нажатий. Врезка ESP32 с другим значением будет перезатираться — нужен оверрайд
  (своё значение догоняющим кадром после каждой чужой записи) или кнопки пульта.
- После стопа пульт делает один последний опрос 0x0B и замолкает: шина мертва,
  фаза продувки по ней не видна. Обойти можно только тихим опросом ESP32-мастера.

## 4. Регистры

| Рег | Формат | Наблюдаемое | Смысл (гипотеза) | Уверенность |
|---|---|---|---|---|
| 0x50 | u8 | 0x20/0x24/0x25/0x27/0x2F/0x2E | машина состояний | высокая |
| 0x0C | u32 | 2,4,5,6,7,9-11,13 | номер шага программы | высокая |
| 0x24 | f32 | 13,0-13,8 В | напряжение питания | высокая |
| 0x20 | f32 | 38..253 °C | температура узла печки | высокая |
| 0x16 | u32 | 0..4400 | обороты вентилятора | высокая |
| 0x10 | u32 | персистентный, тикает в рабочих фазах | счётчик доз топлива | средняя |
| 0x0B | u8 | 0xFF | код ошибки (FF = норма) | средняя |
| 0x03 | - | 0/1 | пуск/стоп (запись) | высокая |
| 0x01 | - | ватты | мощность (запись) | высокая |
| 0x02, 0x04, 0x05, 0x23, 0x66 | ? | 0/0,0219 | неизвестно | низкая |

Поправка: ранняя гипотеза «0x0C = уставка температуры» снята логом пуска.

## 5. Машина состояний (по логу полного пуска, мощность 5)

| 0x50 | Шаг | Фаза | Длительность |
|---|---|---|---|
| 0x20 | 2 | свеча/прогрев | ~3 с |
| 0x24 | 4-5 | розжиг: свеча+насос, вентилятор до ~2000 | ~90 с |
| 0x25 | 6 | пламя поймано, контроль горения | ~30 с |
| 0x27 | 7-8 | набор оборотов, частота доз растёт | ~30 с |
| 0x2F | 9-11 | выход на режим: вентилятор 1500->4400, свеча ещё работает | ~130 с |
| 0x2E | 13 | установившееся горение | до стопа |

- 0x10 (дозы) начинает тикать только с фазы 0x27.
- Напряжение проседает до ~13,0 В пока свеча работает; восстановление = свеча потухла.

## 6. Темп дозирования (0x10) по ступеням

| Мощность | Доз/с | Примечание |
|---|---|---|
| 1000 Вт | 1,7-2,3 | установившийся режим |
| 1900 Вт | 2,2-3,1 | |
| 2800 Вт | 2,4-3,7 | |
| 3700 Вт | 2,8-4,35 | |
| 4600 Вт | 4,4-4,6 | |

Вентилятор (0x16): 2400 на мощности 1, 4400 на мощности 5; переход занимает
десятки секунд (постоянная времени большая — короткие логи ловят только разгон).

## 7. Электрика подключения ESP32

- RX: делитель на синем проводе — 10 кОм последовательно, 20 кОм на GND
  (даёт 3,33 В на GPIO).
- TX: **только через диод 1N4148** (анод к линии, катод к TX ESP32). Когда TX=0,
  диод тянет линию к ~0,7 В; при TX=3,3 В диод заперт и линия висит на родной
  подтяжке 5 В. Так ESP32 физически не может пережать пульт/печку.
- GND общий. Питание ESP32 — от +5 В печки через VIN или свой DC-DC.
- UART ESP32: один пин на TX+RX (полудуплекс) или раздельные пины; эхо своей
  передачи глушить по времени (в коде — suppressUntil).

## 8. Прошивка v0.1 (Arduino)

> Этот раздел содержит первоначальный одnofайловый прототип протокола. Основная интегрированная прошивка находится в firmware/app/app.ino: она сохраняет описанные ниже кадры, CRC, регистры и команды, но использует проектный стек ESPAsyncWebServer + AsyncWebSocket и текущие GPIO из раздела 7. Код прототипа не нужно копировать поверх основного firmware.

Зависимости: WiFi (core), WebServer (core), WebSocketsServer (библиотека
WebSockets, Markus Sattler). Для OTA: библиотека ElegantOTA и схема разделов
с 2 МБ APP (Tools -> Partition Scheme -> "No OTA (2MB APP/2MB SPIFFS)" или
"Minimal SPIFFS (1.9MB APP...)" — главное, чтобы хватило места под два раздела).

Флаг `ENABLE_OTA`: 0 = только Wi-Fi AP и веб, 1 = включить ElegantOTA
(обновление по `http://<IP>/update`).

```cpp
/* Autoheater ESP32 v0.1 — сниффер с правом голоса
 * Пульт остаётся мастером. ESP32 читает шину, держит веб-интерфейс (AP Heater),
 * в паузах врезает команды: пуск/стоп/мощность (с оверрайдом против перезаписи).
 * Синий провод: RX через делитель 10к/20к; TX через 1N4148 (анод к линии).
 */
#define ENABLE_OTA 0          // 1 = включить ElegantOTA (нужна библиотека)

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#if ENABLE_OTA
  #include <ElegantOTA.h>
#endif

#define PIN_RX 16
#define PIN_TX 17
#define AP_SSID "Heater"
#define AP_PASS "heater123"

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
    if(f.cmd==0x30 && f.len==1) pendingReg=f.data[0];
    else if(f.cmd==0x01 && f.len==5 && f.data[0]==0x01){
      float v=f32(f.data+1); lastPanelPower=v;
      if(powerOverride){
        if(fabs(v-desiredPower)>0.5) enqFloat(0x01,desiredPower);  // перезаписал — вернём своё
        else powerOverride=false;
      }
    }
    else if(f.cmd==0x20 && f.len==5 && f.data[0]==0x00)
      H.panelTemp=f32(f.data+1);
  } else {
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
  nextTxAt = millis()+21;
}

void pumpRx(){
  while(Serial1.available()>0){
    uint8_t b=Serial1.read();
    if(millis()<suppressUntil) continue;            // эхо не разбираем
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
  WiFi.softAP(AP_SSID, AP_PASS);          // потом переход на домашний Wi-Fi
  server.on("/", [](){ server.send_P(200,"text/html",PAGE); });
  server.begin(); ws.begin(); ws.onEvent(wsEvent);
#if ENABLE_OTA
  ElegantOTA.begin(&server);              // обновление: http://<IP>/update
#endif
  Serial.printf("AP %s, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop(){
  pumpRx();
  injectTask();
  /* Тихий опрос: пульт после стопа замолкает — держим шину живой,
     иначе дашборд ослепнет. Меняет 0x50 / 0x24 раз в секунду. */
  static uint32_t lastSilent=0; static bool tog=false;
  if(busEverSeen && txEmpty() && millis()-lastRxMs>2000 && millis()-lastSilent>1000){
    lastSilent=millis(); tog=!tog;
    uint8_t f[8]; pushTx(f, makeRead(tog?0x50:0x24, f));
  }
  server.handleClient();
  ws.loop();
#if ENABLE_OTA
  ElegantOTA.loop();
#endif
  static uint32_t lastPush=0;
  if(millis()-lastPush>500){ lastPush=millis(); pushState(); }
}
```

## 9. Чек-лист первого пуска

1. **Только сниффер.** Ничего не нажимать в вебе; запустить печку родным пультом
   и сверить показания дашборда с поведением печки (ступени, температура, обороты).
2. **Пуск через веб** на холодной печке при включённом пульте. Слушать щелчки
   насоса, следить за 0x50.
3. **Мощность +/-**: менять в вебе, сверять обороты 0x16; проверить оверрайд.
4. **Стоп через веб**: пульт продолжит опрос — увидим коды продувки в 0x50
   (бонус: закроем недостающую фазу остановки).
5. **Пуск на молчащей шине** (после родного стопа): проверить, принимает ли
   печка команды от нас, когда пульт уснул. Непроверенное допущение.

## 10. Открытые вопросы

- Что за регистры 0x02/0x04/0x05/0x23/0x66.
- Сброс счётчика 0x10 между сессиями (3 разных несмонотонных значения).
- Словарь ошибок 0x0B (ждём лог с ошибкой).
- Фаза продувки: только через ESP32-мастер после стопа.
- Приемлемость врезок вне цикла пульта (проверяется пуском №2-3).
