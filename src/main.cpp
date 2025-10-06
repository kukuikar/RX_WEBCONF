// ===== RX for ESP32 (HC-12 + 13 реле, сервер конфигурации имя→GPIO) =====
// Совместим с Arduino-ESP32 (PlatformIO). Проверено без конкатенаций F()+String.
//
// -------------------- Инклуды --------------------
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// -------------------- Настройки HC-12 --------------------
#define HC12_BAUD   9600

// ВНИМАНИЕ: GPIO4 и GPIO5 — страп-ножки на многих ESP32. Если AP/загрузка чудят,
// переназначьте, например, RX=16, TX=17 (и перекиньте проводку).
static const int HC12_RX_PIN = 4;
static const int HC12_TX_PIN = 5;
HardwareSerial &HC12 = Serial2;

// -------------------- Реле --------------------
static const uint8_t RELAY_COUNT = 13;

// Порядок и имена реле
static const char keyOrder[RELAY_COUNT + 1] = "CBADEHGFZMOKT";

// LOW-активные модули реле (типовые на оптоканалах)
static const uint8_t RELAY_ACTIVE_LEVEL   = LOW;
static const uint8_t RELAY_INACTIVE_LEVEL = HIGH;

// Текущее сопоставление "индекс реле -> GPIO". Меняется через веб-страницу.
static uint8_t RELAY_PINS[RELAY_COUNT] = {
  13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 16
};

// Белый список разрешённых GPIO для выбора в конфиге (исключили HC-12 RX/TX)
static const uint8_t ALLOWED_PINS[] = {
  13, 14, 16, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
};

// -------------------- Индикация --------------------
static const int8_t LED_PIN = 2;     // встроенный синий/зелёный (на большинстве плат)
static const bool   LED_ACTIVE_HIGH = true;

// -------------------- Тайминги --------------------
static const uint32_t RELAY_TIMEOUT_MS = 500;

// -------------------- Wi-Fi / HTTP --------------------
WebServer server(80);
Preferences prefs;

static const char* AP_SSID = "ESP32-RX";
static const char* AP_PASS = "12345678"; // ≥8 символов; при проблемах будет фолбэк на открытый AP

// -------------------- Внутреннее состояние --------------------
static char rxBuf[24];
static size_t rxIdx = 0;
static uint32_t lastCmdMs = 0;
static uint16_t lastMask = 0;

// -------------------- Прототипы --------------------
void applyRelayMask(uint16_t mask);
void allRelaysOff();
void applyPinModesFromConfig();
bool isAllowedPin(uint8_t p);
bool hasDuplicates(const uint8_t *pins, uint8_t n);
void savePinsToPrefs(const uint8_t *pins);
void loadPinsFromPrefs();
bool startAP(const char* ssid, const char* pass);
void startHttp();

void handleRoot();
void handleSave();
void handleJson();
void handleReboot();

// -------------------- Утилиты --------------------
static inline void setLed(bool on) {
  if (LED_PIN < 0) return;
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW  : HIGH));
}

static inline void writeRelay(uint8_t pin, bool on) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, on ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
}

void applyRelayMask(uint16_t mask) {
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    writeRelay(RELAY_PINS[i], (mask >> i) & 0x1);
  }
}

void allRelaysOff() {
  applyRelayMask(0);
}

void applyPinModesFromConfig() {
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_INACTIVE_LEVEL);
  }
}

bool isAllowedPin(uint8_t p) {
  for (size_t i = 0; i < sizeof(ALLOWED_PINS); i++) {
    if (ALLOWED_PINS[i] == p) return true;
  }
  return false;
}

bool hasDuplicates(const uint8_t *pins, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    for (uint8_t j = i + 1; j < n; j++) {
      if (pins[i] == pins[j]) return true;
    }
  }
  return false;
}

// -------------------- Preferences --------------------
void loadPinsFromPrefs() {
  prefs.begin("cfg", true);
  bool changed = false;

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String keyIdx = String("p") + String(i);
    String keyChr = String("k_") + String(keyOrder[i]);

    int stored = prefs.getUChar(keyIdx.c_str(), 255);
    if (stored == 255) stored = prefs.getUChar(keyChr.c_str(), 255);

    if (stored != 255 && isAllowedPin((uint8_t)stored)) {
      if (RELAY_PINS[i] != (uint8_t)stored) {
        RELAY_PINS[i] = (uint8_t)stored;
        changed = true;
      }
    } else {
      // Запишем дефолт в NVS для наглядности
      prefs.end(); prefs.begin("cfg", false);
      prefs.putUChar(keyIdx.c_str(), RELAY_PINS[i]);
      prefs.putUChar(keyChr.c_str(), RELAY_PINS[i]);
      prefs.end(); prefs.begin("cfg", true);
    }
  }

  prefs.end();
  if (changed) applyPinModesFromConfig();
}

void savePinsToPrefs(const uint8_t *pins) {
  prefs.begin("cfg", false);
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String keyIdx = String("p") + String(i);
    String keyChr = String("k_") + String(keyOrder[i]);
    prefs.putUChar(keyIdx.c_str(), pins[i]);
    prefs.putUChar(keyChr.c_str(), pins[i]);
  }
  prefs.end();
}

// -------------------- HTML --------------------
String htmlHeader() {
  return String(
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>ESP32-RX Config</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;padding:16px;}"
    "table{border-collapse:collapse;width:100%;max-width:820px}"
    "th,td{border:1px solid #ccc;padding:8px;text-align:left}"
    "th{background:#f5f5f5}"
    "button{padding:8px 14px;border:0;border-radius:8px;cursor:pointer}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}"
    ".ok{color:green}.warn{color:#a66}.muted{color:#666}"
    "</style></head><body><h1>ESP32-RX — конфигурация GPIO</h1>"
  );
}

String htmlFooter() {
  return String(
    "<div class='muted' style='margin-top:16px'>"
    "HC-12: RX=") + String(HC12_RX_PIN) + ", TX=" + String(HC12_TX_PIN) +
    ". Эти GPIO исключены из выбора."
    "</div></body></html>";
}

String optionsForPins(uint8_t current) {
  String s;
  for (size_t i = 0; i < sizeof(ALLOWED_PINS); i++) {
    uint8_t p = ALLOWED_PINS[i];
    s += "<option value='";
    s += String(p);
    s += "'";
    if (p == current) s += " selected";
    s += ">GPIO ";
    s += String(p);
    s += "</option>";
  }
  return s;
}

// -------------------- HTTP Handlers --------------------
void handleRoot() {
  String page = htmlHeader();
  page += "<form method='POST' action='/save'>";
  page += "<table><tr><th>#</th><th>Имя</th><th>GPIO</th></tr>";

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    page += "<tr><td>";
    page += String(i);
    page += "</td><td><b>";
    page += String(keyOrder[i]);
    page += "</b></td><td><select name='pin_";
    page += String(i);
    page += "'>";
    page += optionsForPins(RELAY_PINS[i]);
    page += "</select></td></tr>";
  }
  page += "</table>";

  page += "<div class='row'>"
          "<button type='submit' style='background:#0a7d0a;color:#fff'>Сохранить</button>"
          "<a href='/reboot'><button type='button'>Перезагрузить</button></a>"
          "<a href='/api/config'><button type='button'>JSON</button></a>"
          "</div></form>";

  page += "<h3>Текущее состояние</h3><pre>";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    page += String(keyOrder[i]); page += " => GPIO ";
    page += String(RELAY_PINS[i]); page += "\n";
  }
  page += "</pre>";
  page += htmlFooter();

  server.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  uint8_t newPins[RELAY_COUNT];
  bool ok = true;

  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    String name = String("pin_") + String(i);
    if (!server.hasArg(name)) { ok = false; break; }
    int val = server.arg(name).toInt();
    if (!isAllowedPin((uint8_t)val)) { ok = false; break; }
    newPins[i] = (uint8_t)val;
  }

  if (!ok) {
    server.send(400, "text/plain; charset=utf-8", "Некорректные данные");
    return;
  }

  if (hasDuplicates(newPins, RELAY_COUNT)) {
    server.send(409, "text/plain; charset=utf-8",
                "Конфликт: один и тот же GPIO выбран для нескольких реле.");
    return;
  }

  // Применяем
  allRelaysOff();
  for (uint8_t i = 0; i < RELAY_COUNT; i++) RELAY_PINS[i] = newPins[i];
  savePinsToPrefs(RELAY_PINS);
  applyPinModesFromConfig();

  String okPage = htmlHeader();
  okPage += "<p class='ok'>Сохранено.</p><p><a href='/'>Назад</a></p>";
  okPage += htmlFooter();
  server.send(200, "text/html; charset=utf-8", okPage);
}

void handleJson() {
  String j = "{\n  \"relays\": [\n";
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    j += "    {\"index\": ";
    j += String(i);
    j += ", \"name\": \"";
    j += String(keyOrder[i]);
    j += "\", \"gpio\": ";
    j += String(RELAY_PINS[i]);
    j += "}";
    if (i != RELAY_COUNT - 1) j += ",\n";
    else j += "\n";
  }
  j += "  ]\n}";
  server.send(200, "application/json; charset=utf-8", j);
}

void handleReboot() {
  server.send(200, "text/plain; charset=utf-8", "Перезагрузка...");
  delay(200);
  ESP.restart();
}

// -------------------- AP + HTTP --------------------
bool startAP(const char* ssid, const char* pass) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  bool cfgOk = WiFi.softAPConfig(IPAddress(192,168,4,1),
                                 IPAddress(192,168,4,1),
                                 IPAddress(255,255,255,0));
  Serial.print("[WiFi] softAPConfig: "); Serial.println(cfgOk ? "OK" : "FAIL");

  for (int i = 0; i < 3; ++i) {
    bool ok = WiFi.softAP(ssid, pass);
    Serial.print("[WiFi] softAP("); Serial.print(ssid); Serial.print("): ");
    Serial.println(ok ? "OK" : "FAIL");
    if (ok) {
      delay(50);
      Serial.print("[WiFi] AP IP: ");
      Serial.println(WiFi.softAPIP().toString());
      return true;
    }
    delay(300);
  }
  return false;
}

void startHttp() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/config", HTTP_GET, handleJson);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.onNotFound([]{
    server.send(404, "text/plain; charset=utf-8", "404 Not Found");
  });

  server.begin();
  Serial.println("[HTTP] server.begin(): OK");
}

// -------------------- Парсер команд HC-12 --------------------
bool tryParseAndApply(const char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (line[0] != 'K' || line[1] != ':') return false;

  uint16_t mask = (uint16_t)strtol(line + 2, nullptr, 16) & 0x1FFF; // 13 бит
  applyRelayMask(mask);
  lastMask  = mask;
  lastCmdMs = millis();
  setLed(mask != 0);

  // Отладка в Serial
  Serial.print("Mask: 0x"); Serial.print(mask, HEX);
  Serial.print("  Active: ");
  bool any = false;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    if ((mask >> i) & 1) {
      any = true; Serial.print(keyOrder[i]); Serial.print(' ');
    }
  }
  if (!any) Serial.print("none");
  Serial.println();
  return true;
}

// -------------------- setup / loop --------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  // LED
  if (LED_PIN >= 0) { pinMode(LED_PIN, OUTPUT); setLed(false); }

  // Wi-Fi AP
  bool ap = startAP(AP_SSID, AP_PASS);
  if (!ap) {
    Serial.println("[WiFi] Пароль/регион мешают? Пробую открытый AP...");
    ap = startAP(AP_SSID, nullptr);
  }
  if (ap) {
    startHttp();
    Serial.print("Open http://");
    Serial.print(WiFi.softAPIP().toString());
    Serial.println(" for config");
  } else {
    Serial.println("[WiFi] AP failed — HTTP отключён");
  }

  // HC-12
  HC12.begin(HC12_BAUD, SERIAL_8N1, HC12_RX_PIN, HC12_TX_PIN);

  // Конфигурация реле
  loadPinsFromPrefs();
  applyPinModesFromConfig();
  allRelaysOff();

  rxIdx = 0;
  lastCmdMs = millis();
  lastMask = 0;

  Serial.println("[BOOT] RX ESP32 ready");
  Serial.print("Relay order: "); Serial.println(keyOrder);
  Serial.print("Relay pins: ");
  for (uint8_t i = 0; i < RELAY_COUNT; i++) {
    Serial.print(RELAY_PINS[i]); Serial.print(i == RELAY_COUNT - 1 ? '\n' : ',');
  }
}

void loop() {
  // HTTP
  server.handleClient();

  // Приём из HC-12 построчно
  while (HC12.available()) {
    char c = (char)HC12.read();
    if (c == '\n') {
      rxBuf[rxIdx] = '\0';
      tryParseAndApply(rxBuf);
      rxIdx = 0;
    } else if (c != '\r') {
      if (rxIdx < sizeof(rxBuf) - 1) rxIdx++; else rxIdx = 0;
      rxBuf[rxIdx - 1] = c;
    }
  }

  // Таймаут — выключить всё
  uint32_t now = millis();
  if (now - lastCmdMs > RELAY_TIMEOUT_MS) {
    if (lastMask != 0) {
      allRelaysOff();
      setLed(false);
      lastMask = 0;
    }
    lastCmdMs = now;
  }
}
