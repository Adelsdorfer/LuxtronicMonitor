// Luxtronik Monitor für ESP32-S3 (Arduino)
// Ausgabe: Nur Temperaturwerte (Name: Wert), sonst nichts Relevantes.
//
// Voraussetzungen:
//   - Board: ESP32-S3
//   - Libraries: WiFi, WebSockets (arduinoWebSockets von Markus Sattler)

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <math.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// =========================
// WLAN-KONFIGURATION
// =========================
const char* WIFI_SSID     = "cisco2";
const char* WIFI_PASSWORD = "Roli2010!+";

// =========================
// LUXTRONIK-KONFIGURATION
// =========================
const char*   CONTROLLER_IP       = "192.168.0.40";
const uint16_t CONTROLLER_PORT    = 8214;
const char*   WS_PROTOCOL         = "Lux_WS";
const char*   CONTROLLER_PASSWORD = "0";   // Luxtronik Passwort wie im Webinterface

// Refresh-Intervall in Sekunden (wird bei RUN_ONCE nicht verwendet)
const unsigned long REFRESH_INTERVAL_SEC = 5;

// Nur einen kompletten Durchlauf durchführen?
const bool RUN_ONCE = false;

// =========================
// Globale Variablen
// =========================

WebSocketsClient webSocket;

bool cycleComplete      = false;
unsigned long lastRefreshMs = 0;
bool loggedIn           = false;
float lastWarmWaterTemp = NAN;
float lastOutsideTemp   = NAN;
float lastVorlaufTemp   = NAN;

// ID des Navigationspunkts "Temperaturen"
String tempNavId = "";

enum LedMode {
  LED_MODE_GREEN = 0,
  LED_MODE_RED,
  LED_MODE_RED_BLINK,
  LED_MODE_ORANGE
};

LedMode requestedLedMode = LED_MODE_GREEN;
LedMode appliedLedMode   = LED_MODE_GREEN;
bool ledBlinkOn          = false;
unsigned long lastLedBlinkMs = 0;
const unsigned long LED_BLINK_INTERVAL_MS = 500;

const uint8_t STATUS_LED_PIN = 48;  // eingebaute NeoPixel-LED des ESP32-S3 DevKit

#if defined(NEOPIXEL_POWER_PIN)
const int STATUS_LED_POWER_PIN = NEOPIXEL_POWER_PIN;
#elif defined(PIN_NEOPIXEL_POWER)
const int STATUS_LED_POWER_PIN = PIN_NEOPIXEL_POWER;
#else
const int STATUS_LED_POWER_PIN = 46;  // standard bei vielen ESP32-S3-Devkits
#endif
Adafruit_NeoPixel statusPixel(1, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

// Display
const int SCREEN_WIDTH  = 128;
const int SCREEN_HEIGHT = 64;
const int OLED_RESET_PIN = -1;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);
bool displayReady = false;
bool displayDirty = true;

// Zeit / Anzeige
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;
unsigned long lastNtpSyncMs = 0;
const unsigned long NTP_SYNC_INTERVAL_MS = 3600000UL;
unsigned long lastDisplayUpdateMs = 0;

// =========================
// Hilfsfunktionen
// =========================

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    tries++;
    if (tries > 60) {   // nach ca. 30 Sekunden neu versuchen
      ESP.restart();
    }
  }

  // Nach erfolgreicher Verbindung keine weiteren Debug-Ausgaben nötig
}

// Overload 1: für normale String-Variablen
void sendWS(String &payload) {
  webSocket.sendTXT(payload);
}

// Overload 2: für String-Literale ("REFRESH", etc.)
void sendWS(const char *payload) {
  String tmp(payload);
  webSocket.sendTXT(tmp);
}

// sehr einfache Root-Tag-Erkennung: <TAG ... >
String getRootTag(const String& xml) {
  int start = xml.indexOf('<');
  if (start < 0) return "";
  int end = xml.indexOf('>', start + 1);
  if (end < 0) return "";
  String tag = xml.substring(start + 1, end);

  // Verarbeitung für <?xml ...?>: Suche nach schließendem ?>
  if (tag.startsWith("?")) {
    int closePos = xml.indexOf("?>", start);
    if (closePos > start) {
      return getRootTag(xml.substring(closePos + 2));
    }
  }

  // Eventuelle Attribute abtrennen
  int spacePos = tag.indexOf(' ');
  if (spacePos > 0) {
    tag = tag.substring(0, spacePos);
  }

  tag.toUpperCase();

  // Entferne ein evtl. vorangestelltes '?' aus <?xml ...>
  if (tag.startsWith("?")) {
    tag = tag.substring(1);
  }
  return tag;
}

String extractAttribute(const String& block, const String& attr) {
  String marker = attr + "=";
  int pos = block.indexOf(marker);
  if (pos < 0) return "";
  pos += marker.length();
  if (pos >= block.length()) return "";
  char quote = block.charAt(pos);
  if (quote != '"' && quote != '\'') {
    return "";
  }
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
  if (degreeIndex >= 0) {
    value = value.substring(0, degreeIndex);
  }
  value.trim();
  if (value.length() == 0) {
    return NAN;
  }
  return value.toFloat();
}

void updateLedModeFromTemperature(float temperature) {
  if (isnan(temperature)) {
    return;
  }

  if (temperature < 30.0f) {
    requestedLedMode = LED_MODE_RED_BLINK;
  } else if (temperature < 35.0f) {
    requestedLedMode = LED_MODE_ORANGE;
  } else {
    requestedLedMode = LED_MODE_GREEN;
  }
}

void applyLedColor(uint8_t red, uint8_t green, uint8_t blue) {
  statusPixel.setPixelColor(0, statusPixel.Color(red, green, blue));
  statusPixel.show();
}

void handleStatusLed() {
  unsigned long now = millis();

  if (requestedLedMode == LED_MODE_RED_BLINK) {
    if (appliedLedMode != LED_MODE_RED_BLINK) {
      appliedLedMode   = LED_MODE_RED_BLINK;
      ledBlinkOn       = false;
      lastLedBlinkMs   = now;
      applyLedColor(255, 0, 0);
      return;
    }

    if (now - lastLedBlinkMs >= LED_BLINK_INTERVAL_MS) {
      ledBlinkOn = !ledBlinkOn;
      lastLedBlinkMs = now;
      applyLedColor(ledBlinkOn ? 255 : 0, 0, 0);
    }
  } else {
    if (appliedLedMode == LED_MODE_RED_BLINK) {
      ledBlinkOn = false;
    }

    if (requestedLedMode != appliedLedMode) {
      switch (requestedLedMode) {
        case LED_MODE_RED:
          applyLedColor(255, 0, 0);
          break;
        case LED_MODE_ORANGE:
          applyLedColor(255, 120, 0);
          break;
        default:
          applyLedColor(0, 255, 0);
          break;
      }
      appliedLedMode = requestedLedMode;
    }
  }
}

bool getLocalTimeInfo(struct tm &timeinfo) {
  if (!getLocalTime(&timeinfo, 100)) {
    return false;
  }
  return true;
}

void syncTimeIfNeeded(bool force = false) {
  unsigned long now = millis();
  if (force || now - lastNtpSyncMs >= NTP_SYNC_INTERVAL_MS) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTimeInfo(timeinfo)) {
      lastNtpSyncMs = now;
    }
  }
}

String formatTemperature(float value) {
  if (isnan(value)) {
    return "--.-";
  }
  char buffer[8];
  dtostrf(value, 4, 1, buffer);
  return String(buffer);
}

void updateDisplay() {
  if (!displayReady) {
    return;
  }
  struct tm timeinfo;
  bool hasTime = getLocalTimeInfo(timeinfo);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);

  if (hasTime) {
    char timeBuffer[20];
    strftime(timeBuffer, sizeof(timeBuffer), "%d.%m %H:%M:%S", &timeinfo);
    display.println(timeBuffer);
  } else {
    display.println("Zeit NTP...");
  }

  display.println("-------------------");
  display.setTextSize(2);
  display.print("A. ");
  display.setTextSize(2);
  display.print(formatTemperature(lastOutsideTemp));
  display.setTextSize(2);
  display.println("C");

  display.print("WW. ");
  display.setTextSize(2);
  display.print(formatTemperature(lastWarmWaterTemp));
  display.setTextSize(2);
  display.println("C");

  display.print("VL. ");
  display.setTextSize(2);
  display.print(formatTemperature(lastVorlaufTemp));
  display.setTextSize(2);
  display.println("C");

  display.display();
  lastDisplayUpdateMs = millis();
  displayDirty = false;
}

// Sucht in der Navigation nach dem <item> mit <name>Temperaturen</name>
// und merkt sich dessen id in tempNavId.
void findTemperatureNavId(const String& xml) {
  tempNavId = "";

  int pos = 0;
  while (true) {
    int itemStart = xml.indexOf("<item", pos);
    if (itemStart < 0) break;

    int tagEnd = xml.indexOf('>', itemStart);
    if (tagEnd < 0) break;

    // Nur das Start-Tag des Items
    String itemOpenTag = xml.substring(itemStart, tagEnd + 1);
    String id = extractAttribute(itemOpenTag, "id");

    // Das erste <name> hinter diesem <item>-Tag gehört in der Regel zu diesem Item
    int nameStart = xml.indexOf("<name>", tagEnd);
    if (nameStart < 0) {
      pos = tagEnd + 1;
      continue;
    }
    int nameEnd = xml.indexOf("</name>", nameStart);
    if (nameEnd < 0) {
      pos = tagEnd + 1;
      continue;
    }

    String name = xml.substring(nameStart + 6, nameEnd);
    name.trim();

    // Auf "Temperaturen" prüfen (ohne Rücksicht auf Groß/Kleinschreibung)
    String cmp = name;
    cmp.toLowerCase();
    if (cmp == "temperaturen") {
      tempNavId = id;
      break;
    }

    pos = tagEnd + 1;
  }
}

// Liest alle <item>…</item>-Blöcke aus CONTENT/VALUES und gibt Name: Wert aus
void printTemperaturesFromContent(const String& xml) {
  int pos = 0;
  bool warmWaterFound = false;
  float detectedWarmWater = NAN;
  while (true) {
    int itemStart = xml.indexOf("<item", pos);
    if (itemStart < 0) break;

    int itemEnd = xml.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;

    String block = xml.substring(itemStart, itemEnd + 7);

    String name  = extractTagValue(block, "name");
    String value = extractTagValue(block, "value");

    if (name.length() > 0 && value.length() > 0) {
      name.trim();
      value.trim();
      Serial.print(name);
      Serial.print(": ");
      Serial.println(value);

      String loweredName = name;
      loweredName.toLowerCase();
      loweredName.replace(" ", "");
      if (loweredName.indexOf("warmwasser-ist") >= 0 || loweredName == "warmwasserist") {
        float parsed = parseTemperatureValue(value);
        if (!isnan(parsed)) {
          detectedWarmWater = parsed;
          warmWaterFound = true;
        }
      }

      if (loweredName.indexOf("aussentemperatur") >= 0) {
        float parsed = parseTemperatureValue(value);
        if (!isnan(parsed)) {
          lastOutsideTemp = parsed;
          displayDirty = true;
        }
      }

      if (loweredName == "vorlauf") {
        float parsed = parseTemperatureValue(value);
        if (!isnan(parsed)) {
          lastVorlaufTemp = parsed;
          displayDirty = true;
        }
      }
    }

    pos = itemEnd + 7;
  }

  if (warmWaterFound) {
    lastWarmWaterTemp = detectedWarmWater;
    updateLedModeFromTemperature(detectedWarmWater);
    displayDirty = true;
  }
  if (displayDirty) {
    updateDisplay();
  }
}

// =========================
// WebSocket-Event-Handler
// =========================

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      loggedIn = false;
      break;

    case WStype_CONNECTED: {
      // LOGIN & erster REFRESH
      String loginCmd = "LOGIN;";
      loginCmd += CONTROLLER_PASSWORD;
      sendWS(loginCmd);
      loggedIn = true;

      // Navigation anfordern
      sendWS("REFRESH");
      lastRefreshMs = millis();
      cycleComplete = false;
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload).substring(0, length);

      String root = getRootTag(msg);

      if (root == "NAVIGATION" || root == "NAV") {
        // Navigation erhalten -> ID für "Temperaturen" suchen
        findTemperatureNavId(msg);

        if (tempNavId.length() > 0) {
          // Genau einen GET für die Temperatur-Seite schicken
          String cmd = "GET;";
          cmd += tempNavId;
          sendWS(cmd);
          cycleComplete = false;
        } else {
          // Keine passende Navigation gefunden -> Zyklus beenden
          cycleComplete = true;
        }

      } else if (root == "CONTENT" || root == "VALUES") {
        // Inhalt der Temperature-Seite parsen und ausgeben
        printTemperaturesFromContent(msg);
        cycleComplete = true;
      }
      break;
    }

    case WStype_PING:
      // keepalive
      break;

    case WStype_PONG:
      // Antwort auf Ping
      break;

    default:
      break;
  }
}

// =========================
// Arduino setup & loop
// =========================

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (STATUS_LED_POWER_PIN >= 0) {
    pinMode(STATUS_LED_POWER_PIN, OUTPUT);
    digitalWrite(STATUS_LED_POWER_PIN, HIGH);
  }

  statusPixel.begin();
  statusPixel.setBrightness(128);
  statusPixel.show();

  displayReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!displayReady) {
    Serial.println("SSD1306 nicht gefunden");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Luxtronik Monitor");
    display.display();
  }

  connectWifi();

  // WebSocket konfigurieren
  webSocket.begin(CONTROLLER_IP, CONTROLLER_PORT, "/", WS_PROTOCOL);
  webSocket.setExtraHeaders("");  // optional
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);  // 5s Reconnect
  webSocket.setAuthorization("", "");    // keine Auth hier

  lastRefreshMs = millis();
  applyLedColor(0, 255, 0);
  syncTimeIfNeeded(true);
}

void loop() {
  webSocket.loop();
  handleStatusLed();
  syncTimeIfNeeded();

  // Wenn wir nur einmal laufen wollen und der Zyklus fertig ist:
  if (RUN_ONCE && cycleComplete) {
    // Hier könntest du z.B. auch in DeepSleep gehen
    // ESP.deepSleep(0);
    return;
  }

  // Mehrfach-Zyklen sind hier nicht nötig, aber Logik bleibt zur Sicherheit drin
  if (!RUN_ONCE && loggedIn) {
    unsigned long now = millis();
    if (cycleComplete && (now - lastRefreshMs >= REFRESH_INTERVAL_SEC * 1000UL)) {
      // neuen REFRESH nur, wenn man zyklisch laufen möchte
      sendWS("REFRESH");
      lastRefreshMs = now;
      cycleComplete = true;
    }
  }

  if (displayDirty || millis() - lastDisplayUpdateMs >= REFRESH_INTERVAL_SEC * 1000UL) {
    updateDisplay();
  }
}
