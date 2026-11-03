# Базовая версия Autoheater ESP32 — v0.0.0

> **Статус:** архивный снимок исходного скетча, предоставленного автором проекта 4 ноября 2026 года.
> **Назначение:** зафиксировать первую реализацию как неизменяемую точку отсчёта для сравнения с последующими версиями.
> **Важно:** код ниже сохранён в предоставленном виде. Это не текущая прошивка и не исходник для сборки актуального проекта.

## 1. Назначение

Первая версия Autoheater ESP32 задумывалась как дополнительный участник шины штатного пульта автономного отопителя:

- штатный пульт остаётся мастером и продолжает выполнять собственный цикл обмена;
- ESP32 принимает и разбирает кадры пульта и отопителя;
- ESP32 показывает телеметрию в локальном веб-интерфейсе;
- команды управления ставятся в очередь и передаются в паузах между сообщениями шины;
- после прекращения обмена со штатным пультом ESP32 может самостоятельно опрашивать часть регистров, сохраняя телеметрию доступной в веб-интерфейсе.

Это не полностью самостоятельный мастер шины: первоначальная логика рассчитана на работу рядом со штатным пультом, который остаётся основным инициатором обмена.

## 2. Аппаратная конфигурация

| Назначение | Значение |
|---|---|
| Контроллер | ESP32 |
| UART отопителя | \`Serial1\`, 9600 бод, 8N1 |
| RX | GPIO16, через делитель 10 кОм / 20 кОм |
| TX | GPIO17, через диод 1N4148 |
| Точка доступа Wi-Fi | \`Heater\` |
| Пароль точки доступа | \`heater123\` |
| HTTP | порт 80 |
| WebSocket | порт 81 |
| Тихий опрос | включён (\`SILENT_POLL 1\`) |

**Электрическое замечание из исходного комментария:** синяя линия шины подключается к RX через делитель, а TX подключается через 1N4148 (анод к линии). Общая земля обязательна. Линию шины нельзя подключать непосредственно к GPIO ESP32 без согласования уровней.

## 3. Программная архитектура и функционал

Вся реализация находится в одном Arduino-файле. Код не разделён на отдельные модули.

### 3.1. Формирование кадров и CRC

Используется CRC-8/MAXIM:

- начальное значение \`0x00\`;
- отражённый полином \`0x8C\` (соответствует полиному \`0x31\` при записи MSB-first);
- CRC вычисляется по всем байтам кадра, кроме самого CRC;
- дополнительный \`xorout\` не применяется.

Реализованы три функции формирования исходящих кадров:

- \`makeRead()\` — запрос чтения одного регистра;
- \`makeWriteByte()\` — запись одного байта в регистр;
- \`makeWriteFloat()\` — запись \`float\` в little-endian представлении.

Формат кадра: \`[SYNC][CMD][LEN_HI][LEN_LO][DATA][CRC]\`.

### 3.2. Приём и разбор UART

Класс \`Parser\` накапливает входящие байты в буфере размером 64 байта. Началом кадра считается \`0xA5\` или \`0xA0\`. После получения заголовка определяется длина поля данных; длина более 32 байт приводит к сбросу накопленного кадра.

После получения полного кадра формируется структура \`Parsed\`, содержащая SYNC, CMD, длину, указатель на данные и результат проверки CRC.

### 3.3. Модель состояния отопителя

Структура \`HeaterState H\` хранит:

- \`state\` — код состояния отопителя;
- \`step\` — шаг программы;
- \`err\` — код ошибки, начальное значение \`0xFF\`;
- \`nodeTemp\` — температура узла отопителя;
- \`volt\` — напряжение;
- \`panelTemp\` — температура, переданная штатным пультом;
- \`fan\` — обороты вентилятора;
- \`doses\` — счётчик доз топлива;
- \`lastSeen\` — время последнего принятого ответа отопителя.

Дополнительно хранятся последняя мощность, полученная от пульта (\`lastPanelPower\`), заданная ESP32 мощность (\`desiredPower\`), флаг переопределения мощности (\`powerOverride\`), признак обнаружения шины (\`busEverSeen\`) и ожидаемый регистр ответа (\`pendingReg\`).

### 3.4. Очередь передач и врезка в шину

Исходящие кадры помещаются в кольцевую очередь \`txQ\` на 24 слота. Если очередь заполнена, \`pushTx()\` просто отбрасывает новый кадр.

\`injectTask()\` отправляет не более одного кадра за вызов при выполнении условий:

- очередь не пуста;
- наступило время следующей передачи;
- после последнего принятого байта прошло не менее 15 мс.

После отправки выдерживается интервал около 21 мс до следующей передачи. Для подавления собственного эха используется временное окно \`suppressUntil\`, рассчитанное из длины отправленного кадра.

### 3.5. Обработка команд

Поддерживаются четыре команды веб-интерфейса:

| Команда WebSocket | Действие |
|---|---|
| \`start\` | Запуск отопителя |
| \`stop\` | Остановка отопителя |
| \`pup\` | Увеличение заданной мощности на одну ступень |
| \`pdown\` | Уменьшение заданной мощности на одну ступень |

Последовательность запуска:

1. Три записи \`0x02 = 0x00\`.
2. Затем три записи \`0x03 = 0x01\`.

Последовательность остановки: три записи \`0x03 = 0x00\`.

Доступные ступени мощности: 1000, 1900, 2800, 3700 и 4600 Вт. Мощность передаётся как \`float32 LE\` в регистр \`0x01\`.

Когда включён \`powerOverride\`, прошивка сравнивает значение мощности, переданное штатным пультом, с \`desiredPower\`. Если значения отличаются более чем на 0,5 Вт, ESP32 ставит в очередь повторную запись заданной мощности. Если значения совпадают, \`powerOverride\` отключается.

### 3.6. Тихий опрос

При \`SILENT_POLL 1\` тихий опрос включается только после того, как был обнаружен хотя бы один ответ отопителя (\`busEverSeen == true\`).

Если после последнего принятого байта прошло более 2 секунд, очередь передач пуста, а с предыдущего тихого опроса прошла минимум 1 секунда, ESP32 ставит запрос регистра в очередь.

Опрос чередует два регистра:

- \`0x50\` — состояние;
- \`0x24\` — напряжение.

В этой первой версии тихий опрос ограничен двумя регистрами.

### 3.7. Веб-интерфейс

Веб-страница хранится непосредственно в константе \`PAGE\` внутри скетча. HTTP-сервер отдаёт её по адресу \`/\`.

WebSocket-сервер работает на порту 81. Раз в 500 мс функция \`pushState()\` рассылает клиентам JSON с состоянием, шагом, кодом ошибки, температурой, напряжением, оборотами вентилятора, счётчиком доз, температурой пульта, мощностью и признаком связи.

Интерфейс показывает:

- наличие или отсутствие связи с отопителем;
- текстовое название известных состояний либо числовой код;
- температуру узла и напряжение;
- обороты вентилятора и мощность штатного пульта;
- код ошибки с текстом-подсказкой;
- кнопки пуска, остановки и изменения мощности.

### 3.8. Основной цикл

В \`loop()\` последовательно выполняются:

1. \`pumpRx()\` — чтение UART и разбор входящих кадров;
2. \`injectTask()\` — отправка одного ожидающего кадра при наличии свободного окна;
3. планирование тихого опроса, если он включён;
4. обслуживание HTTP-сервера;
5. обслуживание WebSocket;
6. периодическая рассылка состояния интерфейсу.

## 4. Известные ограничения исходной реализации

Этот раздел фиксирует свойства именно архивного кода, а не требования к текущей прошивке.

1. **CRC вычисляется, но проверяется не для всех кадров.** Для кадров \`0xA0\` проверка CRC выполняется в \`onFrame()\`, однако обработчики кадров \`0xA5\` не отбрасывают кадры с \`crcOk == false\`.
2. **Сопоставление ответов упрощено.** \`pendingReg\` устанавливается по запросу чтения от пульта. Ответ отопителя связывается с этим регистром без проверки команды ответа, ожидаемой длины, наличия актуального запроса или тайм-аута ожидания.
3. **Собственные запросы тихого опроса не устанавливают \`pendingReg\`.** Поэтому ответ на такой запрос может быть обработан с использованием предыдущего значения \`pendingReg\`.
4. **Подавление эха основано на временном окне.** На время \`suppressUntil\` отбрасываются все входящие байты. Если настоящий ответ отопителя попадёт в это окно, он также будет потерян.
5. **У парсера нет межбайтового тайм-аута.** Незавершённый кадр может оставаться в буфере до прихода новых байтов. При длине более 32 байт парсер сбрасывает состояние целиком.
6. **Переполнение очереди не сигнализируется.** \`pushTx()\` молча отбрасывает кадр, если очередь заполнена.
7. **Остановка не имеет отдельного приоритета.** Команды стопа добавляются в общую очередь так же, как остальные записи.
8. **Тихий опрос ограничен регистрами \`0x50\` и \`0x24\`.**
9. **Веб-интерфейс и прошивка тесно связаны.** HTML, сетевой сервер, парсер, управление и телеметрия находятся в одном файле.
10. **Нет OTA и OLED.** В исходном коде этой версии не реализованы ElegantOTA и вывод на OLED.
11. **Нет подтверждения аппаратными испытаниями в рамках этого документа.** Наличие кода и его описание не доказывают корректную работу на конкретном отопителе.

Эти ограничения перечислены для понимания эволюции проекта. Они не означают, что их следует переносить в актуальную прошивку.

## 5. Правила использования архивного снимка

- \`docs/base_code.md\` — историческая точка отсчёта, а не актуальная спецификация протокола и не исходник для сборки.
- Код в разделе 6 должен оставаться точной копией предоставленного исходного текста.
- Исправления, новые функции и изменения текущей прошивки вносятся в актуальные исходные файлы, а не в архивный листинг.
- Если потребуется сохранить другую исходную версию, для неё создаётся отдельный явно обозначенный архивный документ или тег Git.
- При сравнении последующих версий следует отделять изменение поведения от исправления ошибок первоначальной реализации.

---

## 6. Полный исходный код первой версии

Ниже приведён исходный код, предоставленный автором. Содержимое блока не редактировалось.

\`\`\`cpp
/* Autoheater ESP32 v0.0.0— слушатель с правом голоса
 * Пульт остаётся мастером. ESP32 читает шину, показывает состояние в веб-интерфейсе
 * и в паузах врезает команды: пуск/стоп/мощность.
 * Синий провод: RX GPIO16 через делитель 10к/20к; TX GPIO17 через 1N4148 (анод к линии).
 */
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#define PIN_RX 16
#define PIN_TX 17
#define AP_SSID "Heater"
#define AP_PASS "heater123"
#define SILENT_POLL 1          // сам опрашивать печку, когда пульт замолчал

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
      H.panelTemp=f32(f.data+1);                             // телеметрия пульта (датчик комнаты)
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
    if(millis()<suppressUntil) continue;            // это наше эхо — не разбирам
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
  WiFi.softAP(AP_SSID, AP_PASS);          // потом перейдёшь на домашний Wi-Fi
  server.on("/", [](){ server.send_P(200,"text/html",PAGE); });
  server.begin(); ws.begin(); ws.onEvent(wsEvent);
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
  static uint32_t lastPush=0;
  if(millis()-lastPush>500){ lastPush=millis(); pushState(); }
}
\`\`\`
