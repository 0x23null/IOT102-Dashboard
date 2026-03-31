#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==========================================
// 1. SYSTEM CONFIGURATION
// ==========================================
#define WIFI_SSID "IOT10232"
#define WIFI_PASSWORD "0123456788"

#define FIREBASE_API_KEY "AIzaSyC3ak9wrcoxz0P45xMaoaSGbKW-abFz-Yc"
#define FIREBASE_DATABASE_URL "https://iot102-airmonitor-default-rtdb.asia-southeast1.firebasedatabase.app"

#define TELEGRAM_BOT_TOKEN "8074728154:AAHEJqS8g_Iq81iSI-A7J4d-WPeqjyFYv9w"
#define TELEGRAM_CHAT_ID "7958238823"

// ==========================================
// 2. HARDWARE PINS
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define MQ135_PIN 34
#define PM_LED_PIN 13
#define PM_VO_PIN 35

// ==========================================
// 3. GLOBAL VARIABLES & STATE FLAGS
// ==========================================
float t = 0.0, h = 0.0;
int mq135_raw = 0, aqi_value = 0;
float dust_ug = 0.0, smoothed_dust_ug = 0.0;

bool isGasHazardous = false;
bool isDustHazardous = false;
bool isHazardous = false;

unsigned long prevSensor = 0;
unsigned long prevOLED = 0;
unsigned long prevFirebase = 0;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool isFirebaseReady = false;

// ==========================================
// 4. CORE FUNCTIONS
// ==========================================

float readSmoothDust() {
  float totalVolt = 0;
  int samples = 30;
  for (int i = 0; i < samples; i++) {
    digitalWrite(PM_LED_PIN, LOW);
    delayMicroseconds(280);
    int raw = analogRead(PM_VO_PIN);
    delayMicroseconds(40);
    digitalWrite(PM_LED_PIN, HIGH);
    delayMicroseconds(9680);
    totalVolt += (raw * (3.3 / 4095.0));
  }
  return totalVolt / samples;
}

int calculateAQI(float pm25) {
  if (pm25 <= 12.0) return map(pm25, 0, 12.0, 0, 50);
  else if (pm25 <= 35.4) return map(pm25, 12.1, 35.4, 51, 100);
  else if (pm25 <= 55.4) return map(pm25, 35.5, 55.4, 101, 150);
  else if (pm25 <= 150.4) return map(pm25, 55.5, 150.4, 151, 200);
  else if (pm25 <= 250.4) return map(pm25, 150.5, 250.4, 201, 300);
  else return map(pm25, 250.5, 500.4, 301, 500);
}

void sendTelegramMessage(String message) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    message.replace(" ", "%20");

    String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) + "&text=" + message;

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
      Serial.printf("[TELEGRAM] Message sent. Code: %d\n", httpCode);
    } else {
      Serial.printf("[TELEGRAM] Failed to send. Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}

void checkHazardState() {

  if (millis() > 120000) {
    // --- 1. GAS CHECK ---
    if (mq135_raw > 1250) {
      if (!isGasHazardous) {
        isGasHazardous = true;
        isHazardous = true;
        updateOLED();
        Serial.println("[STATE] GAS: HAZARDOUS");
        sendTelegramMessage("🚨 GAS ALERT: High gas or smoke detected.");
      }
    } else {
      if (isGasHazardous) {
        isGasHazardous = false;
        Serial.println("[STATE] GAS: SAFE");
        sendTelegramMessage("✅ GAS CLEAR: Gas levels returned to normal.");
      }
    }
  }

  // --- 2. DUST CHECK (AQI) ---
  if (aqi_value > 150) {
    if (!isDustHazardous) {
      isDustHazardous = true;
      Serial.println("[STATE] DUST: HAZARDOUS");
      sendTelegramMessage("😷 DUST ALERT: Air quality is poor, AQI over 150!");
    }
  } else {
    if (isDustHazardous) {
      isDustHazardous = false;
      Serial.println("[STATE] DUST: SAFE");
      sendTelegramMessage("✅ DUST CLEAR: Air quality is safe again.");
    }
  }

  isHazardous = (isGasHazardous || isDustHazardous);
}

void updateOLED() {
  display.clearDisplay();

  if (isHazardous) {
    // UI Override: Blinking Warning
    bool blinkState = (millis() / 500) % 2 == 0;
    display.invertDisplay(blinkState);

    display.setTextSize(2);
    display.setCursor(20, 25);
    display.println("DANGER!");
  } else {
    // UI Normal: Standard Metrics
    display.invertDisplay(false);
    display.setTextSize(1);
    display.setCursor(0, 0);
    //display.println("IP: " + WiFi.localIP().toString());
    display.println("Air Monitoring System");
    display.setCursor(0, 15);
    display.printf("Temp: %.1f C", t);
    display.setCursor(0, 25);
    display.printf("Hum:  %.1f %%", h);
    display.setCursor(0, 35);
    display.printf("Dust: %d ug/m3", (int)smoothed_dust_ug);
    display.setCursor(0, 45);
    display.printf("AQI:  %d", aqi_value);
    display.setCursor(0, 55);
    display.printf("Gas:  %d", mq135_raw);
  }
  display.display();
}

// ==========================================
// 5. SYSTEM INITIALIZATION
// ==========================================
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PM_LED_PIN, OUTPUT);
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED initialization failed");
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[SYSTEM] WiFi Connected!");

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("[SYSTEM] Firebase Auth OK");
    isFirebaseReady = true;
  } else {
    Serial.printf("[ERROR] Firebase Auth Failed: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

// ==========================================
// 6. MAIN LOOP (NON-BLOCKING)
// ==========================================
void loop() {
  unsigned long now = millis();

  // TASK 1: Read Sensors (Every 2 seconds)
  if (now - prevSensor >= 2000) {
    prevSensor = now;

    float new_t = dht.readTemperature();
    float new_h = dht.readHumidity();
    if (!isnan(new_t) && !isnan(new_h)) {
      t = new_t;
      h = new_h;
    }

    mq135_raw = analogRead(MQ135_PIN);

    float volt_dust_raw = readSmoothDust();
    float dustDensity = 0.17 * volt_dust_raw - 0.035;
    if (dustDensity < 0) dustDensity = 0;

    dust_ug = dustDensity * 1000;

    if (smoothed_dust_ug == 0.0 && dust_ug > 0) {
      smoothed_dust_ug = dust_ug;
    } else {
      smoothed_dust_ug = (0.1 * dust_ug) + (0.9 * smoothed_dust_ug);
    }

    aqi_value = calculateAQI(smoothed_dust_ug);

    // Evaluate if current state triggers an alert
    checkHazardState();
  }

  // TASK 2: Update UI (Every 0.5 seconds)
  if (now - prevOLED >= 500) {
    prevOLED = now;
    updateOLED();
  }

  // TASK 3: Sync to Cloud (Every 3 seconds)
  if (now - prevFirebase >= 3000) {
    prevFirebase = now;
    if (Firebase.ready() && isFirebaseReady) {
      Firebase.RTDB.setFloat(&fbdo, "SensorData/nhiet", t);
      Firebase.RTDB.setFloat(&fbdo, "SensorData/am", h);
      Firebase.RTDB.setInt(&fbdo, "SensorData/bui", (int)smoothed_dust_ug);
      Firebase.RTDB.setInt(&fbdo, "SensorData/gas", mq135_raw);
      Firebase.RTDB.setInt(&fbdo, "SensorData/aqi", aqi_value);
    }
  }
}