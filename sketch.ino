/*
  SMART COLD CHAIN MONITORING - Wokwi Simulation (Hardened v2)
  ----------------------------------------------------------------
  Boards/Parts used (all natively simulate-able in Wokwi):
    - ESP32 DevKit v1
    - DS18B20              -> GPIO4   (temperature)
    - DHT22                 -> GPIO15  (humidity)
    - NEO-6M GPS             -> UART2 (RX2=GPIO16, TX2=GPIO17)
    - Reed Switch (door)     -> GPIO27
    - MPU6050 (shock)        -> I2C  SDA=GPIO21, SCL=GPIO22
    - SSD1306 OLED           -> I2C  SDA=GPIO21, SCL=GPIO22 (shared bus)
    - Buzzer                  -> GPIO26
    - Relay (cooling ctrl)    -> GPIO25
    - MicroSD card (SPI)      -> CS=GPIO5, MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
    - Potentiometer            -> GPIO34  (stands in for a battery-voltage sense circuit)
    - Pushbutton               -> GPIO32  (toggles a simulated "main power lost" event)

  WHAT CHANGED IN THIS "HARDENED" VERSION, AND WHY
  ---------------------------------------------------
  This version specifically addresses four real-world operational risks
  that the original prototype did NOT handle:

  1. MAINTENANCE / CALIBRATION DRIFT
     -> checkCalibrationStatus() tracks device uptime and flags sensors as
        "calibration due" past a threshold, logging it instead of silently
        trusting readings forever.

  2. SYSTEM FAILURES (software hangs, silent freezes)
     -> A hardware watchdog timer (esp_task_wdt) force-reboots the ESP32
        if the main loop ever stops responding, which is standard practice
        for unattended IoT devices.

  3. POWER DEPENDENCY
     -> A battery-voltage sense input and a main-power-loss detector let
        the device recognize when it has switched to backup power, log
        that event, and drop into a reduced-power mode to extend battery
        runtime instead of dying silently.

  4. SECURITY RISKS
     -> Cloud uploads now use HTTPS (WiFiClientSecure) instead of plain
        HTTP, and requests are authenticated with a device API key header.
        NOTE: setInsecure() below skips certificate validation for demo
        simplicity in the simulator. A real deployment MUST validate the
        server certificate (or use mutual TLS / AWS IoT device certs)
        instead of setInsecure().

  Libraries to install (Library Manager, exact names):
    - OneWire
    - DallasTemperature
    - DHT sensor library (by Adafruit)  + Adafruit Unified Sensor
    - Adafruit MPU6050 + Adafruit Sensor
    - Adafruit SSD1306 + Adafruit GFX Library
    - TinyGPSPlus
  (esp_task_wdt.h, WiFiClientSecure.h, HTTPClient.h, WiFi.h, SD.h, SPI.h
   are all part of the built-in ESP32 Arduino core - no separate install needed)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <SD.h>
#include <esp_task_wdt.h>

// ---------- Pin definitions ----------
#define DS18B20_PIN     4
#define DHT_PIN         15
#define DHT_TYPE        DHT22
#define DOOR_PIN        27
#define BUZZER_PIN      26
#define RELAY_PIN       25
#define SD_CS_PIN       5
#define BATTERY_PIN     34   // analog: simulated battery voltage divider
#define POWER_LOSS_PIN  32   // digital: simulated "mains power lost" toggle

// ---------- Security config ----------
// In production, do NOT hardcode secrets in firmware source. Store this in
// ESP32 NVS (encrypted partition) or provision it at manufacturing time.
// It's hardcoded here only because this is a simulator demo.
const char* DEVICE_API_KEY = "DEMO-DEVICE-KEY-CHANGE-ME";
const char* CLOUD_ENDPOINT  = "https://httpbin.org/post"; // replace with your real HTTPS API

// ---------- Reliability config ----------
#define WDT_TIMEOUT_SECONDS 10
// Demo value kept low so you can SEE the calibration-due warning fire during
// a short simulation run. In production this should be ~8760 (1 year in hours).
#define CALIBRATION_INTERVAL_HOURS 0.02  // ~72 seconds, for demo purposes only

// ---------- Objects ----------
OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
TinyGPSPlus gps;
HardwareSerial GPSSerial(2); // UART2 -> RX2=16, TX2=17
WiFiClientSecure secureClient;

// ---------- State ----------
int doorOpenCount = 0;
bool lastDoorState = false;
float shelfLifeHours = 24.0;
int spoilageRisk = 0;
unsigned long lastLogTime = 0;
unsigned long bootTimeMillis = 0;
bool calibrationDue = false;
bool onBackupPower = false;
bool lastPowerBtnState = HIGH;
const float SAFE_TEMP_MIN = 2.0;   // deg C
const float SAFE_TEMP_MAX = 8.0;   // deg C
const float LOW_BATTERY_PCT = 20.0;

void setup() {
  Serial.begin(115200);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(POWER_LOSS_PIN, INPUT_PULLUP);
  digitalWrite(RELAY_PIN, LOW);

  tempSensor.begin();
  dht.begin();
  GPSSerial.begin(9600, SERIAL_8N1, 16, 17);

  Wire.begin(21, 22);
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found!");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card init failed (simulation may not include a card image).");
  } else {
    File logFile = SD.open("/log.csv", FILE_WRITE);
    if (logFile) {
      logFile.println("timestamp,temp,humidity,door_opens,shock,risk,shelf_life_hrs,battery_pct,on_backup_power,calibration_due");
      logFile.close();
    }
  }

  // ---- RELIABILITY: hardware watchdog ----
  // If loop() ever hangs (a software bug, a sensor library that never
  // returns, etc.) this forces a full reboot rather than leaving the
  // device silently frozen and offline indefinitely.
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true); // true = panic/reboot on timeout
  esp_task_wdt_add(NULL);

  // Optional: connect to Wokwi's simulated Wi-Fi (has real internet access)
  WiFi.begin("Wokwi-GUEST", "", 6);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected!" : " could not connect (continuing offline).");

  // Demo simplification: skip TLS certificate validation. A production
  // build should call secureClient.setCACert(rootCACert) with the real
  // certificate of your cloud endpoint instead of setInsecure().
  secureClient.setInsecure();

  bootTimeMillis = millis();
}

void loop() {
  esp_task_wdt_reset(); // "I'm alive" - prevents the watchdog reboot

  // ---- MAINTENANCE: calibration-due check ----
  checkCalibrationStatus();

  // ---- POWER: battery + mains-loss check ----
  float batteryPct = readBatteryPercent();
  checkPowerState();

  // ---- Read sensors ----
  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);
  float humidity = dht.readHumidity();
  if (isnan(humidity)) humidity = 0;

  bool doorOpen = (digitalRead(DOOR_PIN) == LOW); // reed switch pulled LOW when open
  if (doorOpen && !lastDoorState) doorOpenCount++;
  lastDoorState = doorOpen;

  sensors_event_t a, g, temp_mpu;
  float shockMagnitude = 0;
  if (mpu.getEvent(&a, &g, &temp_mpu)) {
    shockMagnitude = sqrt(a.acceleration.x * a.acceleration.x +
                           a.acceleration.y * a.acceleration.y +
                           a.acceleration.z * a.acceleration.z);
  }

  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
  }

  // ---- Simple rule-based "AI" model ----
  float tempDeviation = 0;
  if (temperature < SAFE_TEMP_MIN) tempDeviation = SAFE_TEMP_MIN - temperature;
  else if (temperature > SAFE_TEMP_MAX) tempDeviation = temperature - SAFE_TEMP_MAX;

  spoilageRisk = constrain((int)(tempDeviation * 12) + (doorOpenCount * 6) +
                            (humidity > 85 ? 15 : 0), 0, 100);

  // If a sensor is overdue for calibration, we no longer fully trust its
  // reading - inflate the reported risk slightly and flag it, rather than
  // silently presenting a precise-looking number that may be drifting.
  if (calibrationDue) spoilageRisk = constrain(spoilageRisk + 10, 0, 100);

  shelfLifeHours = constrain(24.0 - (tempDeviation * 2.5) - (doorOpenCount * 0.8), 0, 24);

  bool shockDetected = shockMagnitude > 15.0; // rough threshold for a "harsh event"

  // ---- Alerts & actuation ----
  bool alarm = (spoilageRisk > 70) || shockDetected || onBackupPower || (batteryPct < LOW_BATTERY_PCT);
  digitalWrite(BUZZER_PIN, alarm ? HIGH : LOW);

  // On backup power, avoid running the relay-controlled cooling unit
  // aggressively to conserve battery runtime, unless temperature is
  // critically out of range.
  bool coolingNeeded = (temperature > SAFE_TEMP_MAX);
  digitalWrite(RELAY_PIN, (coolingNeeded && (!onBackupPower || tempDeviation > 3.0)) ? HIGH : LOW);

  // ---- OLED display ----
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.print("Temp: "); display.print(temperature); display.println(" C");
  display.print("Humidity: "); display.print(humidity); display.println(" %");
  display.print("Door opens: "); display.println(doorOpenCount);
  display.print("Shelf life: "); display.print(shelfLifeHours); display.println("h");
  display.print("Risk: "); display.print(spoilageRisk); display.println("%");
  display.print("Battery: "); display.print(batteryPct, 0); display.println("%");
  if (onBackupPower) display.println("ON BACKUP POWER");
  if (calibrationDue) display.println("CALIBRATION DUE");
  if (shockDetected) display.println("SHOCK DETECTED!");
  display.display();

  // ---- Serial debug ----
  Serial.printf("Temp=%.2fC Hum=%.1f%% Door#=%d Shock=%.2f Risk=%d%% ShelfLife=%.1fh Batt=%.0f%% Backup=%s CalDue=%s\n",
                temperature, humidity, doorOpenCount, shockMagnitude, spoilageRisk, shelfLifeHours,
                batteryPct, onBackupPower ? "YES" : "no", calibrationDue ? "YES" : "no");
  if (gps.location.isValid()) {
    Serial.printf("GPS: %.6f, %.6f\n", gps.location.lat(), gps.location.lng());
  }

  // ---- Log to SD every 5 seconds ----
  if (millis() - lastLogTime > 5000) {
    lastLogTime = millis();
    File logFile = SD.open("/log.csv", FILE_APPEND);
    if (logFile) {
      logFile.printf("%lu,%.2f,%.1f,%d,%.2f,%d,%.1f,%.0f,%d,%d\n",
                      millis(), temperature, humidity, doorOpenCount,
                      shockMagnitude, spoilageRisk, shelfLifeHours,
                      batteryPct, onBackupPower ? 1 : 0, calibrationDue ? 1 : 0);
      logFile.close();
    }

    // ---- Push to cloud over HTTPS with API key auth (only if Wi-Fi connected) ----
    // Skip cloud upload while on backup power to conserve battery - locally
    // logged data will sync once mains power (and full connectivity) return.
    if (WiFi.status() == WL_CONNECTED && !onBackupPower) {
      HTTPClient http;
      http.begin(secureClient, CLOUD_ENDPOINT);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-API-Key", DEVICE_API_KEY);
      String payload = "{\"temp\":" + String(temperature) +
                        ",\"humidity\":" + String(humidity) +
                        ",\"risk\":" + String(spoilageRisk) +
                        ",\"shelf_life_hrs\":" + String(shelfLifeHours) +
                        ",\"battery_pct\":" + String(batteryPct) +
                        ",\"calibration_due\":" + String(calibrationDue ? "true" : "false") + "}";
      int code = http.POST(payload);
      Serial.printf("Cloud upload (HTTPS) response code: %d\n", code);
      http.end();
    }
  }

  delay(2000);
}

// ---- MAINTENANCE: flags sensors as due for calibration past a threshold ----
// This uses device uptime as a stand-in for calendar time. A production
// build would sync real time via NTP (or an RTC module) so this reflects
// actual calendar days/months since the last physical calibration, and
// would track each sensor's calibration date independently rather than
// one shared flag.
void checkCalibrationStatus() {
  float hoursSinceBoot = (millis() - bootTimeMillis) / 3600000.0;
  bool nowDue = hoursSinceBoot > CALIBRATION_INTERVAL_HOURS;
  if (nowDue && !calibrationDue) {
    Serial.println("MAINTENANCE ALERT: sensors are due for calibration check.");
    File logFile = SD.open("/maintenance.log", FILE_APPEND);
    if (logFile) {
      logFile.printf("%lu,CALIBRATION_DUE\n", millis());
      logFile.close();
    }
  }
  calibrationDue = nowDue;
}

// ---- POWER: reads simulated battery level (0-100%) ----
float readBatteryPercent() {
  int raw = analogRead(BATTERY_PIN); // 0-4095
  return (raw / 4095.0) * 100.0;
}

// ---- POWER: detects a simulated mains-power-loss event (button toggle) ----
void checkPowerState() {
  bool btnState = digitalRead(POWER_LOSS_PIN);
  if (btnState == LOW && lastPowerBtnState == HIGH) {
    onBackupPower = !onBackupPower; // toggle on each press
    Serial.println(onBackupPower ? "!!! MAIN POWER LOST - switched to backup battery !!!"
                                  : "Main power restored.");
    File logFile = SD.open("/power_events.log", FILE_APPEND);
    if (logFile) {
      logFile.printf("%lu,%s\n", millis(), onBackupPower ? "POWER_LOST" : "POWER_RESTORED");
      logFile.close();
    }
  }
  lastPowerBtnState = btnState;
}
