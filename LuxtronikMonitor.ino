#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include <math.h>

// ===== Farben (RGB565) =====
#ifndef BLACK
  #define BLACK 0x0000
#endif
#ifndef WHITE
  #define WHITE 0xFFFF
#endif

// =========================
// WLAN-KONFIGURATION
// =========================
const char* WIFI_SSID     = "SSID";
const char* WIFI_PASSWORD = "PWD";

// =========================
// Luxtronik / Controller
// =========================
const char*  CONTROLLER_IP       = "192.168.0.40";
const uint16_t CONTROLLER_PORT   = 8214;
const char*  WS_PROTOCOL         = "Lux_WS";
const char*  CONTROLLER_PASSWORD = "0";

// Update alle 3 Sekunden
const unsigned long REFRESH_INTERVAL_SEC = 3;
const bool RUN_ONCE = false;

// =========================
// Display Pins (Waveshare ESP32-C6-LCD-1.47)
// =========================
#define PIN_LCD_MOSI  6
#define PIN_LCD_SCLK  7
#define PIN_LCD_CS    14
#define PIN_LCD_DC    15
#define PIN_LCD_RST   21
#define PIN_LCD_BL    22

#define LCD_W         172
#define LCD_H         320
#define LCD_X_OFFSET  34
#define LCD_Y_OFFSET  0

// 90° Rotation: 3 ist häufig "richtig" (falls andersrum: 1 testen)
#define LCD_ROTATION  3

WebSocketsClient webSocket;

// Arduino_GFX setup (SPI Bus)
Arduino_DataBus* bus = new Arduino_ESP32SPI(
  PIN_LCD_DC, PIN_LCD_CS,
  PIN_LCD_SCLK, PIN_LCD_MOSI,
  -1 /* MISO */
);

// ST7789 with offsets
Arduino_GFX* gfx = new Arduino_ST7789(
  bus, PIN_LCD_RST, LCD_ROTATION,
  true /* invert */,
  LCD_W, LCD_H,
  LCD_X_OFFSET, LCD_Y_OFFSET
);

// =========================
// Status / Values
// =========================
bool loggedIn = false;
unsigned long lastRefreshMs = 0;

// ID des Navigationspunkts "Temperaturen"
String tempNavId = "";

// Temperaturen (letzte Werte)
float lastWarmWaterTemp = NAN;
float lastOutsideTemp   = NAN;
float lastVorlaufTemp   = NAN;

// NTP (optional, für Timestamp auf Display)
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;   // DE
const int   DAYLIGHT_OFFSET_SEC = 0;
unsigned long lastNtpSyncMs = 0;
const unsigned long NTP_SYNC_INTERVAL_MS = 3600000UL;

// =========================
// Helpers
// =========================
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    tries++;
    if (tries > 120) ESP.restart();
  }
}

void sendWS(const String& payload) {
  // WebSockets 2.7.1 erwartet String& (nicht const)
  String tmp = payload;
  webSocket.sendTXT(tmp);
}

String getRootTag(const String& xml) {
  int start = xml.indexOf('<');
  if (start < 0) return "";
  int end = xml.indexOf('>', start + 1);
  if (end < 0) return "";
  String tag = xml.substring(start + 1, end);

  if (tag.startsWith("?")) {
    int closePos = xml.indexOf("?>", start);
    if (closePos > start) return getRootTag(xml.substring(closePos + 2));
  }

  int spacePos = tag.indexOf(' ');
  if (spacePos > 0) tag = tag.substring(0, spacePos);

  tag.toUpperCase();
  if (tag.startsWith("?")) tag = tag.substring(1);
  return tag;
}

String extractAttribute(const String& block, const String& attr) {
  String marker = attr + "=";
  int pos = block.indexOf(marker);
  if (pos < 0) return "";
  pos += marker.length();
  if (pos >= (int)block.length()) return "";
  char quote = block.charAt(pos);
  if (quote != '"' && quote != '\'') return "";
  int valueStart = pos + 1;
  int valueEnd = block.indexOf(quote, valueStart);
  if (valueEnd < 0) return "";
  return block.substring(valueStart, valueEnd);
}

String extractTagValue(const String& block, const String& tag) {
  String openTag  = "<" + tag + ">";
  String closeTag = "</" + tag + ">";
  int start = block.indexOf(openTag);
  if (start < 0) return "";
  start += openTag.length();
  int end = block.indexOf(closeTag, start);
  if (end < 0) return "";
  String value = block.substring(start, end);
  value.trim();
  return value;
}

float parseTemperatureValue(String value) {
  value.replace(",", ".");
  int degreeIndex = value.indexOf("°");
  if (degreeIndex >= 0) value = value.substring(0, degreeIndex);
  value.trim();
  if (value.isEmpty()) return NAN;
  return value.toFloat();
}

// Suche in NAVIGATION nach Item "Temperaturen" und merke ID
void findTemperatureNavId(const String& xml) {
  tempNavId = "";

  int pos = 0;
  while (true) {
    int itemStart = xml.indexOf("<item", pos);
    if (itemStart < 0) break;

    int tagEnd = xml.indexOf('>', itemStart);
    if (tagEnd < 0) break;

    String itemOpenTag = xml.substring(itemStart, tagEnd + 1);
    String id = extractAttribute(itemOpenTag, "id");

    int nameStart = xml.indexOf("<name>", tagEnd);
    if (nameStart < 0) { pos = tagEnd + 1; continue; }
    int nameEnd = xml.indexOf("</name>", nameStart);
    if (nameEnd < 0) { pos = tagEnd + 1; continue; }

    String name = xml.substring(nameStart + 6, nameEnd);
    name.trim();

    String cmp = name; cmp.toLowerCase();
    if (cmp == "temperaturen") {
      tempNavId = id;
      break;
    }
    pos = tagEnd + 1;
  }
}

String fmt1(float v) {
  if (isnan(v)) return "--.-";
  char b[8];
  dtostrf(v, 4, 1, b);
  return String(b);
}

bool getLocalTimeInfo(struct tm &timeinfo) {
  return getLocalTime(&timeinfo, 100);
}

void syncTimeIfNeeded(bool force = false) {
  unsigned long now = millis();
  if (force || now - lastNtpSyncMs >= NTP_SYNC_INTERVAL_MS) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTimeInfo(timeinfo)) lastNtpSyncMs = now;
  }
}

// ===== Display =====
void renderDisplay() {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);

  // Header klein
  gfx->setTextSize(1);
  gfx->setCursor(0, 0);
  gfx->print("Luxtronik Temps  IP ");
  gfx->println(WiFi.localIP());

  struct tm t;
  gfx->setCursor(0, 12);
  if (getLocalTimeInfo(t)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%d.%m %H:%M:%S", &t);
    gfx->println(buf);
  } else {
    gfx->println("NTP...");
  }

  gfx->drawFastHLine(0, 26, gfx->width(), WHITE);

  // Werte groß
  int y = 34;
  gfx->setTextSize(2);

  gfx->setCursor(0, y);
  gfx->print("A: ");
  gfx->print(fmt1(lastOutsideTemp));
  gfx->println("C");

  y += 26;
  gfx->setCursor(0, y);
  gfx->print("WW:");
  gfx->print(fmt1(lastWarmWaterTemp));
  gfx->println("C");

  y += 26;
  gfx->setCursor(0, y);
  gfx->print("VL:");
  gfx->print(fmt1(lastVorlaufTemp));
  gfx->println("C");
}

// Parse nur die Temperatur-Seite und update die 3 Werte
void parseTemperaturesFromContent(const String& xml) {
  int pos = 0;

  while (true) {
    int itemStart = xml.indexOf("<item", pos);
    if (itemStart < 0) break;

    int itemEnd = xml.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;

    String block = xml.substring(itemStart, itemEnd + 7);
    String name  = extractTagValue(block, "name");
    String value = extractTagValue(block, "value");

    if (name.length() && value.length()) {
      String lowered = name;
      lowered.toLowerCase();
      lowered.replace(" ", "");
      lowered.replace("_", "");
      lowered.replace("-", "");

      // Luxtronik kann je nach Firmware leicht andere Namen liefern:
      // Außen: "Aussentemperatur", Warmwasser: "Warmwasser-Ist", Vorlauf: "Vorlauf"
      if (lowered.indexOf("aussentemperatur") >= 0 || lowered.indexOf("außentemperatur") >= 0) {
        float v = parseTemperatureValue(value);
        if (!isnan(v)) lastOutsideTemp = v;
      }

      if (lowered.indexOf("warmwasserist") >= 0 || lowered.indexOf("warmwasser-ist") >= 0 || lowered.indexOf("warmwasser") >= 0) {
        float v = parseTemperatureValue(value);
        if (!isnan(v)) lastWarmWaterTemp = v;
      }

      if (lowered == "vorlauf" || lowered.indexOf("vorlauf") >= 0) {
        float v = parseTemperatureValue(value);
        if (!isnan(v)) lastVorlaufTemp = v;
      }
    }

    pos = itemEnd + 7;
  }

  renderDisplay();
}

// =========================
// WebSocket Event
// =========================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      loggedIn = false;
      break;

    case WStype_CONNECTED: {
      // LOGIN
      String loginCmd = "LOGIN;";
      loginCmd += CONTROLLER_PASSWORD;
      sendWS(loginCmd);
      loggedIn = true;

      // NAV anfordern
      sendWS("REFRESH");
      lastRefreshMs = millis();
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload).substring(0, length);
      String root = getRootTag(msg);

      if (root == "NAVIGATION" || root == "NAV") {
        // finde die Seite "Temperaturen" und hole nur die
        findTemperatureNavId(msg);
        if (tempNavId.length() > 0) {
          sendWS("GET;" + tempNavId);
        }
      } else if (root == "CONTENT" || root == "VALUES") {
        // Temperatur-Seite auswerten
        parseTemperaturesFromContent(msg);
      }
      break;
    }

    default:
      break;
  }
}

// =========================
// setup & loop
// =========================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Backlight
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(0, 0);
  gfx->println("Luxtronik Temps...");
  gfx->println("WiFi connect...");

  connectWifi();
  syncTimeIfNeeded(true);

  renderDisplay();

  webSocket.begin(CONTROLLER_IP, CONTROLLER_PORT, "/", WS_PROTOCOL);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.setExtraHeaders("");
  webSocket.setAuthorization("", "");

  lastRefreshMs = millis();
}

void loop() {
  webSocket.loop();
  syncTimeIfNeeded();

  if (!loggedIn) return;

  unsigned long now = millis();
  if (!RUN_ONCE && (now - lastRefreshMs >= REFRESH_INTERVAL_SEC * 1000UL)) {
    // Alle 3 Sekunden: REFRESH -> NAVIGATION -> GET;tempNavId -> CONTENT
    sendWS("REFRESH");
    lastRefreshMs = now;
  }
}
