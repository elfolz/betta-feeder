/*
  BettaFeeder - ESP32-WROOM-DA + 28BYJ-48 + ULN2003
  Wi-Fi/NTP clock; BLE configuration; 15-position carousel:
  - position 0 is the EMPTY safety pocket parked at the discharge opening
  - positions 1..14 hold the 14 portions
  - after portion 14, the rotor stops at pocket 14 until refill is confirmed in the PWA

  Arduino IDE:
  - Board package: "esp32 by Espressif Systems"
  - For 4 MB flash, select partition scheme "No FS (2MB APP x2)"; the default
    1.2 MB app partition is too small for this firmware.
  - BLE uses NimBLE-Arduino, a lower-memory alternative to Bluedroid.
  - No DS3231 / RTC module required.

  BLE commands (write one command per BLE write):
    SSID=MyWiFi
    PASS=MyPassword
    FEED1=08:00
    FEED2=20:00
    TIMES=08:00,20:00
    STATUS
    FEED
    SET_HOME       // physically align empty pocket on first use or after interrupted motion
    RELOAD         // confirm refill in the PWA; resumes without moving the motor
    CAL_STEPS=4076 // gearbox calibration; requires SET_HOME afterwards
    CLEAR_WIFI
    BLE_ON         // comando legado; BLE permanece ativo

  Motor pins (ULN2003 IN1..IN4):
    IN1=GPIO16, IN2=GPIO17, IN3=GPIO18, IN4=GPIO19

  IMPORTANT:
  - The motor has no position sensor. SET_HOME is a manual reference operation.
  - A clean reboot restores the saved rotor position without moving the motor.
  - If power is lost during a movement, the rotor position may be uncertain;
    re-align manually and send SET_HOME before feeding resumes.
*/

#include <WiFi.h>
#include <esp_bt.h>
#include <time.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECppVersion.h>
#include <NimBLEUtils.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include <memory>
#include <cstring>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>

// ---------- Hardware ----------
static const uint8_t MOTOR_PINS[4] = {16, 17, 18, 19}; // ULN2003 IN1..IN4
// DS18B20 data pin; use a 4.7k pull-up between DATA and 3.3V.
static const uint8_t DS18B20_PIN = 4;
static const float LOW_TEMPERATURE_LIMIT_C = 24.0f;
static const float HIGH_TEMPERATURE_LIMIT_C = 28.0f;
static const unsigned long TEMPERATURE_SAMPLE_MS = 2500;
static const uint32_t TEMPERATURE_ALERT_INTERVAL_SECONDS = 30UL * 60UL;
// 28BYJ-48 varies by gearbox. Start at 4076 halfsteps/revolution;
// CAL_STEPS=NNNN via BLE permits adjusting after physical testing.
uint16_t halfStepsPerRev = 4076;
static const bool MOTOR_FORWARD = true; // invert if mechanical rotation is opposite
static const uint8_t POCKETS = 15;
static const uint8_t PORTIONS_PER_LOAD = 14;
static const uint16_t STEP_DELAY_MS = 2;
static const uint8_t JOG_HALF_STEPS = 32;

// 8-step half-step sequence, coils IN1..IN4
static const uint8_t HALF_STEP_SEQ[8][4] = {
  {1,0,0,0},
  {1,1,0,0},
  {0,1,0,0},
  {0,1,1,0},
  {0,0,1,0},
  {0,0,1,1},
  {0,0,0,1},
  {1,0,0,1}
};

// ---------- BLE ----------
static const char* BLE_DEVICE_NAME = "BettaFeeder";
static const char* SERVICE_UUID = "7f510001-1b15-4f0b-9a2e-5b8f6a100001";
static const char* RX_UUID      = "7f510002-1b15-4f0b-9a2e-5b8f6a100001";
static const char* TX_UUID      = "7f510003-1b15-4f0b-9a2e-5b8f6a100001";

NimBLECharacteristic* txCharacteristic = nullptr;
bool bleConnected = false;

portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
char pendingCommand[192] = {0};
volatile bool commandPending = false;
volatile bool pushNetworkBusy = false;
SemaphoreHandle_t pushQueueMutex = nullptr;
TaskHandle_t pushWorkerHandle = nullptr;

// ---------- Persistent settings ----------
Preferences prefs;
String wifiSSID;
String wifiPASS;
uint16_t feedMinute[2] = {8 * 60, 20 * 60};
int lastFeedDate[2] = {0, 0};
uint32_t lastFeedEpoch = 0; // hora da ultima porcao efetivamente concluida

uint8_t rotorPosition = 0;     // 0 = empty safety pocket at outlet
uint8_t portionsDispensed = 0; // 0..14
uint32_t batchFeedEpochs[PORTIONS_PER_LOAD] = {0};
struct FeedHistoryRecord {
  uint32_t epoch;
  uint8_t success;
  uint8_t portion;
  char reason[40];
};
static const uint8_t FEED_HISTORY_CAPACITY = 32;
FeedHistoryRecord feedHistory[FEED_HISTORY_CAPACITY] = {};
uint8_t feedHistoryNext = 0;
uint8_t feedHistoryCount = 0;
bool awaitingReload = true; // fail-safe until a load is explicitly confirmed
bool referenceValid = false; // restored after a clean reboot; cleared if motion was interrupted
bool motionInProgress = false; // persisted before motor starts
bool feedInProgress = false;
bool startupScheduleChecked = false;
bool suppressOverdueAfterReload = false;
bool motorTestMode = false;
uint8_t motorTestShots = 0;
uint32_t motorTestNextEpoch = 0;
static const uint32_t MOTOR_TEST_INTERVALS_SECONDS[3] = {900UL, 1800UL, 3600UL};
String lastScheduleBlockLog;
bool clockAvailabilityLogged = false;
OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);
float waterTemperatureC = DEVICE_DISCONNECTED_C;
bool temperatureConversionPending = false;
unsigned long temperatureConversionStarted = 0;
unsigned long lastTemperatureRequest = 0;
bool lowTemperatureAlertActive = false;
uint32_t lastLowTemperatureAlertEpoch = 0;
bool highTemperatureAlertActive = false;
uint32_t lastHighTemperatureAlertEpoch = 0;

// ---------- Wi-Fi / clock ----------
bool ntpConfigured = false;
unsigned long lastWiFiAttempt = 0;
bool wifiConnecting = false;
static const unsigned long WIFI_RETRY_MS = 30000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
unsigned long wifiAttemptStarted = 0;
// FCM: varios navegadores podem receber notificacoes do mesmo alimentador.
static const uint8_t FCM_DEVICE_CAPACITY = 5;
String fcmTargetTokens[FCM_DEVICE_CAPACITY];
uint8_t fcmTargetCount = 0;
String pendingFcmToken;
bool pendingFcmRemoval = false;
size_t expectedFcmLength = 0;
String firebaseProjectId;
String firebaseClientEmail;
String firebasePrivateKey;
String pendingFirebaseProjectId;
String pendingFirebaseClientEmail;
String pendingFirebasePrivateKey;
size_t expectedFirebaseProjectLength = 0;
size_t expectedFirebaseEmailLength = 0;
size_t expectedFirebasePrivateKeyLength = 0;
bool firebaseConfigEditing = false;
String oauthAccessToken;
unsigned long oauthExpiresAtMs = 0;
String pendingPushEvent;
String pendingPushText;
struct QueuedPush {
  String event;
  String text;
};
static const uint8_t PUSH_QUEUE_CAPACITY = 8;
QueuedPush deferredPushes[PUSH_QUEUE_CAPACITY];
uint8_t deferredPushCount = 0;
unsigned long pushNextAttempt = 0;
unsigned long lastPushWifiLog = 0;
uint32_t lastEmptyReminderEpoch = 0;
static const unsigned long PUSH_RETRY_MS = 60000;
static const unsigned long PUSH_WIFI_WAIT_MS = 5000;
static const unsigned long FCM_VALIDATION_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;
static const unsigned long FCM_VALIDATION_RETRY_MS = 15UL * 60UL * 1000UL;
unsigned long nextFcmValidationAt = 0;
static const uint32_t EMPTY_REMINDER_INTERVAL_SECONDS = 60UL * 60UL;

void connectWiFiIfNeeded();

static const int LATE_GRACE_MINUTES = 30; // skip a missed meal after 30 min

// Compact persistent ring buffer in NVS. Keeps the latest 32 diagnostic events.
struct DeviceLogRecord { uint32_t epoch; char message[96]; };
static const uint8_t DEVICE_LOG_CAPACITY = 32;
uint8_t deviceLogNext = 0;
uint8_t deviceLogCount = 0;

String deviceLogKey(uint8_t index) { return String("log") + (index < 10 ? "0" : "") + String(index); }
String batchFeedKey(uint8_t index) { return String("feedTs") + index; }
String feedHistoryKey(uint8_t index) { return String("feedEvt") + (index < 10 ? "0" : "") + String(index); }

bool isValidFeedHistoryRecord(const FeedHistoryRecord& record) {
  if (record.success > 1 || record.portion > PORTIONS_PER_LOAD || record.reason[0] == '\0') return false;
  const char* knownReasons[] = {
    "success", "test_mode", "motor_busy", "position_unknown", "batch_empty",
    "position_mismatch", "movement_interrupted", "schedule_too_late", "time_unavailable"
  };
  for (const char* known : knownReasons) {
    if (strncmp(record.reason, known, sizeof(record.reason)) == 0) return true;
  }
  return false;
}

void appendFeedHistory(bool success, const char* reason, uint8_t portion = 0, uint32_t epoch = 0xFFFFFFFFUL) {
  if (epoch == 0xFFFFFFFFUL) {
    time_t now = time(nullptr);
    if (now >= 1735689600) epoch = static_cast<uint32_t>(now);
  }
  FeedHistoryRecord record{};
  record.epoch = epoch;
  record.success = success ? 1 : 0;
  record.portion = portion;
  String(reason).substring(0, sizeof(record.reason) - 1).toCharArray(record.reason, sizeof(record.reason));
  feedHistory[feedHistoryNext] = record;
  prefs.putBytes(feedHistoryKey(feedHistoryNext).c_str(), &record, sizeof(record));
  feedHistoryNext = (feedHistoryNext + 1) % FEED_HISTORY_CAPACITY;
  if (feedHistoryCount < FEED_HISTORY_CAPACITY) feedHistoryCount++;
  prefs.putUChar("feedNext", feedHistoryNext);
  prefs.putUChar("feedCount", feedHistoryCount);
}

void clearBatchFeedHistory() {
  feedHistoryNext = 0;
  feedHistoryCount = 0;
  for (uint8_t i = 0; i < FEED_HISTORY_CAPACITY; i++) {
    feedHistory[i] = FeedHistoryRecord{};
    prefs.remove(feedHistoryKey(i).c_str());
  }
  prefs.putUChar("feedNext", 0);
  prefs.putUChar("feedCount", 0);
  for (uint8_t i = 0; i < PORTIONS_PER_LOAD; i++) {
    batchFeedEpochs[i] = 0;
    prefs.putUInt(batchFeedKey(i).c_str(), 0);
  }
}

void emitBatchFeedHistory() {
  uint8_t validCount = 0;
  uint8_t first = (feedHistoryNext + FEED_HISTORY_CAPACITY - feedHistoryCount) % FEED_HISTORY_CAPACITY;
  for (uint8_t i = 0; i < feedHistoryCount; i++) {
    uint8_t slot = (first + i) % FEED_HISTORY_CAPACITY;
    if (isValidFeedHistoryRecord(feedHistory[slot])) validCount++;
  }
  notifyBLE(String("FEEDS_BEGIN:") + validCount);
  for (uint8_t i = 0; i < feedHistoryCount; i++) {
    uint8_t slot = (first + i) % FEED_HISTORY_CAPACITY;
    const FeedHistoryRecord& record = feedHistory[slot];
    if (!isValidFeedHistoryRecord(record)) continue;
    notifyBLE(String("FEED_EVENT:") + record.epoch + ":" + (record.success ? "OK" : "FAIL") +
      ":" + record.portion + ":" + record.reason);
    delay(25);
  }
  notifyBLE("FEEDS_END");
}

void saveDeviceLog(const String& message) {
  if (message.startsWith("Push:")) return;
  DeviceLogRecord record{};
  time_t now = time(nullptr);
  if (now >= 1735689600) record.epoch = static_cast<uint32_t>(now);
  String safe = message;
  safe.replace("\n", " "); safe.replace("\r", " ");
  safe.substring(0, sizeof(record.message) - 1).toCharArray(record.message, sizeof(record.message));
  prefs.putBytes(deviceLogKey(deviceLogNext).c_str(), &record, sizeof(record));
  deviceLogNext = (deviceLogNext + 1) % DEVICE_LOG_CAPACITY;
  if (deviceLogCount < DEVICE_LOG_CAPACITY) deviceLogCount++;
  prefs.putUChar("logNext", deviceLogNext);
  prefs.putUChar("logCount", deviceLogCount);
}

void emitDeviceLogs() {
  notifyBLE("LOGS_BEGIN");
  uint8_t first = (deviceLogNext + DEVICE_LOG_CAPACITY - deviceLogCount) % DEVICE_LOG_CAPACITY;
  for (uint8_t i = 0; i < deviceLogCount; i++) {
    DeviceLogRecord record{};
    uint8_t slot = (first + i) % DEVICE_LOG_CAPACITY;
    if (prefs.getBytes(deviceLogKey(slot).c_str(), &record, sizeof(record)) != sizeof(record)) continue;
    if (strncmp(record.message, "Push:", 5) == 0) continue;
    notifyBLE(String("LOG:") + String(record.epoch) + ":" + String(record.message));
    delay(25);
  }
  notifyBLE("LOGS_DONE");
}

// ---------- Helpers ----------
String twoDigits(int n) {
  return (n < 10 ? "0" : "") + String(n);
}

bool parseHHMM(const String& value, uint16_t& result) {
  if (value.length() != 5 || value[2] != ':' ||
      value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9' ||
      value[3] < '0' || value[3] > '9' || value[4] < '0' || value[4] > '9') return false;
  int h = value.substring(0, 2).toInt();
  int m = value.substring(3, 5).toInt();
  if (h > 23 || m > 59) return false;
  result = (uint16_t)(h * 60 + m);
  return true;
}

String formatHHMM(uint16_t value) {
  return twoDigits(value / 60) + ":" + twoDigits(value % 60);
}

int dateKey(const struct tm& t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

bool clockIsValid(struct tm* out = nullptr) {
  time_t now = time(nullptr);
  if (now < 1735689600) return false; // before 2025-01-01 means not synced
  if (out) {
    localtime_r(&now, out);
  }
  return true;
}

void notifyBLE(const String& message) {
  Serial.println(message);
  if (bleConnected && txCharacteristic) {
    // GATT notifications may be limited to 20 bytes (MTU 23).
    // Prefix '+' for intermediate 18-byte fragments, '=' for final.
    // This protocol is handled by the updated PWA v4.
    if (message.length() <= 19) {
      txCharacteristic->setValue(message.c_str());
      txCharacteristic->notify();
    } else {
      for (size_t offset = 0; offset < message.length(); offset += 18) {
        bool last = (offset + 18 >= message.length());
        String frag = String(last ? '=' : '+') + message.substring(offset, offset + 18);
        txCharacteristic->setValue(frag.c_str());
        txCharacteristic->notify();
        delay(35);
      }
    }
  }
}

void notifyPushLog(const String& message) {
  // Nunca inclua o token FCM, a chave privada ou o corpo completo de credenciais.
  saveDeviceLog(String("Push: ") + message);
  // Push diagnostics stay on USB serial only; they must not flood the PWA log
  // or delay BLE command handling with fragmented notifications.
  Serial.println(String("Push: ") + message);
}

void saveSettings() {
  // Token de notificacao fica no NVS, nunca em logs BLE.
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPASS);
  prefs.putUShort("feed1", feedMinute[0]);
  prefs.putUShort("feed2", feedMinute[1]);
  prefs.putInt("last1", lastFeedDate[0]);
  prefs.putInt("last2", lastFeedDate[1]);
  prefs.putUInt("lastFeed", lastFeedEpoch);
  prefs.putBool("reloadGuard", suppressOverdueAfterReload);
  prefs.putBool("motorTest", motorTestMode);
  prefs.putUChar("position", rotorPosition);
  prefs.putUChar("portions", portionsDispensed);
  prefs.putBool("reload", awaitingReload);
  prefs.putBool("homed", referenceValid);
  prefs.putBool("moving", motionInProgress);
  prefs.putUShort("steps", halfStepsPerRev);
  // Tokens FCM sao mantidos separadamente em fcm0..fcm4.
}

String fcmDeviceKey(uint8_t index) { return String("fcm") + index; }

bool saveFcmDevices() {
  for (uint8_t i = 0; i < FCM_DEVICE_CAPACITY; i++) {
    if (i < fcmTargetCount) {
      const size_t written = prefs.putString(fcmDeviceKey(i).c_str(), fcmTargetTokens[i]);
      if (written != fcmTargetTokens[i].length() || prefs.getString(fcmDeviceKey(i).c_str(), "") != fcmTargetTokens[i]) return false;
    } else prefs.remove(fcmDeviceKey(i).c_str());
  }
  prefs.putUChar("fcmCount", fcmTargetCount);
  prefs.remove("fcmToken"); // remove legado de token unico apos migracao
  return true;
}

bool upsertFcmToken(const String& newToken) {
  for (uint8_t i = 0; i < fcmTargetCount; i++) if (fcmTargetTokens[i] == newToken) return true;
  if (fcmTargetCount >= FCM_DEVICE_CAPACITY) return false;
  fcmTargetTokens[fcmTargetCount++] = newToken;
  if (saveFcmDevices()) return true;
  fcmTargetTokens[--fcmTargetCount] = "";
  return false;
}

bool removeFcmToken(const String& token) {
  for (uint8_t i = 0; i < fcmTargetCount; i++) {
    if (fcmTargetTokens[i] != token) continue;
    for (uint8_t j = i + 1; j < fcmTargetCount; j++) fcmTargetTokens[j - 1] = fcmTargetTokens[j];
    fcmTargetTokens[--fcmTargetCount] = "";
    return saveFcmDevices();
  }
  return true;
}

bool clearFcmDevices() {
  String previousTokens[FCM_DEVICE_CAPACITY];
  const uint8_t previousCount = fcmTargetCount;
  for (uint8_t i = 0; i < previousCount; i++) previousTokens[i] = fcmTargetTokens[i];
  for (uint8_t i = 0; i < FCM_DEVICE_CAPACITY; i++) fcmTargetTokens[i] = "";
  fcmTargetCount = 0;
  if (saveFcmDevices()) return true;
  fcmTargetCount = previousCount;
  for (uint8_t i = 0; i < previousCount; i++) fcmTargetTokens[i] = previousTokens[i];
  return false;
}

bool hasFcmDevices() { return fcmTargetCount > 0; }

void clearQueuedPushes();
bool hasPendingPush();

void clearFirebaseCredentialsIfNoDevices() {
  if (hasFcmDevices()) return;
  // Keep Firebase credentials so a replacement device can be registered without
  // requiring the user to enter the service account again.
  oauthAccessToken = ""; oauthExpiresAtMs = 0;
  notifyPushLog("nenhum dispositivo FCM cadastrado; credenciais Firebase mantidas");
}

void loadSettings() {
  prefs.begin("bettafeed", false);
  lowTemperatureAlertActive = prefs.getBool("tempLowOn", false);
  lastLowTemperatureAlertEpoch = prefs.getUInt("tempLowAt", 0);
  highTemperatureAlertActive = prefs.getBool("tempHighOn", false);
  lastHighTemperatureAlertEpoch = prefs.getUInt("tempHighAt", 0);
  wifiSSID = prefs.getString("ssid", "");
  fcmTargetCount = prefs.getUChar("fcmCount", 0);
  if (fcmTargetCount > FCM_DEVICE_CAPACITY) fcmTargetCount = 0;
  for (uint8_t i = 0; i < fcmTargetCount; i++) fcmTargetTokens[i] = prefs.getString(fcmDeviceKey(i).c_str(), "");
  if (fcmTargetCount == 0) {
    String legacyToken = prefs.getString("fcmToken", "");
    if (!legacyToken.isEmpty()) { fcmTargetTokens[0] = legacyToken; fcmTargetCount = 1; saveFcmDevices(); }
  }
  firebaseProjectId = prefs.getString("fbProject", "");
  firebaseClientEmail = prefs.getString("fbEmail", "");
  firebasePrivateKey = "";
  size_t savedKeyLength = prefs.getBytesLength("fbKeyBin");
  if (savedKeyLength > 1 && savedKeyLength <= 1901) {
    char* savedKey = (char*)malloc(savedKeyLength);
    if (savedKey) {
      if (prefs.getBytes("fbKeyBin", savedKey, savedKeyLength) == savedKeyLength) {
        firebasePrivateKey = String(savedKey);
      }
      free(savedKey);
    }
  }
  if (firebasePrivateKey.isEmpty()) firebasePrivateKey = prefs.getString("fbKey", "");
  wifiPASS = prefs.getString("pass", "");
  feedMinute[0] = prefs.getUShort("feed1", 8 * 60);
  feedMinute[1] = prefs.getUShort("feed2", 20 * 60);
  lastFeedDate[0] = prefs.getInt("last1", 0);
  lastFeedDate[1] = prefs.getInt("last2", 0);
  lastFeedEpoch = prefs.getUInt("lastFeed", 0);
  suppressOverdueAfterReload = prefs.getBool("reloadGuard", false);
  motorTestMode = prefs.getBool("motorTest", false);
  if (prefs.getUChar("feedVer", 0) < 1) {
    // Older firmware marked scheduled slots before attempting motor movement,
    // so those dates cannot prove that a portion was actually dispensed.
    lastFeedDate[0] = lastFeedDate[1] = 0;
    prefs.putUChar("feedVer", 1);
  }
  rotorPosition = prefs.getUChar("position", 0);
  portionsDispensed = prefs.getUChar("portions", 0);
  for (uint8_t i = 0; i < PORTIONS_PER_LOAD; i++) batchFeedEpochs[i] = prefs.getUInt(batchFeedKey(i).c_str(), 0);
  feedHistoryNext = prefs.getUChar("feedNext", 0) % FEED_HISTORY_CAPACITY;
  feedHistoryCount = prefs.getUChar("feedCount", 0);
  if (feedHistoryCount > FEED_HISTORY_CAPACITY) feedHistoryCount = FEED_HISTORY_CAPACITY;
  for (uint8_t i = 0; i < FEED_HISTORY_CAPACITY; i++) {
    prefs.getBytes(feedHistoryKey(i).c_str(), &feedHistory[i], sizeof(FeedHistoryRecord));
  }
  if (feedHistoryCount == 0) {
    uint8_t previousFeeds = portionsDispensed > PORTIONS_PER_LOAD ? PORTIONS_PER_LOAD : portionsDispensed;
    for (uint8_t i = 0; i < previousFeeds; i++) appendFeedHistory(true, "success", i + 1, batchFeedEpochs[i]);
  }
  awaitingReload = prefs.getBool("reload", true);
  lastEmptyReminderEpoch = prefs.getUInt("emptyReminder", 0);
  motionInProgress = prefs.getBool("moving", false);
  halfStepsPerRev = prefs.getUShort("steps", 4076);
  if (halfStepsPerRev < 3900 || halfStepsPerRev > 4200) halfStepsPerRev = 4076;
  // Reuse the saved position after a clean restart. A reset during movement
  // leaves `moving` set, so require manual alignment instead of moving at boot.
  referenceValid = prefs.getBool("homed", false);
  if (motionInProgress) {
    referenceValid = false;
    awaitingReload = true;
  }
  if (rotorPosition >= POCKETS) rotorPosition = 0;
  if (portionsDispensed > PORTIONS_PER_LOAD) {
    portionsDispensed = PORTIONS_PER_LOAD;
    awaitingReload = true;
  }
}

void releaseMotor() {
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(MOTOR_PINS[i], LOW);
  }
}

void doHalfStep(bool forward) {
  static int seqIndex = 0;
  if (forward) seqIndex = (seqIndex + 1) % 8;
  else seqIndex = (seqIndex + 7) % 8;

  for (uint8_t pin = 0; pin < 4; pin++) {
    digitalWrite(MOTOR_PINS[pin], HALF_STEP_SEQ[seqIndex][pin]);
  }
  delay(STEP_DELAY_MS);
}

// Number of half-steps from home (position 0) to a pocket index.
// Rounded absolute positions avoid cumulative 273-step rounding drift.
uint16_t stepsAtPosition(uint8_t position) {
  return (uint16_t)lround((double)position * halfStepsPerRev / POCKETS);
}

void rotateToNextPocket(const char* source, bool settleForFood = true) {
  motionInProgress = true;
  saveSettings(); // power failure during rotation will be flagged
  uint8_t oldPosition = rotorPosition;
  uint8_t newPosition = (uint8_t)((rotorPosition + 1) % POCKETS);

  uint16_t oldSteps = stepsAtPosition(oldPosition);
  uint16_t newSteps = stepsAtPosition(newPosition);
  uint16_t stepsToMove;

  if (newPosition == 0) {
    stepsToMove = halfStepsPerRev - oldSteps;
  } else {
    stepsToMove = newSteps - oldSteps;
  }

  for (uint16_t i = 0; i < stepsToMove; i++) {
    doHalfStep(MOTOR_FORWARD);
  }
  releaseMotor();

  rotorPosition = newPosition;
  motionInProgress = false;
  saveSettings();
  saveDeviceLog(String("Sequencia de passos do motor executada (") + source +"); posicao solicitada=" + String(newPosition) + ".");
  if (settleForFood) delay(250); // allow pellets to fall before another feeding movement
}

bool jogMotor(bool forward) {
  if (feedInProgress || motionInProgress) {
    notifyBLE("BLOQUEADO: motor ocupado.");
    return false;
  }
  // A jog changes the physical rotor angle without a sensor to track it.
  // Invalidate the old pocket reference until the user aligns and sets home.
  motionInProgress = true;
  referenceValid = false;
  saveSettings();
  for (uint8_t i = 0; i < JOG_HALF_STEPS; i++) doHalfStep(forward);
  releaseMotor();
  motionInProgress = false;
  saveSettings();
  saveDeviceLog(String("JOG ") + (forward ? "direita" : "esquerda") + ": sequencia de passos do motor executada; referencia invalidada.");
  return true;
}


// ---------- FCM HTTP v1: OAuth service account no proprio ESP32 ----------
String b64url(const uint8_t* input, size_t len) {
  size_t cap = 4 * ((len + 2) / 3) + 8;
  std::unique_ptr<unsigned char[]> buf(new unsigned char[cap]);
  size_t written = 0;
  if (mbedtls_base64_encode(buf.get(), cap, &written, input, len) != 0) return "";
  String result;
  result.reserve(written);
  for (size_t i = 0; i < written; i++) {
    char c = char(buf[i]);
    if (c == '+') c = '-'; else if (c == '/') c = '_';
    if (c != '=') result += c;
  }
  return result;
}
String b64urlText(const String& s) {
  return b64url((const uint8_t*)s.c_str(), s.length());
}
String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}
bool firebaseConfigured() {
  return !firebaseProjectId.isEmpty() && firebaseClientEmail.indexOf('@') > 0 &&
         firebasePrivateKey.indexOf("-----BEGIN PRIVATE KEY-----") >= 0 &&
         firebasePrivateKey.indexOf("-----END PRIVATE KEY-----") >= 0;
}
bool pushDeliveryConfigured() {
  return firebaseConfigured() && hasFcmDevices();
}
String signedJwt() {
  if (!firebaseConfigured() || !clockIsValid()) return "";
  time_t now = time(nullptr);
  String header = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
  String claims = "{\"iss\":\"" + jsonEscape(firebaseClientEmail) +
    "\",\"scope\":\"https://www.googleapis.com/auth/firebase.messaging\",\"aud\":\"https://oauth2.googleapis.com/token\",\"iat\":" +
    String((uint32_t)now) + ",\"exp\":" + String((uint32_t)(now + 3500)) + "}";
  String input = b64urlText(header) + "." + b64urlText(claims);
  if (input.length() < 10) return "";
  unsigned char hash[32];
#if MBEDTLS_VERSION_MAJOR >= 3
  if (mbedtls_sha256((const unsigned char*)input.c_str(), input.length(), hash, 0) != 0) return "";
#else
  if (mbedtls_sha256_ret((const unsigned char*)input.c_str(), input.length(), hash, 0) != 0) return "";
#endif
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  firebasePrivateKey.replace("\\n", "\n");
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_context rng;
  mbedtls_ctr_drbg_init(&rng);
  const char* personal = "betta-fcm";
  int rc = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
                        (const unsigned char*)personal, strlen(personal));
  if (rc == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
    rc = mbedtls_pk_parse_key(&pk, (const unsigned char*)firebasePrivateKey.c_str(),
           firebasePrivateKey.length() + 1, nullptr, 0,
           mbedtls_ctr_drbg_random, &rng);
#else
    rc = mbedtls_pk_parse_key(&pk, (const unsigned char*)firebasePrivateKey.c_str(),
           firebasePrivateKey.length() + 1, nullptr, 0);
#endif
  }
  unsigned char signature[512];
  size_t sigLen = 0;
  if (rc == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
    rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature,
          sizeof(signature), &sigLen, mbedtls_ctr_drbg_random, &rng);
#else
    rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature,
          &sigLen, mbedtls_ctr_drbg_random, &rng);
#endif
  }
  mbedtls_pk_free(&pk);
  mbedtls_ctr_drbg_free(&rng);
  mbedtls_entropy_free(&entropy);
  if (rc != 0) { Serial.printf("FCM: erro ao assinar JWT (%d)\n", rc); return ""; }
  return input + "." + b64url(signature, sigLen);
}
// Uma extracao limitada: a resposta do Google contem 'access_token' como string JSON.
String extractJsonString(const String& json, const String& field) {
  int i = json.indexOf("\"" + field + "\"");
  if (i < 0) return "";
  i = json.indexOf(':', i); if (i < 0) return "";
  i = json.indexOf('"', i); if (i < 0) return "";
  int j = json.indexOf('"', i+1); if (j < 0) return "";
  return json.substring(i+1, j);
}
const char* httpErrorName(int code) {
  switch (code) {
    case -1: return "connection refused";
    case -2: return "send header failed";
    case -3: return "send payload failed";
    case -4: return "not connected";
    case -5: return "connection lost";
    case -6: return "no stream";
    case -7: return "no HTTP server";
    case -8: return "too little RAM";
    case -11: return "read timeout";
    default: return "transporte desconhecido";
  }
}
String tlsErrorText(WiFiClientSecure& secure) {
  char error[128] = {0};
  secure.lastError(error, sizeof(error));
  return error[0] ? String(error) : "sem detalhe TLS";
}
void notifyTlsError(WiFiClientSecure& secure, const String& prefix) {
  notifyPushLog(prefix + tlsErrorText(secure));
}
bool isTlsMemoryError(WiFiClientSecure& secure) {
  String error = tlsErrorText(secure);
  return error.indexOf("Memory allocation failed") >= 0 ||
         error.indexOf("memory allocation failed") >= 0;
}
void configureTlsClient(WiFiClientSecure& secure) {
  // O bundle de CAs usa memoria adicional durante o handshake. Com BLE
  // ativo, o ESP32 pode ficar com pouca heap e falhar antes de abrir o
  // socket (BIGNUM/Memory allocation failed). Nesse caso, evita manter um
  // segundo cliente TLS de fallback vivo e usa TLS sem validacao de CA.
  // Em heap normal, a cadeia de certificados continua sendo validada.
  if (ESP.getFreeHeap() < 70000) {
    notifyPushLog(String("TLS: heap baixa (") + ESP.getFreeHeap() + "); sem validacao de CA");
    secure.setInsecure();
  } else {
    secure.useBuiltinCACertBundle();
  }
  secure.setHandshakeTimeout(15);
  secure.setTimeout(15000);
}
String oauthToken() {
  if (!oauthAccessToken.isEmpty() && (int32_t)(oauthExpiresAtMs - millis()) > 120000) {
    notifyPushLog("OAuth: token em cache reutilizado");
    return oauthAccessToken;
  }
  notifyPushLog("OAuth: solicitando novo access token");
  notifyPushLog(String("OAuth: IP=") + WiFi.localIP().toString() +
                ", RSSI=" + WiFi.RSSI() + ", heap=" + ESP.getFreeHeap());
  String jwt = signedJwt();
  if (jwt.isEmpty()) {
    notifyPushLog("OAuth: JWT vazio; verifique Firebase, RTC e chave privada");
    return "";
  }
  bool tlsLowMemory = ESP.getFreeHeap() < 70000;
  WiFiClientSecure secure;
  configureTlsClient(secure); // mantém validação do certificado; sem setInsecure()
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(secure, "https://oauth2.googleapis.com/token")) {
    notifyPushLog("OAuth: falha ao abrir conexao TLS");
    return "";
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String requestBody = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt;
  // O assertion ja foi copiado para o corpo; libere a copia temporaria antes
  // de alocar os buffers do handshake TLS.
  jwt = "";
  int code = http.POST(requestBody);
  String response = (code > 0) ? http.getString() : "";
  bool retryWithoutCa = !tlsLowMemory && code < 0 && isTlsMemoryError(secure);
  if (code < 0) notifyTlsError(secure, String("OAuth transporte: ") + httpErrorName(code) + "; ");
  http.end();
  if (retryWithoutCa) {
    notifyPushLog("OAuth: heap insuficiente para X509; tentando TLS sem validacao de CA");
    WiFiClientSecure fallback;
    fallback.setInsecure();
    fallback.setHandshakeTimeout(15);
    fallback.setTimeout(15000);
    HTTPClient retry;
    retry.setTimeout(12000);
    if (retry.begin(fallback, "https://oauth2.googleapis.com/token")) {
      retry.addHeader("Content-Type", "application/x-www-form-urlencoded");
      code = retry.POST(requestBody);
      response = (code > 0) ? retry.getString() : "";
      if (code < 0) notifyTlsError(fallback, String("OAuth fallback transporte: ") + httpErrorName(code) + "; ");
      retry.end();
      notifyPushLog(String("OAuth fallback HTTP ") + code + ", heap=" + ESP.getFreeHeap());
    }
  }
  if (code != 200) {
    Serial.printf("FCM: OAuth HTTP %d\n", code);
    notifyPushLog(String("OAuth HTTP ") + code + " (" + httpErrorName(code) + ")");
    return "";
  }
  oauthAccessToken = extractJsonString(response, "access_token");
  oauthExpiresAtMs = millis() + 3300000UL;
  notifyPushLog(oauthAccessToken.isEmpty() ? "OAuth: resposta sem access_token" : "OAuth: access token obtido");
  return oauthAccessToken;
}
bool deliverPushToToken(const String& title, const String& body, const String& targetToken,
                        bool& tokenInvalid, bool validateOnly = false) {
  tokenInvalid = false;
  if (!hasFcmDevices()) {
    notifyPushLog("envio bloqueado: token FCM vazio");
    return false;
  }
  if (!firebaseConfigured()) {
    notifyPushLog("envio bloqueado: Firebase nao configurado");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    notifyPushLog("envio aguardando: Wi-Fi desconectado");
    return false;
  }
  if (!clockIsValid()) {
    notifyPushLog("envio aguardando: horario NTP invalido");
    return false;
  }
  notifyPushLog(validateOnly ? "validando destino FCM" : "iniciando envio FCM");
  String token = oauthToken();
  if (token.isEmpty()) {
    notifyPushLog("envio abortado: OAuth sem token");
    return false;
  }
  bool tlsLowMemory = ESP.getFreeHeap() < 70000;
  WiFiClientSecure secure;
  configureTlsClient(secure);
  HTTPClient http;
  http.setTimeout(12000);
  String url = "https://fcm.googleapis.com/v1/projects/" + firebaseProjectId + "/messages:send";
  if (!http.begin(secure, url)) {
    notifyPushLog("FCM: falha ao abrir conexao TLS");
    return false;
  }
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  // Mensagem data-only: o onBackgroundMessage do Firebase no Service Worker
  // exibe a notificacao nativa mesmo quando a aba estiver fechada.
  String payload = validateOnly
    ? "{\"validate_only\":true,\"message\":{\"token\":\""
    : "{\"message\":{\"token\":\"";
  payload += jsonEscape(targetToken) +
            "\",\"data\":{\"title\":\"" + jsonEscape(title) +
            "\",\"body\":\"" + jsonEscape(body) +
            "\"},\"webpush\":{\"headers\":{\"Urgency\":\"high\"}}}}";
  int code = http.POST(payload);
  String response = (code > 0) ? http.getString() : "";
  bool retryWithoutCa = !tlsLowMemory && code < 0 && isTlsMemoryError(secure);
  if (code < 0) notifyTlsError(secure, String("FCM transporte: ") + httpErrorName(code) + "; ");
  http.end();
  if (retryWithoutCa) {
    notifyPushLog("FCM: heap insuficiente para X509; tentando TLS sem validacao de CA");
    WiFiClientSecure fallback;
    fallback.setInsecure();
    fallback.setHandshakeTimeout(15);
    fallback.setTimeout(15000);
    HTTPClient retry;
    retry.setTimeout(12000);
    if (retry.begin(fallback, url)) {
      retry.addHeader("Authorization", "Bearer " + token);
      retry.addHeader("Content-Type", "application/json; charset=utf-8");
      code = retry.POST(payload);
      response = (code > 0) ? retry.getString() : "";
      if (code < 0) notifyTlsError(fallback, String("FCM fallback transporte: ") + httpErrorName(code) + "; ");
      retry.end();
      notifyPushLog(String("FCM fallback HTTP ") + code + ", heap=" + ESP.getFreeHeap());
    }
  }
  if (code == 401) { oauthAccessToken = ""; oauthExpiresAtMs = 0; }
  tokenInvalid = (code == 404 &&
    (response.indexOf("UNREGISTERED") >= 0 || response.indexOf("registration-token-not-registered") >= 0)) ||
    (code == 400 && response.indexOf("google.firebase.fcm.v1.FcmError") >= 0 &&
     response.indexOf("INVALID_ARGUMENT") >= 0);
  Serial.printf("FCM: envio HTTP %d\n", code);
  notifyPushLog(String("FCM HTTP ") + code);
  if (code != 200) {
    String detail = response;
    if (detail.length() > 180) detail = detail.substring(0, 180);
    if (!detail.isEmpty()) notifyPushLog(String("FCM erro: ") + detail);
  }
  return code == 200;
}

bool deliverPush(const String& title, const String& body) {
  if (!hasFcmDevices()) return false;
  bool allActiveDevicesDelivered = true;
  uint8_t count = fcmTargetCount;
  String invalidTokens[FCM_DEVICE_CAPACITY];
  uint8_t invalidCount = 0;
  for (uint8_t i = 0; i < count; i++) {
    notifyPushLog(String("enviando para dispositivo ") + (i + 1) + "/" + count);
    bool tokenInvalid = false;
    const String targetToken = fcmTargetTokens[i];
    bool sent = deliverPushToToken(title, body, targetToken, tokenInvalid);
    if (!sent && !tokenInvalid) allActiveDevicesDelivered = false;
    if (tokenInvalid) invalidTokens[invalidCount++] = targetToken;
  }
  for (uint8_t i = 0; i < invalidCount; i++) {
    if (removeFcmToken(invalidTokens[i])) {
      notifyPushLog(String("token FCM expirado removido; restantes=") + fcmTargetCount);
    } else {
      notifyPushLog("falha ao remover token FCM expirado do NVS");
    }
  }
  clearFirebaseCredentialsIfNoDevices();
  return hasFcmDevices() && allActiveDevicesDelivered;
}

void processFcmMaintenance() {
  if (!hasFcmDevices()) {
    nextFcmValidationAt = 0;
    return;
  }
  if (hasPendingPush()) return;
  if ((int32_t)(millis() - nextFcmValidationAt) < 0) return;

  connectWiFiIfNeeded();
  if (WiFi.status() != WL_CONNECTED || !clockIsValid()) {
    nextFcmValidationAt = millis() + PUSH_WIFI_WAIT_MS;
    return;
  }

  notifyPushLog("validacao periodica dos destinos FCM iniciada");
  uint8_t count = fcmTargetCount;
  String invalidTokens[FCM_DEVICE_CAPACITY];
  uint8_t invalidCount = 0;
  bool retryNeeded = false;
  for (uint8_t i = 0; i < count; i++) {
    bool tokenInvalid = false;
    const String targetToken = fcmTargetTokens[i];
    bool valid = deliverPushToToken("BettaFeeder", "Validacao de destino.",
                                    targetToken, tokenInvalid, true);
    if (tokenInvalid) invalidTokens[invalidCount++] = targetToken;
    else if (!valid) retryNeeded = true;
  }
  for (uint8_t i = 0; i < invalidCount; i++) {
    if (removeFcmToken(invalidTokens[i])) {
      notifyPushLog(String("destino FCM sem registro removido; restantes=") + fcmTargetCount);
    } else {
      notifyPushLog("falha ao remover destino FCM sem registro do NVS");
      retryNeeded = true;
    }
  }
  clearFirebaseCredentialsIfNoDevices();
  nextFcmValidationAt = millis() +
    (retryNeeded ? FCM_VALIDATION_RETRY_MS : FCM_VALIDATION_INTERVAL_MS);
}
// Fila em RAM para reenvio periodico; reiniciar pode perder eventos pendentes.

void promoteNextPushLocked() {
  if (!pendingPushEvent.isEmpty() || deferredPushCount == 0) return;
  pendingPushEvent = deferredPushes[0].event;
  pendingPushText = deferredPushes[0].text;
  for (uint8_t i = 1; i < deferredPushCount; i++) deferredPushes[i - 1] = deferredPushes[i];
  deferredPushCount--;
  pushNextAttempt = 0;
}

void clearQueuedPushesLocked() {
  pendingPushEvent = "";
  pendingPushText = "";
  for (uint8_t i = 0; i < PUSH_QUEUE_CAPACITY; i++) {
    deferredPushes[i].event = "";
    deferredPushes[i].text = "";
  }
  deferredPushCount = 0;
  pushNextAttempt = 0;
}

void queuePush(const String& event, const String& text) {
  if (!pushDeliveryConfigured()) return;
  bool updated = false;
  bool dropped = false;
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  if (pendingPushEvent.isEmpty()) {
    pendingPushEvent = event;
    pendingPushText = text;
    pushNextAttempt = 0;
  } else if (pendingPushEvent == event) {
    pendingPushText = text;
    pushNextAttempt = 0;
  } else {
    for (uint8_t i = 0; i < deferredPushCount; i++) {
      if (deferredPushes[i].event == event) {
        deferredPushes[i].text = text;
        updated = true;
        break;
      }
    }
    if (!updated && deferredPushCount >= PUSH_QUEUE_CAPACITY) {
      dropped = true;
    } else if (!updated) {
      deferredPushes[deferredPushCount].event = event;
      deferredPushes[deferredPushCount].text = text;
      deferredPushCount++;
    }
  }
  xSemaphoreGive(pushQueueMutex);
  if (dropped) notifyPushLog(String("fila cheia; evento descartado: ") + event);
  else notifyPushLog(String(updated ? "evento atualizado na fila: " : "evento enfileirado: ") + event);
}

bool hasQueuedPush(const String& event) {
  bool found = false;
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  if (pendingPushEvent == event) found = true;
  for (uint8_t i = 0; i < deferredPushCount; i++) {
    if (deferredPushes[i].event == event) found = true;
  }
  xSemaphoreGive(pushQueueMutex);
  return found;
}

bool hasPendingPush() {
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  const bool pending = !pendingPushEvent.isEmpty();
  xSemaphoreGive(pushQueueMutex);
  return pending;
}

void cancelQueuedPush(const String& event) {
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  if (pendingPushEvent == event) {
    pendingPushEvent = "";
    pendingPushText = "";
    pushNextAttempt = 0;
  }
  for (uint8_t i = 0; i < deferredPushCount;) {
    if (deferredPushes[i].event != event) {
      i++;
      continue;
    }
    for (uint8_t j = i + 1; j < deferredPushCount; j++) deferredPushes[j - 1] = deferredPushes[j];
    deferredPushCount--;
  }
  promoteNextPushLocked();
  xSemaphoreGive(pushQueueMutex);
}

void clearQueuedPushes() {
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  clearQueuedPushesLocked();
  xSemaphoreGive(pushQueueMutex);
}

void processPush() {
  String deliveredEvent;
  String deliveredText;
  if (!pushDeliveryConfigured()) {
    xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
    if (!pendingPushEvent.isEmpty() || deferredPushCount > 0) clearQueuedPushesLocked();
    xSemaphoreGive(pushQueueMutex);
    return;
  }
  xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
  const bool hasDuePush = !pendingPushEvent.isEmpty() &&
    (int32_t)(millis() - pushNextAttempt) >= 0;
  if (hasDuePush) {
    deliveredEvent = pendingPushEvent;
    deliveredText = pendingPushText;
  }
  xSemaphoreGive(pushQueueMutex);
  if (!hasDuePush) return;

  // Primeiro inicia/continua a conexao Wi-Fi. O loop antigo tentava o FCM
  // antes desta funcao e acabava adiando o push por 60 segundos.
  connectWiFiIfNeeded();
  if (WiFi.status() != WL_CONNECTED || !clockIsValid()) {
    unsigned long now = millis();
    xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
    if (pendingPushEvent == deliveredEvent && pendingPushText == deliveredText)
      pushNextAttempt = now + PUSH_WIFI_WAIT_MS;
    xSemaphoreGive(pushQueueMutex);
    if (now - lastPushWifiLog >= 15000 || lastPushWifiLog == 0) {
      lastPushWifiLog = now;
      notifyPushLog(WiFi.status() != WL_CONNECTED
        ? "envio aguardando: Wi-Fi desconectado"
        : "envio aguardando: NTP ainda nao sincronizado");
    }
    return;
  }

  if (deliverPush("BettaFeeder", deliveredText)) {
    xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
    if (pendingPushEvent == deliveredEvent && pendingPushText == deliveredText) {
      pendingPushEvent = "";
      pendingPushText = "";
      promoteNextPushLocked();
    }
    xSemaphoreGive(pushQueueMutex);
    if (deliveredEvent == "EMPTY" || deliveredEvent == "EMPTY_REMINDER") {
      lastEmptyReminderEpoch = static_cast<uint32_t>(time(nullptr));
      prefs.putUInt("emptyReminder", lastEmptyReminderEpoch);
    }
    notifyPushLog("push entregue com sucesso");
    nextFcmValidationAt = millis() + FCM_VALIDATION_INTERVAL_MS;
  } else {
    xSemaphoreTake(pushQueueMutex, portMAX_DELAY);
    if (pendingPushEvent == deliveredEvent && pendingPushText == deliveredText)
      pushNextAttempt = millis() + PUSH_RETRY_MS;
    xSemaphoreGive(pushQueueMutex);
    notifyPushLog(String("push falhou; nova tentativa em ") + (PUSH_RETRY_MS / 1000) + "s");
  }
}

void processEmptyLoadReminder() {
  if (!pushDeliveryConfigured() || !awaitingReload ||
      portionsDispensed < PORTIONS_PER_LOAD || !clockIsValid()) return;
  if (hasQueuedPush("EMPTY") || hasQueuedPush("EMPTY_REMINDER")) return;

  const uint32_t nowEpoch = static_cast<uint32_t>(time(nullptr));
  if (lastEmptyReminderEpoch != 0 &&
      static_cast<uint32_t>(nowEpoch - lastEmptyReminderEpoch) < EMPTY_REMINDER_INTERVAL_SECONDS) return;

  queuePush("EMPTY_REMINDER", "O alimentador está sem porções. Reabasteça e confirme a recarga no App.");
}

bool feedOnePortion(const char* source, bool testMode = false) {
  uint8_t nextPortion = portionsDispensed < PORTIONS_PER_LOAD ? portionsDispensed + 1 : 0;
  if (feedInProgress || motionInProgress) {
    appendFeedHistory(false, "motor_busy", nextPortion);
    saveDeviceLog(String("Alimentacao bloqueada (") + source + "): motor ocupado.");
    notifyBLE("BLOQUEADO: motor ocupado."); return false;
  }
  if (!referenceValid) {
    appendFeedHistory(false, "position_unknown", nextPortion);
    saveDeviceLog(String("Alimentacao bloqueada (") + source + "): referencia desconhecida.");
    notifyBLE("BLOQUEADO: referencia desconhecida. Confira a posicao fisica e envie SET_HOME."); return false;
  }
  if (awaitingReload || portionsDispensed >= PORTIONS_PER_LOAD) {
    appendFeedHistory(false, "batch_empty", 0);
    saveDeviceLog(String("Alimentacao bloqueada (") + source + "): lote aguardando recarga.");
    notifyBLE("LOTE_VAZIO: alimentador parado. Reabasteca e confirme a recarga no PWA.");
    return false;
  }
  const bool firstFeedAfterReload = portionsDispensed == 0 && rotorPosition == PORTIONS_PER_LOAD;
  if (rotorPosition != portionsDispensed && !firstFeedAfterReload) {
    appendFeedHistory(false, "position_mismatch", nextPortion);
    referenceValid = false;
    saveSettings();
    notifyBLE("FALHA: indice inconsistente. Confira fisicamente e envie SET_HOME."); return false;
  }
  feedInProgress = true;
  // After refill at pocket 14, this scheduled feeding advances over the
  // empty safety pocket 0 and then dispenses pocket 1. No movement occurs at
  // refill confirmation or during boot.
  if (firstFeedAfterReload) rotateToNextPocket(source);
  rotateToNextPocket(source);
  portionsDispensed++;
  time_t feedTime = time(nullptr);
  lastFeedEpoch = feedTime >= 1735689600 ? static_cast<uint32_t>(feedTime) : 0;
  batchFeedEpochs[portionsDispensed - 1] = lastFeedEpoch;
  prefs.putUInt(batchFeedKey(portionsDispensed - 1).c_str(), lastFeedEpoch);
  appendFeedHistory(true, testMode ? "test_mode" : "success", portionsDispensed, lastFeedEpoch);
  saveSettings();
  saveDeviceLog(String("Sequencia de passos do motor executada (") + source + "); lote=" + String(portionsDispensed) + "/14.");
  notifyBLE(String("ALIMENTACAO: ") + source + "; porcoes_no_lote=" + String(portionsDispensed) + "/14");
  queuePush("FEED_COMMANDED", String(testMode ? "MODO TESTE: " : "") + "Sequencia de passos do motor executada, porcao " + String(portionsDispensed) +
            "/14. Queda da racao nao confirmada pelo ESP32.");
  if (portionsDispensed == PORTIONS_PER_LOAD) {
    // Stop at pocket 14 until the owner refills and confirms through the PWA.
    awaitingReload = true;
    saveSettings();
    notifyBLE("LOTE_FINALIZADO: sequencia do motor da porcao 14 executada. Verifique e reabasteca o alimentador.");
    queuePush("EMPTY", "Sequencia do motor da porcao 14 executada. Verifique e reabasteca o alimentador.");
  }
  feedInProgress = false;
  return true;
}

void processMotorTestSchedule() {
  if (!motorTestMode) return;
  if (!referenceValid) {
    motorTestMode = false;
    motorTestNextEpoch = 0;
    saveSettings();
    saveDeviceLog("Teste do motor cancelado: referencia do rotor invalida.");
    notifyBLE(String("MOTOR_TEST_STATE:OFF:") + motorTestShots);
    return;
  }
  if (motorTestShots >= 9 || feedInProgress || motionInProgress) return;

  time_t currentTime = time(nullptr);
  if (currentTime < 1735689600) return;
  uint32_t nowEpoch = static_cast<uint32_t>(currentTime);
  if (motorTestNextEpoch == 0) {
    // Quarter-hour boundaries are also local quarter hours in BRT (UTC-3).
    motorTestNextEpoch = (nowEpoch / 900UL + 1UL) * 900UL;
    notifyBLE(String("MOTOR_TEST_NEXT:") + motorTestNextEpoch);
  }
  if (nowEpoch < motorTestNextEpoch) return;

  const uint32_t scheduledEpoch = motorTestNextEpoch;
  if (!feedOnePortion("TEST_MODE", true)) {
    motorTestMode = false;
    motorTestNextEpoch = 0;
    saveSettings();
    saveDeviceLog("Teste do motor interrompido: fluxo de alimentacao recusou o disparo.");
    notifyBLE(String("MOTOR_TEST_STATE:OFF:") + motorTestShots);
    return;
  }
  motorTestShots++;
  uint32_t actualEpoch = static_cast<uint32_t>(time(nullptr));
  if (motorTestShots < 9) {
    const uint8_t nextStage = motorTestShots / 3;
    const uint32_t nextInterval = MOTOR_TEST_INTERVALS_SECONDS[nextStage];
    motorTestNextEpoch = scheduledEpoch + nextInterval;
    if (actualEpoch >= motorTestNextEpoch) {
      // Do not fire missed test slots back-to-back if other work delayed the loop.
      motorTestNextEpoch = (actualEpoch / nextInterval + 1UL) * nextInterval;
    }
  } else motorTestNextEpoch = 0;
  saveSettings();
  time_t scheduledTime = scheduledEpoch;
  struct tm scheduledTm;
  localtime_r(&scheduledTime, &scheduledTm);
  String shotLog = String("Teste motor disparo ") + motorTestShots + "/9; previsto " +
    twoDigits(scheduledTm.tm_hour) + ":" + twoDigits(scheduledTm.tm_min);
  if (motorTestShots < 9) {
    time_t nextTime = motorTestNextEpoch;
    struct tm nextTm;
    localtime_r(&nextTime, &nextTm);
    shotLog += String("; proximo ") + twoDigits(nextTm.tm_hour) + ":" + twoDigits(nextTm.tm_min);
  } else {
    shotLog += "; ultimo disparo";
  }
  saveDeviceLog(shotLog);
  notifyBLE(String("MOTOR_TEST_SHOT:") + motorTestShots + "/9:" + motorTestNextEpoch);

  if (motorTestShots >= 9) {
    motorTestMode = false;
    motorTestNextEpoch = 0;
    saveSettings();
    saveDeviceLog("Teste do motor concluido. Os disparos foram contabilizados como porcoes pelo fluxo de producao.");
    notifyBLE(String("MOTOR_TEST_STATE:OFF:") + motorTestShots);
  } else {
    notifyBLE(String("MOTOR_TEST_NEXT:") + motorTestNextEpoch);
  }
}

void connectWiFiIfNeeded() {
  if (wifiSSID.isEmpty()) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    if (!ntpConfigured) {
      // Brazil standard time: UTC-3. Brazil currently has no daylight saving time.
      configTzTime("BRT3", "pool.ntp.org", "time.google.com", "br.pool.ntp.org");
      ntpConfigured = true;
      notifyBLE("NTP: sincronizacao solicitada.");
    }
    return;
  }

  unsigned long now = millis();
  if (wifiConnecting) {
    if (now - wifiAttemptStarted > WIFI_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect();
      wifiConnecting = false;
      lastWiFiAttempt = now;
      notifyBLE("WIFI: tempo esgotado; nova tentativa em 30 segundos.");
    }
    return;
  }

  if (now - lastWiFiAttempt >= WIFI_RETRY_MS || lastWiFiAttempt == 0) {
    lastWiFiAttempt = now;
    wifiAttemptStarted = now;
    wifiConnecting = true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
    notifyBLE("WIFI: tentando conectar a " + wifiSSID);
  }
}

void processSchedule() {
  if (motorTestMode) {
    processMotorTestSchedule();
    return;
  }

  struct tm nowTm;
  if (!clockIsValid(&nowTm)) {
    if (!clockAvailabilityLogged) {
      saveDeviceLog("Agendamento suspenso: aguardando sincronizacao do horario NTP.");
      clockAvailabilityLogged = true;
    }
    return;
  }
  if (clockAvailabilityLogged) {
    saveDeviceLog("Agendamento restabelecido apos sincronizacao do horario NTP.");
    clockAvailabilityLogged = false;
  }

  int today = dateKey(nowTm);
  int currentMinute = nowTm.tm_hour * 60 + nowTm.tm_min;

  if (suppressOverdueAfterReload) {
    for (int slot = 0; slot < 2; slot++) {
      if (currentMinute >= feedMinute[slot] && lastFeedDate[slot] != today) {
        lastFeedDate[slot] = today;
        saveDeviceLog(String("Horario ") + (slot + 1) +
          " ja vencido na confirmacao da recarga; proxima porcao aguardara o proximo horario configurado.");
      }
    }
    suppressOverdueAfterReload = false;
    saveSettings();
  }

  // A restart does not discard elapsed slots: compare each due slot with the
  // persisted time of the last completed portion before deciding to feed.
  if (!startupScheduleChecked) {
    startupScheduleChecked = true;
  }

  if (!referenceValid || awaitingReload || motionInProgress) {
    String reason = !referenceValid ? "referencia desconhecida" :
      (awaitingReload ? "aguardando recarga" : "movimento em andamento");
    for (int slot = 0; slot < 2; slot++) {
      if (lastFeedDate[slot] != today && currentMinute >= feedMinute[slot]) {
        String signature = String(slot) + ":" + reason + ":" + String(today);
        if (signature != lastScheduleBlockLog) {
          const char* reasonCode = !referenceValid ? "position_unknown" :
            (awaitingReload ? "batch_empty" : "movement_interrupted");
          appendFeedHistory(false, reasonCode, portionsDispensed < PORTIONS_PER_LOAD ? portionsDispensed + 1 : 0);
          saveDeviceLog(String("Horario ") + (slot + 1) + " devido, mas bloqueado: " + reason + ".");
          lastScheduleBlockLog = signature;
        }
      }
    }
    return;
  }
  for (int slot = 0; slot < 2; slot++) {
    if (lastFeedDate[slot] == today) continue;
    if (currentMinute < feedMinute[slot]) continue;

    // If the latest completed feeding was at or after this slot today, that
    // meal is already covered, including when it happened before a reboot.
    bool coveredByLastFeeding = false;
    String lastFeedClock;
    if (lastFeedEpoch) {
      struct tm lastFeedTm;
      time_t lastFeedTime = static_cast<time_t>(lastFeedEpoch);
      localtime_r(&lastFeedTime, &lastFeedTm);
      if (dateKey(lastFeedTm) == today) {
        lastFeedClock = twoDigits(lastFeedTm.tm_hour) + ":" + twoDigits(lastFeedTm.tm_min);
        struct tm scheduledTm = nowTm;
        scheduledTm.tm_hour = feedMinute[slot] / 60;
        scheduledTm.tm_min = feedMinute[slot] % 60;
        scheduledTm.tm_sec = 0;
        time_t scheduledEpoch = mktime(&scheduledTm);
        coveredByLastFeeding = lastFeedTime >= scheduledEpoch;
      }
    }
    if (coveredByLastFeeding) {
      lastFeedDate[slot] = today;
      saveSettings();
      saveDeviceLog(String("Horario ") + (slot + 1) + " ja atendido pela alimentacao salva as " +
        lastFeedClock + "; sem repetir.");
      continue;
    }

    int lateBy = currentMinute - feedMinute[slot];
    if (lateBy <= LATE_GRACE_MINUTES) {
      if (feedOnePortion(slot == 0 ? "HORARIO_1" : "HORARIO_2")) {
        lastFeedDate[slot] = today;
        saveSettings();
      }
    } else {
      appendFeedHistory(false, "schedule_too_late", portionsDispensed < PORTIONS_PER_LOAD ? portionsDispensed + 1 : 0);
      String missedMessage = String("Horario ") + (slot == 0 ? "1" : "2") +
        " perdido: atraso maior que 30 minutos; alimentacao ignorada.";
      notifyBLE(String("HORARIO_PERDIDO: ") + missedMessage);
      saveDeviceLog(missedMessage);
      queuePush("HORARIO_PERDIDO", missedMessage);
      lastFeedDate[slot] = today;
      saveSettings();
    }
  }
}

void sendStatus() {
  struct tm t;
  uint8_t portionsRemaining = awaitingReload || portionsDispensed >= PORTIONS_PER_LOAD
    ? 0
    : PORTIONS_PER_LOAD - portionsDispensed;
  String s = "STATUS;wifi=" + String(WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado");
  s += ";ssid=" + wifiSSID;
  s += ";hora=";
  if (clockIsValid(&t)) {
    s += twoDigits(t.tm_hour) + ":" + twoDigits(t.tm_min) + ":" + twoDigits(t.tm_sec);
    s += ";data=" + String(t.tm_year + 1900) + "-" + twoDigits(t.tm_mon + 1) + "-" + twoDigits(t.tm_mday);
  } else {
    s += "nao_sincronizada";
  }
  s += ";feed1=" + formatHHMM(feedMinute[0]);
  s += ";feed2=" + formatHHMM(feedMinute[1]);
  s += ";posicao=" + String(rotorPosition) + "/15";
  s += ";porcoes_restantes=" + String(portionsRemaining);
  s += ";porcoes_lote=" + String(portionsDispensed) + "/14";
  s += ";lote=" + String(awaitingReload ? "aguardando_recarga" : "pronto");
  s += ";referencia=" + String(referenceValid ? "confirmada" : "verificar");
  s += ";motor_test=" + String(motorTestMode ? "ON," : "OFF,") + String(motorTestShots) + "," + String(motorTestNextEpoch);
  s += ";movimento_interrompido=" + String(motionInProgress ? "sim" : "nao");
  s += ";passos_volta=" + String(halfStepsPerRev);
  s += ";push=" + String(hasFcmDevices() ? "configurado" : "sem_destino");
  s += ";firebase=" + String(firebaseConfigured() ? "configurado" : "incompleto");
  s += ";fb_project=" + firebaseProjectId;
  s += ";fb_email=" + firebaseClientEmail;
  s += ";temperatura=";
  if (waterTemperatureC != DEVICE_DISCONNECTED_C && isfinite(waterTemperatureC)) {
    s += String(waterTemperatureC, 1);
  } else {
    s += "indisponivel";
  }
  notifyBLE(s);
}

void processTemperature() {
  const unsigned long now = millis();
  if (temperatureConversionPending) {
    if (now - temperatureConversionStarted < 750) return;
    float measured = temperatureSensor.getTempCByIndex(0);
    temperatureConversionPending = false;
    waterTemperatureC = measured;
    if (measured == DEVICE_DISCONNECTED_C || !isfinite(measured)) {
      // A transient sensor read failure is not evidence that the water
      // returned to a safe range; retain alert state and its repeat timer.
      return;
    }

    time_t epochNow = time(nullptr);
    const bool hasValidClock = clockIsValid() && epochNow >= 1735689600;
    const uint32_t nowEpoch = hasValidClock ? static_cast<uint32_t>(epochNow) : 0;

    if (measured < LOW_TEMPERATURE_LIMIT_C &&
        (!lowTemperatureAlertActive || (nowEpoch && lastLowTemperatureAlertEpoch &&
          static_cast<uint32_t>(nowEpoch - lastLowTemperatureAlertEpoch) >= TEMPERATURE_ALERT_INTERVAL_SECONDS))) {
      lowTemperatureAlertActive = true;
      lastLowTemperatureAlertEpoch = nowEpoch;
      prefs.putBool("tempLowOn", true);
      prefs.putUInt("tempLowAt", lastLowTemperatureAlertEpoch);
      String alertText = "Temperatura da agua baixa: " + String(measured, 1) + " °C. Limite: 24 °C.";
      notifyBLE(String("ALERTA_TEMPERATURA: ") + alertText);
      queuePush("TEMPERATURA_BAIXA", alertText);
    } else if (measured >= LOW_TEMPERATURE_LIMIT_C + 0.5f) {
      lowTemperatureAlertActive = false;
      lastLowTemperatureAlertEpoch = 0;
      prefs.putBool("tempLowOn", false);
      prefs.putUInt("tempLowAt", 0);
      cancelQueuedPush("TEMPERATURA_BAIXA");
    } else if (lowTemperatureAlertActive && !lastLowTemperatureAlertEpoch && nowEpoch) {
      // Start the persistent 30-minute interval once NTP becomes available.
      lastLowTemperatureAlertEpoch = nowEpoch;
      prefs.putUInt("tempLowAt", lastLowTemperatureAlertEpoch);
    }

    if (measured > HIGH_TEMPERATURE_LIMIT_C &&
        (!highTemperatureAlertActive || (nowEpoch && lastHighTemperatureAlertEpoch &&
          static_cast<uint32_t>(nowEpoch - lastHighTemperatureAlertEpoch) >= TEMPERATURE_ALERT_INTERVAL_SECONDS))) {
      highTemperatureAlertActive = true;
      lastHighTemperatureAlertEpoch = nowEpoch;
      prefs.putBool("tempHighOn", true);
      prefs.putUInt("tempHighAt", lastHighTemperatureAlertEpoch);
      String alertText = "Temperatura da agua alta: " + String(measured, 1) + " °C. Limite: 28 °C.";
      notifyBLE(String("ALERTA_TEMPERATURA: ") + alertText);
      queuePush("TEMPERATURA_ALTA", alertText);
    } else if (measured <= HIGH_TEMPERATURE_LIMIT_C - 0.5f) {
      highTemperatureAlertActive = false;
      lastHighTemperatureAlertEpoch = 0;
      prefs.putBool("tempHighOn", false);
      prefs.putUInt("tempHighAt", 0);
      cancelQueuedPush("TEMPERATURA_ALTA");
    } else if (highTemperatureAlertActive && !lastHighTemperatureAlertEpoch && nowEpoch) {
      lastHighTemperatureAlertEpoch = nowEpoch;
      prefs.putUInt("tempHighAt", lastHighTemperatureAlertEpoch);
    }
  }

  if (!temperatureConversionPending &&
      (lastTemperatureRequest == 0 || now - lastTemperatureRequest >= TEMPERATURE_SAMPLE_MS)) {
    lastTemperatureRequest = now;
    temperatureSensor.requestTemperatures();
    temperatureConversionPending = true;
    temperatureConversionStarted = now;
  }
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;

  int eq = cmd.indexOf('=');
  String key = (eq >= 0 ? cmd.substring(0, eq) : cmd);
  String value = (eq >= 0 ? cmd.substring(eq + 1) : "");
  key.trim();
  value.trim();
  key.toUpperCase();

  if (key == "TEST_MODE") {
    if (value.equalsIgnoreCase("ON")) {
      if (!motorTestMode && (!referenceValid || motionInProgress || feedInProgress || awaitingReload || portionsDispensed > PORTIONS_PER_LOAD - 9)) {
        notifyBLE("ERRO: defina SET_HOME e confirme uma recarga com pelo menos 9 porcoes disponiveis antes do teste.");
        return;
      }
      if (!motorTestMode) {
        time_t now = time(nullptr);
        if (now < 1735689600) {
          notifyBLE("ERRO: sincronize o relogio NTP antes de iniciar o teste.");
          return;
        }
        motorTestShots = 0;
        motorTestNextEpoch = (static_cast<uint32_t>(now) / 900UL + 1UL) * 900UL;
        motorTestMode = true;
        saveSettings();
        time_t nextTime = motorTestNextEpoch;
        struct tm nextTm;
        localtime_r(&nextTime, &nextTm);
        saveDeviceLog(String("Modo de teste iniciado; primeiro disparo em ") +
          twoDigits(nextTm.tm_hour) + ":" + twoDigits(nextTm.tm_min) +
          "; sequencia 3x15, 3x30 e 3x60 minutos.");
      }
      notifyBLE(String("OK: MOTOR_TEST=ON:") + motorTestShots + ":" + motorTestNextEpoch);
    } else if (value.equalsIgnoreCase("OFF")) {
      if (motorTestMode) {
        motorTestMode = false;
        motorTestNextEpoch = 0;
        saveSettings();
        saveDeviceLog(String("Modo de teste do motor cancelado apos ") + motorTestShots +
          "/9 disparos; porcoes contabilizadas pelo fluxo normal; reposicione o rotor e use SET_HOME antes de abastecer racao real.");
      }
      notifyBLE(String("OK: MOTOR_TEST=OFF:") + motorTestShots + ":0");
    } else {
      notifyBLE("ERRO: use TEST_MODE=ON ou TEST_MODE=OFF.");
    }
    return;
  }

  if (motorTestMode && key != "STATUS" && key != "LOGS" && key != "FEED_HISTORY") {
    notifyBLE("ERRO: desligue o modo de teste antes de outros comandos.");
    return;
  }

  if (key == "BLE_ON") {
    notifyBLE("OK: BLE permanece ativo");
  } else if (key == "SSID" && value.length() > 0) {
    wifiSSID = value;
    saveSettings();
    wifiConnecting = false;
    lastWiFiAttempt = 0;
    WiFi.disconnect(true);
    notifyBLE("OK: SSID salvo; conectando ao Wi-Fi.");
  } else if (key == "PASS") {
    wifiPASS = value;
    saveSettings();
    wifiConnecting = false;
    lastWiFiAttempt = 0;
    WiFi.disconnect(true);
    notifyBLE("OK: senha Wi-Fi salva.");
  } else if (key == "FEED1" || key == "FEED2") {
    uint16_t parsed;
    if (!parseHHMM(value, parsed)) {
      notifyBLE("ERRO: use HH:MM, por exemplo FEED1=08:00.");
      return;
    }
    feedMinute[key == "FEED1" ? 0 : 1] = parsed;
    saveSettings();
    notifyBLE("OK: " + key + "=" + formatHHMM(parsed));
  } else if (key == "TIMES") {
    int comma = value.indexOf(',');
    if (comma < 0 || value.indexOf(',', comma + 1) >= 0) {
      notifyBLE("ERRO: use TIMES=08:00,20:00.");
      return;
    }
    uint16_t first, second;
    if (!parseHHMM(value.substring(0, comma), first) ||
        !parseHHMM(value.substring(comma + 1), second) || first == second) {
      notifyBLE("ERRO: informe dois horarios validos e diferentes (HH:MM,HH:MM).");
      return;
    }
    feedMinute[0] = first;
    feedMinute[1] = second;
    saveSettings();
    notifyBLE("OK: horarios salvos=" + formatHHMM(first) + "," + formatHHMM(second));
  } else if (key == "STATUS") {
    sendStatus();
  } else if (key == "LOGS") {
    emitDeviceLogs();
  } else if (key == "FEED_HISTORY") {
    emitBatchFeedHistory();
  } else if (key == "TEMP") {
    sendStatus();
  } else if (key == "FEED") {
    feedOnePortion("MANUAL");
  } else if (key == "JOG_LEFT") {
    if (jogMotor(false)) notifyBLE("OK: JOG_LEFT");
  } else if (key == "JOG_RIGHT") {
    if (jogMotor(true)) notifyBLE("OK: JOG_RIGHT");
  } else if (key == "SET_HOME") {
    // Manual home only. Align the EMPTY pocket at outlet; DO NOT use
    // this command as a shortcut to bypass a physical jam.
    rotorPosition = 0;
    portionsDispensed = 0;
    clearBatchFeedHistory();
    awaitingReload = true; // require explicit confirmation after loading
    referenceValid = true;
    motionInProgress = false;
    saveSettings();
    notifyBLE("OK: posicao inicial definida. Abasteca as cavidades e confirme a recarga pelo botao do PWA.");
  } else if (key == "RELOAD") {
    if (!awaitingReload) {
      notifyBLE("ERRO: recarga nao solicitada pelo alimentador.");
      return;
    }
    if (!referenceValid || motionInProgress ||
        (rotorPosition != 0 && rotorPosition != PORTIONS_PER_LOAD)) {
      notifyBLE("ERRO: referencia invalida. Alinhe fisicamente a cavidade vazia e envie SET_HOME.");
      return;
    }
    // Refill confirmation only updates the load state. Motor movement is
    // reserved for scheduled feeding times.
    portionsDispensed = 0;
    clearBatchFeedHistory();
    awaitingReload = false;
    suppressOverdueAfterReload = true;
    // A refill is not a request to catch up on a meal whose time has already
    // passed. Mark elapsed slots as handled for today so the scheduler waits
    // for the next configured time instead of dispensing immediately.
    lastEmptyReminderEpoch = 0;
    prefs.putUInt("emptyReminder", 0);
    cancelQueuedPush("EMPTY");
    cancelQueuedPush("EMPTY_REMINDER");
    saveSettings();
    notifyBLE("OK: recarga confirmada pelo PWA; alimentador pronto.");
  } else if (key == "FB_BEGIN") {
    pendingFirebaseProjectId = firebaseProjectId;
    pendingFirebaseClientEmail = firebaseClientEmail;
    pendingFirebasePrivateKey = firebasePrivateKey;
    expectedFirebaseProjectLength = 0;
    expectedFirebaseEmailLength = 0;
    expectedFirebasePrivateKeyLength = 0;
    firebaseConfigEditing = true;
    notifyBLE("OK: FB_BEGIN");
  } else if (key == "PRJ_BEGIN") {
    const long fieldLength = value.toInt();
    if (!firebaseConfigEditing || fieldLength < 6 || fieldLength > 30) {
      notifyBLE("ERRO: tamanho do ID do projeto invalido.");
      return;
    }
    expectedFirebaseProjectLength = (size_t)fieldLength;
    pendingFirebaseProjectId = "";
    pendingFirebaseProjectId.reserve(expectedFirebaseProjectLength + 1);
    notifyBLE("OK: PRJ_BEGIN");
  } else if (key == "PRJ_ADD") {
    if (!firebaseConfigEditing || expectedFirebaseProjectLength == 0 || value.isEmpty() ||
        pendingFirebaseProjectId.length() + value.length() > expectedFirebaseProjectLength) {
      notifyBLE("ERRO: fragmento do ID do projeto invalido.");
      return;
    }
    pendingFirebaseProjectId += value;
    notifyBLE("OK: PRJ_ADD");
  } else if (key == "PRJ_SAVE") {
    if (!firebaseConfigEditing || expectedFirebaseProjectLength == 0 ||
        pendingFirebaseProjectId.length() != expectedFirebaseProjectLength) {
      notifyBLE("ERRO: ID do projeto incompleto.");
      return;
    }
    if (pendingFirebaseProjectId[0] < 'a' || pendingFirebaseProjectId[0] > 'z' ||
        !((pendingFirebaseProjectId[pendingFirebaseProjectId.length() - 1] >= 'a' && pendingFirebaseProjectId[pendingFirebaseProjectId.length() - 1] <= 'z') ||
          (pendingFirebaseProjectId[pendingFirebaseProjectId.length() - 1] >= '0' && pendingFirebaseProjectId[pendingFirebaseProjectId.length() - 1] <= '9'))) {
      notifyBLE("ERRO: formato do ID do projeto invalido.");
      return;
    }
    for (size_t i = 0; i < pendingFirebaseProjectId.length(); i++) {
      const char c = pendingFirebaseProjectId[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
        notifyBLE("ERRO: ID do projeto deve usar letras minusculas, numeros e hifens.");
        return;
      }
    }
    expectedFirebaseProjectLength = 0;
    notifyBLE("OK: PRJ_SAVE");
  } else if (key == "EML_BEGIN") {
    const long fieldLength = value.toInt();
    if (!firebaseConfigEditing || fieldLength < 20 || fieldLength > 160) {
      notifyBLE("ERRO: tamanho do e-mail invalido.");
      return;
    }
    expectedFirebaseEmailLength = (size_t)fieldLength;
    pendingFirebaseClientEmail = "";
    pendingFirebaseClientEmail.reserve(expectedFirebaseEmailLength + 1);
    notifyBLE("OK: EML_BEGIN");
  } else if (key == "EML_ADD") {
    if (!firebaseConfigEditing || expectedFirebaseEmailLength == 0 || value.isEmpty() ||
        pendingFirebaseClientEmail.length() + value.length() > expectedFirebaseEmailLength) {
      notifyBLE("ERRO: fragmento do e-mail invalido.");
      return;
    }
    pendingFirebaseClientEmail += value;
    notifyBLE("OK: EML_ADD");
  } else if (key == "EML_SAVE") {
    if (!firebaseConfigEditing || expectedFirebaseEmailLength == 0 ||
        pendingFirebaseClientEmail.length() != expectedFirebaseEmailLength ||
        pendingFirebaseClientEmail.indexOf('@') <= 0 ||
        !pendingFirebaseClientEmail.endsWith(".iam.gserviceaccount.com")) {
      notifyBLE("ERRO: e-mail da conta de servico invalido.");
      return;
    }
    expectedFirebaseEmailLength = 0;
    notifyBLE("OK: EML_SAVE");
  } else if (key == "PK_BEGIN") {
    const long keyLength = value.toInt();
    if (!firebaseConfigEditing || keyLength < 300 || keyLength > 1900) {
      notifyBLE("ERRO: tamanho da chave privada invalido.");
      return;
    }
    expectedFirebasePrivateKeyLength = (size_t)keyLength;
    pendingFirebasePrivateKey = "";
    pendingFirebasePrivateKey.reserve(expectedFirebasePrivateKeyLength + 1);
    notifyBLE("OK: PK_BEGIN");
  } else if (key == "PK_ADD") {
    if (!firebaseConfigEditing || expectedFirebasePrivateKeyLength == 0 || value.isEmpty() ||
        pendingFirebasePrivateKey.length() + value.length() > expectedFirebasePrivateKeyLength) {
      notifyBLE("ERRO: fragmento da chave privada invalido.");
      return;
    }
    pendingFirebasePrivateKey += value;
    notifyBLE("OK: PK_ADD");
  } else if (key == "PK_SAVE") {
    if (!firebaseConfigEditing || expectedFirebasePrivateKeyLength == 0 ||
        pendingFirebasePrivateKey.length() != expectedFirebasePrivateKeyLength ||
        pendingFirebasePrivateKey.indexOf("-----BEGIN PRIVATE KEY-----") < 0 ||
        pendingFirebasePrivateKey.indexOf("-----END PRIVATE KEY-----") < 0) {
      notifyBLE("ERRO: chave privada incompleta ou formato invalido.");
      return;
    }
    expectedFirebasePrivateKeyLength = 0;
    notifyBLE("OK: PK_SAVE");
  } else if (key == "FB_SAVE") {
    if (!firebaseConfigEditing || pendingFirebaseProjectId.isEmpty() ||
        pendingFirebaseClientEmail.isEmpty() ||
        pendingFirebasePrivateKey.indexOf("-----BEGIN PRIVATE KEY-----") < 0 ||
        pendingFirebasePrivateKey.indexOf("-----END PRIVATE KEY-----") < 0) {
      notifyBLE("ERRO: credenciais Firebase incompletas.");
      return;
    }
    size_t freeEntriesBefore = prefs.freeEntries();
    size_t projectWritten = prefs.putString("fbProject", pendingFirebaseProjectId);
    size_t emailWritten = prefs.putString("fbEmail", pendingFirebaseClientEmail);
    size_t keySize = pendingFirebasePrivateKey.length() + 1;
    size_t keyWritten = prefs.putBytes("fbKeyBin", pendingFirebasePrivateKey.c_str(), keySize);
    String savedProject = prefs.getString("fbProject", "");
    String savedEmail = prefs.getString("fbEmail", "");
    size_t savedKeySize = prefs.getBytesLength("fbKeyBin");
    char* savedKeyBuffer = savedKeySize == keySize ? (char*)malloc(savedKeySize) : nullptr;
    bool savedKeyMatches = false;
    if (savedKeyBuffer) {
      savedKeyMatches = prefs.getBytes("fbKeyBin", savedKeyBuffer, savedKeySize) == savedKeySize &&
        memcmp(savedKeyBuffer, pendingFirebasePrivateKey.c_str(), keySize) == 0;
      free(savedKeyBuffer);
    }
    if (!projectWritten || !emailWritten || !keyWritten ||
        savedProject != pendingFirebaseProjectId ||
        savedEmail != pendingFirebaseClientEmail ||
        !savedKeyMatches) {
      notifyBLE(String("ERRO: NVS Firebase; gravado=") + projectWritten + "," + emailWritten + "," + keyWritten +
                "; lido=" + savedProject.length() + "," + savedEmail.length() + "," + savedKeySize +
                "; esperado=" + pendingFirebaseProjectId.length() + "," + pendingFirebaseClientEmail.length() +
                "," + keySize + "; livres=" + freeEntriesBefore);
      return;
    }
    prefs.remove("fbKey");
    firebaseProjectId = pendingFirebaseProjectId;
    firebaseClientEmail = pendingFirebaseClientEmail;
    firebasePrivateKey = pendingFirebasePrivateKey;
    oauthAccessToken = "";
    oauthExpiresAtMs = 0;
    firebaseConfigEditing = false;
    pendingFirebaseProjectId = "";
    pendingFirebaseClientEmail = "";
    pendingFirebasePrivateKey = "";
    notifyBLE("OK: credenciais Firebase salvas.");
    if (pushDeliveryConfigured()) {
      queuePush("CONFIGURED_TEST", "Notificações configuradas!");
    } else {
      notifyPushLog("teste de confirmacao ignorado: destino FCM ainda nao configurado");
    }
  } else if (key == "CAL_STEPS") {
    int n = value.toInt();
    if (n < 3900 || n > 4200) { notifyBLE("ERRO: CAL_STEPS aceita 3900..4200."); return; }
    halfStepsPerRev = (uint16_t)n;
    referenceValid = false; // require physical alignment after recalibration
    saveSettings();
    notifyBLE("OK: calibracao salva. Confira posicao e envie SET_HOME.");
  } else if (key == "FCM_CLEAR") {
    const uint8_t removedCount = fcmTargetCount;
    if (!clearFcmDevices()) {
      notifyPushLog("falha ao limpar destinos FCM da NVS");
      notifyBLE("ERRO: falha ao limpar dispositivos FCM na NVS");
      return;
    }
    notifyPushLog(String("destinos FCM removidos: ") + removedCount + "; credenciais Firebase mantidas");
    notifyBLE("OK: dispositivos FCM removidos");
  } else if (key == "FCM_BEGIN" || key == "FCM_REMOVE_BEGIN") {
    int n = value.toInt();
    if (n < 50 || n > 512) { notifyBLE("ERRO: comprimento FCM invalido"); return; }
    pendingFcmToken = ""; pendingFcmToken.reserve(n + 1);
    expectedFcmLength = n;
    pendingFcmRemoval = key == "FCM_REMOVE_BEGIN";
    notifyPushLog(String(pendingFcmRemoval ? "recebendo remocao de dispositivo; tamanho=" : "recebendo registro de dispositivo; tamanho=") + n);
    notifyBLE(pendingFcmRemoval ? "OK: FCM_REMOVE_BEGIN" : "OK: FCM_BEGIN");
  } else if (key == "FCM_ADD" || key == "FCM_REMOVE_ADD") {
    if ((key == "FCM_REMOVE_ADD") != pendingFcmRemoval) { notifyBLE("ERRO: operacao FCM invalida"); return; }
    if (expectedFcmLength == 0 || value.isEmpty() ||
        pendingFcmToken.length() + value.length() > expectedFcmLength) {
      notifyPushLog(String("fragmento invalido: recebido=") + pendingFcmToken.length() +
                    ", fragmento=" + value.length() + ", esperado=" + expectedFcmLength);
      notifyBLE("ERRO: fragmento FCM invalido"); return;
    }
    pendingFcmToken += value;
    // Confirma cada fragmento para o PWA aguardar o processamento no loop antes
    // de enviar o proximo; a resposta do GATT so confirma a escrita BLE.
    notifyBLE("OK: FCM_ADD");
  } else if (key == "FCM_SAVE" || key == "FCM_REMOVE") {
    if ((key == "FCM_REMOVE") != pendingFcmRemoval) { notifyBLE("ERRO: operacao FCM invalida"); return; }
    if (expectedFcmLength == 0 || pendingFcmToken.length() != expectedFcmLength) {
      notifyPushLog(String("FCM incompleto: recebido=") + pendingFcmToken.length() +
                    ", esperado=" + expectedFcmLength);
      notifyBLE("ERRO: FCM incompleto"); return;
    }
    if (pendingFcmRemoval) {
      bool wasPresent = false;
      for (uint8_t i = 0; i < fcmTargetCount; i++) if (fcmTargetTokens[i] == pendingFcmToken) wasPresent = true;
      if (!removeFcmToken(pendingFcmToken)) {
        notifyBLE("ERRO: falha ao remover dispositivo FCM");
        pendingFcmToken = ""; expectedFcmLength = 0; pendingFcmRemoval = false;
        return;
      }
      notifyPushLog(String(wasPresent ? "dispositivo FCM removido; restantes=" : "dispositivo FCM ja ausente; restantes=") + fcmTargetCount);
      clearFirebaseCredentialsIfNoDevices();
      pendingFcmToken = ""; expectedFcmLength = 0; pendingFcmRemoval = false;
      notifyBLE("OK: dispositivo FCM removido");
      return;
    }
    if (!upsertFcmToken(pendingFcmToken)) {
      if (fcmTargetCount >= FCM_DEVICE_CAPACITY) {
        notifyPushLog(String("registro FCM recusado: limite atingido; dispositivos=") +
                      fcmTargetCount + "/" + FCM_DEVICE_CAPACITY);
        notifyBLE(String("ERRO: limite de dispositivos FCM atingido (") +
                  FCM_DEVICE_CAPACITY + ")");
      } else {
        notifyPushLog(String("registro FCM falhou ao gravar na NVS; dispositivos=") +
                      fcmTargetCount + "/" + FCM_DEVICE_CAPACITY);
        notifyBLE("ERRO: falha ao gravar dispositivo FCM na NVS");
      }
      pendingFcmToken = ""; expectedFcmLength = 0;
      return;
    }
    pendingFcmToken = ""; expectedFcmLength = 0; pendingFcmRemoval = false;
    notifyPushLog(String("dispositivo FCM salvo; total=") + fcmTargetCount);
    notifyBLE("OK: destino FCM salvo");
  } else if (key == "FCM_TEST") {
    notifyPushLog("comando FCM_TEST recebido");
    if (!pushDeliveryConfigured()) {
      notifyBLE("ERRO: configure o Firebase e ative notificacoes no PWA antes do teste.");
      return;
    }
    queuePush("TEST", "Teste de notificacao do BettaFeeder.");
    notifyBLE("OK: teste de push enfileirado");
  } else if (key == "CLEAR_WIFI") {
    wifiSSID = "";
    wifiPASS = "";
    prefs.remove("ssid");
    prefs.remove("pass");
    WiFi.disconnect(true);
    wifiConnecting = false;
    ntpConfigured = false;
    notifyBLE("OK: dados Wi-Fi apagados.");
  } else {
    notifyBLE("ERRO: comando desconhecido. Use STATUS, FEED, JOG_LEFT, JOG_RIGHT, TIMES, TEST_MODE=ON/OFF, SET_HOME, RELOAD, FCM_*, FB_BEGIN, PRJ_*, EML_*, PK_*, FB_SAVE, BLE_ON.");
  }
}

void processSerialCommands() {
  static String serialBuffer;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!serialBuffer.isEmpty()) {
        String command = serialBuffer;
        serialBuffer = "";
        handleCommand(command);
      }
      continue;
    }
    if (serialBuffer.length() < sizeof(pendingCommand) - 1) serialBuffer += c;
  }
}

bool isFirebaseConfigCommand(const char* command) {
  return strncmp(command, "FB_", 3) == 0 || strncmp(command, "FCM_", 4) == 0 ||
    strncmp(command, "PRJ_", 4) == 0 || strncmp(command, "EML_", 4) == 0 ||
    strncmp(command, "PK_", 3) == 0;
}

void pushWorkerTask(void*) {
  for (;;) {
    pushNetworkBusy = true;
    processPush();
    processFcmMaintenance();
    pushNetworkBusy = false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- BLE callbacks ----------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
#if NIMBLE_CPP_VERSION >= NIMBLE_CPP_VERSION_VAL(3, 0, 0)
  uint8_t onWrite(NimBLECharacteristic* characteristic,
                  NimBLEAttValue& value,
                  NimBLEConnInfo& connInfo) override {
    if (value.length() == 0) return 0;
    String command(value.c_str());
#else
  void onWrite(NimBLECharacteristic* characteristic,
               NimBLEConnInfo& connInfo) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) return;
    String command(value.c_str());
#endif

    portENTER_CRITICAL(&commandMux);
    size_t n = command.length();
    if (n >= sizeof(pendingCommand)) n = sizeof(pendingCommand) - 1;
    memcpy(pendingCommand, command.c_str(), n);
    pendingCommand[n] = '\0';
    commandPending = true;
    portEXIT_CRITICAL(&commandMux);
#if NIMBLE_CPP_VERSION >= NIMBLE_CPP_VERSION_VAL(3, 0, 0)
    return 0;
#endif
  }
};

static ServerCallbacks serverCallbacks;
static RxCallbacks rxCallbacks;

void startBLE() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  // O protocolo usa fragmentos de 18 bytes e funciona com MTU 23. Nao
  // solicite MTU 247, pois isso aumenta o consumo de memoria do BLE.
  NimBLEDevice::setMTU(23);
  NimBLEServer* server = NimBLEDevice::createServer();
#if NIMBLE_CPP_VERSION >= NIMBLE_CPP_VERSION_VAL(3, 0, 0)
  server->setCallbacks(serverCallbacks);
#else
  server->setCallbacks(&serverCallbacks, false);
#endif

  NimBLEService* service = server->createService(SERVICE_UUID);

  txCharacteristic = service->createCharacteristic(
    TX_UUID, NIMBLE_PROPERTY::NOTIFY
  );

  // Plain BLE writes are intentional for this private device. Configuration
  // values, including Wi-Fi and Firebase credentials, are not encrypted over the radio.
  NimBLECharacteristic* rx = service->createCharacteristic(
    RX_UUID, NIMBLE_PROPERTY::WRITE
  );
#if NIMBLE_CPP_VERSION >= NIMBLE_CPP_VERSION_VAL(3, 0, 0)
  rx->setCallbacks(rxCallbacks);
#else
  rx->setCallbacks(&rxCallbacks, false);
#endif

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();
}

void setup() {
  Serial.begin(115200);
  pushQueueMutex = xSemaphoreCreateMutex();

  // O projeto usa somente BLE. Liberar a memoria reservada ao Bluetooth
  // Classic aumenta a heap disponivel para o handshake TLS do FCM/OAuth.
  esp_err_t btMemory = esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.printf("BT Classic: memoria %s; heap=%lu\n",
                btMemory == ESP_OK ? "liberada" : "ja liberada/indisponivel",
                (unsigned long)ESP.getFreeHeap());

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(MOTOR_PINS[i], OUTPUT);
    digitalWrite(MOTOR_PINS[i], LOW);
  }

  temperatureSensor.begin();
  temperatureSensor.setResolution(10); // 187.5 ms conversion, suitable for periodic monitoring
  temperatureSensor.setWaitForConversion(false);

  loadSettings();
  deviceLogNext = prefs.getUChar("logNext", 0) % DEVICE_LOG_CAPACITY;
  deviceLogCount = prefs.getUChar("logCount", 0);
  if (deviceLogCount > DEVICE_LOG_CAPACITY) deviceLogCount = DEVICE_LOG_CAPACITY;
  clearFirebaseCredentialsIfNoDevices();
  saveDeviceLog("Reinicializacao do ESP32.");
  if (motorTestMode) {
    motorTestMode = false;
    motorTestNextEpoch = 0;
    saveSettings();
    saveDeviceLog("Teste do motor interrompido por reinicializacao; porcoes ja executadas permanecem contabilizadas.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  startBLE();

  notifyBLE(referenceValid
    ? "BettaFeeder V6: posicao salva restaurada sem mover o motor."
    : "BettaFeeder V6: alinhe a cavidade vazia e use SET_HOME; o motor permanece parado.");
  if (motionInProgress) {
    referenceValid = false;
    appendFeedHistory(false, "movement_interrupted", portionsDispensed < PORTIONS_PER_LOAD ? portionsDispensed + 1 : 0);
    motionInProgress = false;
    saveSettings();
    const String motionAlert = "Energia interrompida durante movimento do motor. Verifique o rotor e defina novamente a posicao inicial.";
    notifyBLE(String("ALERTA: ") + motionAlert);
    queuePush("MOVIMENTO_INTERROMPIDO", motionAlert);
    saveDeviceLog(motionAlert);
  }
  if (!wifiSSID.isEmpty()) {
    lastWiFiAttempt = 0;
    connectWiFiIfNeeded();
  }
  if (xTaskCreatePinnedToCore(pushWorkerTask, "pushWorker", 16 * 1024,
                              nullptr, 0, &pushWorkerHandle, 1) != pdPASS) {
    pushWorkerHandle = nullptr;
    Serial.println("Push worker indisponivel; processamento fica no loop principal.");
  }
}

void loop() {
  // Run due scheduled feeding before serial/BLE command handling and all
  // maintenance tasks. The motor action completes before its result is logged.
  processSchedule();

  processSerialCommands();

  char localCommand[192];
  bool hasCommand = false;

  portENTER_CRITICAL(&commandMux);
  if (commandPending) {
    strncpy(localCommand, pendingCommand, sizeof(localCommand));
    localCommand[sizeof(localCommand) - 1] = '\0';
    commandPending = false;
    hasCommand = true;
  }
  portEXIT_CRITICAL(&commandMux);

  if (hasCommand && pushNetworkBusy && isFirebaseConfigCommand(localCommand)) {
    portENTER_CRITICAL(&commandMux);
    if (!commandPending) {
      strncpy(pendingCommand, localCommand, sizeof(pendingCommand));
      pendingCommand[sizeof(pendingCommand) - 1] = '\0';
      commandPending = true;
      hasCommand = false;
    }
    portEXIT_CRITICAL(&commandMux);
  }

  if (hasCommand) handleCommand(String(localCommand));

  connectWiFiIfNeeded();
  processTemperature();
  processEmptyLoadReminder();
  if (!pushWorkerHandle) {
    pushNetworkBusy = true;
    processPush();
    processFcmMaintenance();
    pushNetworkBusy = false;
  }

  delay(10);
}
