Luxtronik ESP32-S3 Monitor

Live temperature monitoring for Luxtronik heating controllers using WebSocket, OLED display, and RGB status LED

This project implements a standalone temperature monitor for Luxtronik heating systems using an ESP32-S3 DevKit.
The ESP32 communicates directly with the Luxtronik controller via WebSocket (Lux_WS protocol), parses the temperature values, and displays them on an OLED screen while also providing visual feedback using an RGB status LED.

No cloud services, no backend server, and no polling of HTML pages — everything works locally and in real time.

✨ Features
🔌 Direct WebSocket Communication

Connects directly to the Luxtronik controller

Uses the native Lux_WS protocol

Automatically logs in using configured controller password

Automatically discovers the navigation ID for the "Temperaturen" page

Parses all available <item> entries from CONTENT or VALUES

📺 OLED Display Output (SSD1306, 128×64)

Shows the current date/time (NTP synced)

Displays:

Outdoor temperature

Domestic hot water (Warmwasser)

Flow temperature (Vorlauf)

Large, easy-to-read layout

Automatic periodic updates

🌈 RGB Status LED (Onboard NeoPixel)

Temperature-based status indication:

Green – normal

Orange – borderline

Red blinking – critical hot water temperature (< 30 °C)

Fully dynamic mode switching and blinking logic

🌐 Stable WiFi Operation

Automatic reconnect on network loss

Reboot after long-term failure

Designed for 24/7 operation

⏱ Configurable Operation Mode

Adjustable refresh interval

Optional single-run mode (for deep-sleep or battery applications)

🧰 Hardware Requirements

ESP32-S3 DevKit (with onboard NeoPixel RGB LED)

OLED SSD1306 – 128×64 pixels, I²C

Luxtronik heating controller (WebSocket capable)

USB cable for flashing

WiFi network

Typical Wiring (OLED)
SSD1306 Pin	ESP32-S3 Pin
VCC	3.3V
GND	GND
SDA	GPIO 8 (default I²C)
SCL	GPIO 9 (default I²C)

(Adjust pins in code if needed.)

📦 Software Requirements

Arduino IDE with the following libraries:

Library	Notes
WiFi	Built-in
WebSocketsClient	by Markus Sattler
Adafruit_GFX	Display rendering
Adafruit_SSD1306	OLED handling
Adafruit_NeoPixel	RGB LED control
🚀 Getting Started
1. Clone the repository

2. Install required libraries

Use Arduino Library Manager.

3. Configure your settings

Modify these lines in the code:

const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

const char* CONTROLLER_IP = "192.168.xxx.xxx";
const char* CONTROLLER_PASSWORD = "YOUR_LUXTRONIK_PASSWORD";

4. Select your board

Arduino IDE → Tools → Board → ESP32S3 Dev Module

5. Upload the code

Compile and flash the sketch to your ESP32-S3.

📊 How It Works (Technical Overview)

WiFi connection is established

NTP time sync initializes local time

ESP32 opens a WebSocket to the Luxtronik controller

Sends:

LOGIN;password

REFRESH

Receives navigation menu → identifies the Temperaturen page by name

Sends:

GET;<nav_id>

Parses each <item> block:

<name>…</name>

<value>…</value>

Converts temperatures and updates:

Serial output

OLED display

RGB LED

Repeats periodically based on timer

🔧 Configuration Parameters
Parameter	Description
REFRESH_INTERVAL_SEC	Interval for new WebSocket refresh
RUN_ONCE	If true, performs one cycle only
LED_BLINK_INTERVAL_MS	Blinking speed for warning mode
GMT_OFFSET_SEC	Timezone offset
NTP_SERVER	Changeable NTP server
🟩 Temperature Status Logic
Condition	LED Color	Meaning
< 30 °C	Red blinking	Critical hot water
< 35 °C	Orange	Borderline
≥ 35 °C	Green	Normal
🧪 Serial Output Example
Warmwasser-Ist: 39.4 °C
Aussentemperatur: 4.2 °C
Vorlauf: 28.7 °C

📷 Optional: Add photos/screenshots

You can include:

Setup Photo

OLED Display Output

Serial Monitor example

LED status example

⚠️ Security Warning

Do not commit the following to public repositories:

WiFi passwords

Luxtronik passwords

Local IP addresses

Replace them with placeholders before publishing.

❗ Known Limitations

XML parsing is intentionally lightweight and not fully standards compliant

Some Luxtronik firmware versions may name temperature items differently

If the navigation structure changes, ID discovery may need adjustment

🤝 Contributions

Pull requests, enhancements, and bug reports are welcome!
Feel free to open an issue if you want new features or need help.

📄 License

MIT License – free for private and commercial use.
