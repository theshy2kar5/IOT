#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <RTClib.h>

// ================= CẤU HÌNH =================
#define WIFI_SSID "Ggj"
#define WIFI_PASS "21382005"

#define PIN_DHT      34
#define PIN_MP135    14
#define PIN_MQ7      33
#define PIN_LED      2

#define GP2Y_LED_PIN 19
#define GP2Y_AOUT_PIN 35

#define I2C_SDA      21
#define I2C_SCL      22

// ================= KHỞI TẠO =================
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
DHT dht(PIN_DHT, DHT22);
WebSocketsServer ws(81);
RTC_DS3231 rtc;

// ================= BIẾN =================
float   temp        = 0.0;
float   hum         = 0.0;
int     smoke       = 0;    // MQ-135 raw ADC
int     co          = 0;    // MQ-7 raw ADC
float   dust        = 0.0;  // GP2Y mg/m³
float   dust_v      = 0.0;  // GP2Y voltage
String  timeStr     = "--:--:--";
String  dateStr     = "--/--/----";
float   rtctemp     = 0.0;
bool    warmup      = true;
uint32_t startMs    = 0;

// ================= GP2Y =================
void readDust() {
  digitalWrite(GP2Y_LED_PIN, LOW);
  delayMicroseconds(280);
  int raw = analogRead(GP2Y_AOUT_PIN);
  delayMicroseconds(40);
  digitalWrite(GP2Y_LED_PIN, HIGH);
  delayMicroseconds(9680);

  dust_v = raw * (3.3f / 4095.0f);
  float d = (dust_v - 0.9f) / 0.5f;
  dust = max(d, 0.0f);
}

// ================= WS EVENT =================
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED)
    Serial.printf("[WS] Client #%d connected\n", num);
  else if (type == WStype_DISCONNECTED)
    Serial.printf("[WS] Client #%d disconnected\n", num);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  startMs = millis();

  pinMode(PIN_LED,      OUTPUT);
  pinMode(GP2Y_LED_PIN, OUTPUT);
  digitalWrite(PIN_LED,      HIGH);
  digitalWrite(GP2Y_LED_PIN, HIGH);

  Wire.begin(I2C_SDA, I2C_SCL);

  // OLED
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("OLED failed!");
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0); oled.println("AirSense v4.1");
  oled.setCursor(0, 10); oled.println("Khoi dong...");
  oled.display();

  // DHT22
  dht.begin();
  delay(2000);

  // DS3231
  if (!rtc.begin())
    Serial.println("DS3231 not found!");
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); // bỏ comment 1 lần để set giờ

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nIP: " + WiFi.localIP().toString());
    oled.setCursor(0, 20); oled.println("WiFi OK!");
    oled.setCursor(0, 30); oled.println(WiFi.localIP().toString());
    oled.display();
  } else {
    Serial.println("\nWiFi FAILED");
    oled.setCursor(0, 20); oled.println("WiFi FAILED");
    oled.display();
  }

  ws.begin();
  ws.onEvent(onWsEvent);
  Serial.println("WebSocket port 81");
  delay(1000);
}

// ================= LOOP =================
void loop() {
  ws.loop();

  // Warmup MQ sensors 3 phút
  warmup = (millis() - startMs) < 180000UL;

  // --- DHT22 mỗi 2 giây ---
  static uint32_t lastDHT = 0;
  if (millis() - lastDHT > 2000) {
    lastDHT = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && t > -40 && t < 80)  temp = t;
    else Serial.println("DHT22: temp failed");
    if (!isnan(h) && h >= 0 && h <= 100) hum  = h;
    else Serial.println("DHT22: humi failed");
  }

  // --- MQ sensors ---
  smoke = analogRead(PIN_MP135);
  co    = analogRead(PIN_MQ7);

  // --- GP2Y ---
  readDust();

  // --- DS3231 ---
  DateTime now = rtc.now();
  char tb[9], db[11];
  sprintf(tb, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  sprintf(db, "%02d/%02d/%04d", now.day(), now.month(), now.year());
  timeStr = tb;
  dateStr = db;
  rtctemp = rtc.getTemperature();

  // --- OLED ---
  oled.clearDisplay();
  oled.setCursor(0,  0); oled.printf("T:%.1fC  H:%.0f%%", temp, hum);
  oled.setCursor(0, 10); oled.printf("Dust:%.3f mg/m3", dust);
  oled.setCursor(0, 20); oled.printf("Smoke:%4d CO:%4d", smoke, co);
  oled.setCursor(0, 30); oled.printf("RSSI:%d dBm", WiFi.RSSI());
  oled.setCursor(0, 40); oled.printf("Warmup:%s", warmup ? "ON" : "OK");
  oled.setCursor(0, 50); oled.printf("%s", timeStr.c_str());
  oled.display();

  // --- JSON gửi mỗi 2 giây ---
  static uint32_t lastSend = 0;
  if (millis() - lastSend > 2000) {
    lastSend = millis();

    JsonDocument doc;
    doc["temp"]    = temp;
    doc["humi"]    = hum;
    doc["smoke"]   = smoke;           // MQ-135 ADC raw
    doc["co"]      = co;              // MQ-7 ADC raw
    doc["dust"]    = dust;            // GP2Y mg/m³
    doc["dust_v"]  = dust_v;          // GP2Y voltage
    doc["rtc"]     = timeStr;
    doc["date"]    = dateStr;
    doc["rtctemp"] = rtctemp;
    doc["warmup"]  = warmup;
    doc["rssi"]    = WiFi.RSSI();
    doc["model"]   = "ESP32-DEVKITC";
    doc["fw"]      = "4.1";
    doc["uptime"]  = millis() / 1000;
    doc["ram"]     = ESP.getFreeHeap();

    String json;
    serializeJson(doc, json);
    ws.broadcastTXT(json);

    Serial.printf("→ T:%.1f H:%.0f Smoke:%d CO:%d Dust:%.3f\n",
                  temp, hum, smoke, co, dust);
  }

  delay(100);
}