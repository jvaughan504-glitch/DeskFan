#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#if defined(ARDUINO_ARCH_ESP32)
#include "esp32-hal-ledc.h"  // ensure LEDC helpers are declared
#endif

// ---------- USER CONFIG: WiFi ----------
const char* ssid     = "BELL728";
const char* password = "9134EC94D365";

// ---------- Pins & PWM ----------
#define FAN_PWM_PIN      4   // PWM output to transistor that pulls fan PWM pin low
#define TACH_PIN         3   // Tachometer input (3.3V pulses)
#define ARGB_DATA_PIN    5   // ARGB data line

#define FAN_PWM_CHANNEL  0
#define FAN_PWM_FREQ     25000  // 25 kHz for PC fan control
#define FAN_PWM_RES      8      // 8-bit (0-255)

// ---------- ARGB settings ----------
#define NUM_LEDS         6  // adjust to match your SickleFlow fan

Adafruit_NeoPixel strip(NUM_LEDS, ARGB_DATA_PIN, NEO_GRB + NEO_KHZ800);

// ---------- Web server ----------
WebServer server(80);

// ---------- HTML UI ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32-C3 Fan + ARGB Control</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      background: #111;
      color: #eee;
      text-align: center;
      padding: 20px;
    }
    .card {
      display: inline-block;
      padding: 20px;
      margin: 10px;
      border-radius: 10px;
      background: #222;
      box-shadow: 0 0 10px #000;
      min-width: 280px;
    }
    input[type=range] {
      width: 260px;
    }
    #colorPreview {
      width: 50px;
      height: 50px;
      border-radius: 50%;
      border: 1px solid #333;
      margin: 10px auto;
      box-shadow: 0 0 10px #000;
    }
    .value {
      font-size: 1.2em;
      font-weight: bold;
    }
  </style>
</head>
<body>
  <h1>ESP32-C3 Fan &amp; ARGB Controller</h1>

  <div class="card">
    <h2>Fan Speed</h2>
    <input type="range" id="fanSlider" min="0" max="100" value="50"
           oninput="fanLabel.textContent=this.value; sendFan(this.value);">
    <div>Speed: <span id="fanLabel" class="value">50</span>%</div>
  </div>

  <div class="card">
    <h2>Fan RPM</h2>
    <div class="value" id="rpmVal">----</div>
  </div>

  <div class="card">
    <h2>ARGB Color</h2>
    <input type="color" id="colorPicker" value="#00ff00" oninput="updateColor();">
    <div id="colorPreview"></div>
  </div>

  <script>
    function sendFan(val) {
      fetch('/setFan?val=' + val)
        .then(response => response.text())
        .then(text => console.log(text))
        .catch(err => console.error(err));
    }

    function updateColor() {
      var colorPicker = document.getElementById('colorPicker');
      var color = colorPicker.value; // "#RRGGBB"
      document.getElementById('colorPreview').style.backgroundColor = color;
      fetch('/setColor?c=' + encodeURIComponent(color))
        .then(response => response.text())
        .then(text => console.log(text))
        .catch(err => console.error(err));
    }

    function pollRpm() {
      fetch('/rpm')
        .then(response => response.json())
        .then(data => {
          document.getElementById('rpmVal').textContent = data.rpm;
        })
        .catch(err => {
          console.error(err);
          document.getElementById('rpmVal').textContent = "ERR";
        });
    }

    window.addEventListener('load', function() {
      updateColor();
      pollRpm();
      setInterval(pollRpm, 1000); // update every second
    });
  </script>
</body>
</html>
)rawliteral";

// ---------- State ----------
uint8_t currentFanDuty = 128;  // 0-255
uint8_t currentR = 0, currentG = 255, currentB = 0;

volatile uint32_t tachPulses = 0;  // incremented in ISR on each pulse
uint16_t currentRpm = 0;
uint32_t lastRpmMillis = 0;

// ---------- Fan PWM helpers (ESP32 LEDC or generic PWM fallback) ----------
void initFanPwm() {
#if defined(ARDUINO_ARCH_ESP32)
  ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RES);
  ledcAttachPin(FAN_PWM_PIN, FAN_PWM_CHANNEL);
#else
  pinMode(FAN_PWM_PIN, OUTPUT);
#endif
}

void writeFanDuty(uint8_t duty) {
#if defined(ARDUINO_ARCH_ESP32)
  ledcWrite(FAN_PWM_CHANNEL, duty);
#else
  analogWrite(FAN_PWM_PIN, duty);
#endif
}

// ---------- Helper: set fan PWM 0-100% ----------
void setFanPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint8_t duty = map(percent, 0, 100, 0, 255);
  currentFanDuty = duty;
  writeFanDuty(duty);
}

// ---------- Helper: set ARGB color ----------
void setColor(uint8_t r, uint8_t g, uint8_t b) {
  currentR = r;
  currentG = g;
  currentB = b;
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

// ---------- Tach ISR ----------
void IRAM_ATTR tachISR() {
  tachPulses++;
}

// ---------- Compute RPM (called from loop) ----------
void updateRpm() {
  uint32_t now = millis();
  uint32_t dt = now - lastRpmMillis;
  if (dt < 500) return;  // update at ~1Hz, but allow some slack

  noInterrupts();
  uint32_t pulses = tachPulses;
  tachPulses = 0;
  interrupts();

  lastRpmMillis = now;

  if (dt == 0) {
    currentRpm = 0;
    return;
  }

  // pulses per second
  float pps = (float)pulses * 1000.0f / (float)dt;
  // 2 pulses per revolution
  float rps = pps / 2.0f;
  uint16_t rpm = (uint16_t)(rps * 60.0f + 0.5f);

  currentRpm = rpm;
}

// ---------- HTTP handlers ----------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleSetFan() {
  if (!server.hasArg("val")) {
    server.send(400, "text/plain", "Missing val");
    return;
  }
  int val = server.arg("val").toInt();
  if (val < 0) val = 0;
  if (val > 100) val = 100;
  setFanPercent((uint8_t)val);
  String response = "Fan speed set to ";
  response += val;
  response += "% (duty=";
  response += currentFanDuty;
  response += ")";
  server.send(200, "text/plain", response);
}

void handleSetColor() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "Missing c");
    return;
  }
  String hex = server.arg("c");  // "#RRGGBB"
  if (hex.length() != 7 || hex.charAt(0) != '#') {
    server.send(400, "text/plain", "Invalid color format");
    return;
  }

  long number = strtol(hex.substring(1).c_str(), NULL, 16);
  uint8_t r = (number >> 16) & 0xFF;
  uint8_t g = (number >> 8) & 0xFF;
  uint8_t b = number & 0xFF;

  setColor(r, g, b);

  String response = "Color set to ";
  response += hex;
  server.send(200, "text/plain", response);
}

void handleRpm() {
  // Simple JSON: { "rpm": 1234 }
  String json = "{ \"rpm\": ";
  json += currentRpm;
  json += " }";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  String message = "404 Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  server.send(404, "text/plain", message);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBooting...");

  // Fan PWM
  initFanPwm();
  setFanPercent(50); // start at 50%

  // ARGB
  strip.begin();
  strip.show(); // all off
  setColor(currentR, currentG, currentB);

  // Tach input
  pinMode(TACH_PIN, INPUT_PULLUP);  // or INPUT if you have external pull-up
  attachInterrupt(digitalPinToInterrupt(TACH_PIN), tachISR, FALLING);
  lastRpmMillis = millis();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());

  // Web server routes
  server.on("/", handleRoot);
  server.on("/setFan", handleSetFan);
  server.on("/setColor", handleSetColor);
  server.on("/rpm", handleRpm);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  updateRpm();
}
