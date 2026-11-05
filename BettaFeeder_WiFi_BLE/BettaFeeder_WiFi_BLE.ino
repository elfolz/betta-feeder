/*
  BettaFeeder - ESP32-WROOM-DA + 28BYJ-48 + ULN2003
  Wi-Fi/NTP clock; BLE configuration; 15-position carousel:
  - position 0 is the EMPTY safety pocket parked at the discharge opening
  - positions 1..14 hold the 14 portions
  - after portion 14, the rotor advances once more to park the empty pocket

  Arduino IDE:
  - Board package: "esp32 by Espressif Systems"
  - BLE uses NimBLE-Arduino, a lower-memory alternative to Bluedroid.
  - No DS3231 / RTC module required.

  BLE commands (write one command per BLE write):
    SSID=MyWiFi
    PASS=MyPassword
    FEED1=08:00
    FEED2=20:00
    STATUS
    FEED
    SET_HOME       // physically align empty pocket; requires RELOAD after loading
    RELOAD         // confirm ALL 14 pockets manually loaded; rotor at 0
    CAL_STEPS=4076 // gearbox calibration; requires SET_HOME afterwards
    CLEAR_WIFI
    BLE_ON         // reativa BLE e reinicia o ESP32

  Motor pins (ULN2003 IN1..IN4):
    IN1=GPIO16, IN2=GPIO17, IN3=GPIO18, IN4=GPIO19

  IMPORTANT:
  - The motor has no position sensor. SET_HOME is a manual reference operation.
  - On every boot referenceValid starts false: verify the empty pocket and send SET_HOME.
  - If power is lost during a movement, real rotor position may no longer match
    the saved index. Re-align manually and send SET_HOME.
*/

#include <WiFi.h>
#include <esp_bt.h>
#include <time.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <math.h>
#include <memory>
#include <cstring>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/version.h>
#include "firebase_secrets.h"

// ---------- Hardware ----------
static const uint8_t MOTOR_PINS[4] = {16, 17, 18, 19}; // ULN2003 IN1..IN4
// 28BYJ-48 varies by gearbox. Start at 4076 halfsteps/revolution;
// CAL_STEPS=NNNN via BLE permits adjusting after physical testing.
uint16_t halfStepsPerRev = 4076;
static const bool MOTOR_FORWARD = true; // invert if mechanical rotation is opposite
static const uint8_t POCKETS = 15;
static const uint8_t PORTIONS_PER_LOAD = 14;
static const uint16_t STEP_DELAY_MS = 2;

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
bool serialMode = false;

portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;
char pendingCommand[192] = {0};
volatile bool commandPending = false;

// ---------- Persistent settings ----------
Preferences prefs;
String wifiSSID;
String wifiPASS;
uint16_t feedMinute[2] = {8 * 60, 20 * 60};
int lastFeedDate[2] = {0, 0};

uint8_t rotorPosition = 0;     // 0 = empty safety pocket at outlet
uint8_t portionsDispensed = 0; // 0..14
bool awaitingReload = true; // fail-safe until deliberate physical loading
bool referenceValid = false; // after any reboot, manual verify required
bool motionInProgress = false; // persisted before motor starts
bool feedInProgress = false;

// ---------- Wi-Fi / clock ----------
bool ntpConfigured = false;
unsigned long lastWiFiAttempt = 0;
unsigned long lastStatusCheck = 0;
bool wifiConnecting = false;
static const unsigned long WIFI_RETRY_MS = 30000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
unsigned long wifiAttemptStarted = 0;
// FCM: token da instalacao web vinculado via BLE
String fcmTargetToken;
String pendingFcmToken;
size_t expectedFcmLength = 0;
String oauthAccessToken;
unsigned long oauthExpiresAtMs = 0;
String pendingPushEvent;
String pendingPushText;
unsigned long pushNextAttempt = 0;
unsigned long lastPushWifiLog = 0;
static const unsigned long PUSH_RETRY_MS = 60000;
static const unsigned long PUSH_WIFI_WAIT_MS = 5000;

static const int LATE_GRACE_MINUTES = 30; // skip a missed meal after 30 min

// ---------- Helpers ----------
String twoDigits(int n) {
  return (n < 10 ? "0" : "") + String(n);
}

bool parseHHMM(const String& value, uint16_t& result) {
  int colon = value.indexOf(':');
  if (colon < 1) return false;
  int h = value.substring(0, colon).toInt();
  int m = value.substring(colon + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
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
  notifyBLE(String("PUSH: ") + message);
}

void saveSettings() {
  // Token de notificacao fica no NVS, nunca em logs BLE.
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPASS);
  prefs.putUShort("feed1", feedMinute[0]);
  prefs.putUShort("feed2", feedMinute[1]);
  prefs.putInt("last1", lastFeedDate[0]);
  prefs.putInt("last2", lastFeedDate[1]);
  prefs.putUChar("position", rotorPosition);
  prefs.putUChar("portions", portionsDispensed);
  prefs.putBool("reload", awaitingReload);
  prefs.putBool("moving", motionInProgress);
  prefs.putUShort("steps", halfStepsPerRev);
  prefs.putBool("serialMode", serialMode);
  // O token FCM e salvo apenas em FCM_SAVE/FCM_CLEAR.
}

bool replaceFcmToken(const String& newToken) {
  // putString substitui a entrada existente no NVS. A leitura de volta
  // confirma que a nova chave foi realmente persistida antes de usa-la.
  const size_t written = prefs.putString("fcmToken", newToken);
  if (written != newToken.length()) {
    Serial.printf("FCM: falha ao salvar token (%u/%u bytes)\n",
                  (unsigned)written, (unsigned)newToken.length());
    return false;
  }

  const String savedToken = prefs.getString("fcmToken", "");
  if (savedToken != newToken) {
    Serial.println("FCM: validacao do token persistido falhou");
    return false;
  }

  fcmTargetToken = savedToken;
  return true;
}

void loadSettings() {
  prefs.begin("bettafeed", false);
  wifiSSID = prefs.getString("ssid", "");
  fcmTargetToken = prefs.getString("fcmToken", "");
  serialMode = prefs.getBool("serialMode", false);
  wifiPASS = prefs.getString("pass", "");
  feedMinute[0] = prefs.getUShort("feed1", 8 * 60);
  feedMinute[1] = prefs.getUShort("feed2", 20 * 60);
  lastFeedDate[0] = prefs.getInt("last1", 0);
  lastFeedDate[1] = prefs.getInt("last2", 0);
  rotorPosition = prefs.getUChar("position", 0);
  portionsDispensed = prefs.getUChar("portions", 0);
  awaitingReload = prefs.getBool("reload", true);
  motionInProgress = prefs.getBool("moving", false);
  halfStepsPerRev = prefs.getUShort("steps", 4076);
  if (halfStepsPerRev < 3900 || halfStepsPerRev > 4200) halfStepsPerRev = 4076;
  // Without a homing sensor, position cannot be verified across a reboot.
  // A previous interruption during motion is especially dangerous.
  referenceValid = false;
  if (motionInProgress) awaitingReload = true;
  if (rotorPosition >= POCKETS) rotorPosition = 0;
  if (portionsDispensed > PORTIONS_PER_LOAD) { portionsDispensed = PORTIONS_PER_LOAD; awaitingReload = true; }
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

void rotateToNextPocket() {
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
  delay(250); // allow pellets to fall before another movement
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
  return String(FIREBASE_PROJECT_ID) != "COLOQUE_PROJECT_ID" &&
         String(FIREBASE_CLIENT_EMAIL) != "COLOQUE_CLIENT_EMAIL" &&
         strstr(FIREBASE_PRIVATE_KEY, "COLE_AQUI") == nullptr;
}
String signedJwt() {
  if (!firebaseConfigured() || !clockIsValid()) return "";
  time_t now = time(nullptr);
  String header = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
  String claims = "{\"iss\":\"" + jsonEscape(FIREBASE_CLIENT_EMAIL) +
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
  String privateKey = FIREBASE_PRIVATE_KEY;
  privateKey.replace("\\n", "\n");
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_context rng;
  mbedtls_ctr_drbg_init(&rng);
  const char* personal = "betta-fcm";
  int rc = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
                        (const unsigned char*)personal, strlen(personal));
  if (rc == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
    rc = mbedtls_pk_parse_key(&pk, (const unsigned char*)privateKey.c_str(),
           privateKey.length() + 1, nullptr, 0,
           mbedtls_ctr_drbg_random, &rng);
#else
    rc = mbedtls_pk_parse_key(&pk, (const unsigned char*)privateKey.c_str(),
           privateKey.length() + 1, nullptr, 0);
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
bool deliverPush(const String& title, const String& body) {
  if (fcmTargetToken.isEmpty()) {
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
  notifyPushLog("iniciando envio FCM");
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
  String url = "https://fcm.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID) + "/messages:send";
  if (!http.begin(secure, url)) {
    notifyPushLog("FCM: falha ao abrir conexao TLS");
    return false;
  }
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  // Mensagem data-only: o onBackgroundMessage do Firebase no Service Worker
  // exibe a notificacao nativa mesmo quando a aba estiver fechada.
  String payload = "{\"message\":{\"token\":\"" + jsonEscape(fcmTargetToken) +
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
  Serial.printf("FCM: envio HTTP %d\n", code);
  notifyPushLog(String("FCM HTTP ") + code);
  if (code != 200) {
    String detail = response;
    if (detail.length() > 180) detail = detail.substring(0, 180);
    if (!detail.isEmpty()) notifyPushLog(String("FCM erro: ") + detail);
  }
  return code == 200;
}
// Um evento pendente, reenviado periodicamente ate conectar.
// Sem RTC dedicado ou fila persistente: reiniciar pode perder este aviso.
void connectWiFiIfNeeded();

void queuePush(const String& event, const String& text) {
  pendingPushEvent = event;
  pendingPushText = text;
  pushNextAttempt = 0;
  notifyPushLog(String("evento enfileirado: ") + event);
}
void processPush() {
  if (pendingPushEvent.isEmpty() || (int32_t)(millis() - pushNextAttempt) < 0) return;

  // Primeiro inicia/continua a conexao Wi-Fi. O loop antigo tentava o FCM
  // antes desta funcao e acabava adiando o push por 60 segundos.
  connectWiFiIfNeeded();
  if (WiFi.status() != WL_CONNECTED || !clockIsValid()) {
    unsigned long now = millis();
    pushNextAttempt = now + PUSH_WIFI_WAIT_MS;
    if (now - lastPushWifiLog >= 15000 || lastPushWifiLog == 0) {
      lastPushWifiLog = now;
      notifyPushLog(WiFi.status() != WL_CONNECTED
        ? "envio aguardando: Wi-Fi desconectado"
        : "envio aguardando: NTP ainda nao sincronizado");
    }
    return;
  }

  if (deliverPush("BettaFeeder", pendingPushText)) {
    pendingPushEvent = ""; pendingPushText = "";
    notifyPushLog("push entregue com sucesso");
  } else {
    pushNextAttempt = millis() + PUSH_RETRY_MS;
    notifyPushLog(String("push falhou; nova tentativa em ") + (PUSH_RETRY_MS / 1000) + "s");
  }
}

void feedOnePortion(const char* source) {
  if (feedInProgress || motionInProgress) { notifyBLE("BLOQUEADO: motor ocupado."); return; }
  if (!referenceValid) { notifyBLE("BLOQUEADO: referencia desconhecida. Confira a posicao fisica e envie SET_HOME."); return; }
  if (awaitingReload || portionsDispensed >= PORTIONS_PER_LOAD) {
    notifyBLE("LOTE_VAZIO: recarregue 14 doses e envie RELOAD (rotor na posicao 0)."); return;
  }
  if (rotorPosition != portionsDispensed) {
    referenceValid = false;
    notifyBLE("FALHA: indice inconsistente. Confira fisicamente e envie SET_HOME."); return;
  }
  feedInProgress = true;
  rotateToNextPocket();
  portionsDispensed++;
  saveSettings();
  notifyBLE(String("ALIMENTACAO: ") + source + "; porcoes_no_lote=" + String(portionsDispensed) + "/14");
  queuePush("FEED_COMMANDED", "Motor acionado; dose " + String(portionsDispensed) +
            "/14. Queda da racao nao confirmada por sensor.");
  if (portionsDispensed == PORTIONS_PER_LOAD) {
    // Move to empty safety cavity after dose 14 and LATCH empty status.
    // Do NOT silently reset portions and allow more feedings!
    rotateToNextPocket();
    awaitingReload = true;
    saveSettings();
    notifyBLE("LOTE_FINALIZADO: rotor na posicao vazia. Recarregue e envie RELOAD.");
    queuePush("EMPTY", "Reservatorio vazio: recarregue 14 porcoes e confirme no aplicativo.");
  }
  feedInProgress = false;
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
  struct tm nowTm;
  if (!clockIsValid(&nowTm)) return;

  int today = dateKey(nowTm);
  int currentMinute = nowTm.tm_hour * 60 + nowTm.tm_min;

  if (!referenceValid || awaitingReload || motionInProgress) return;
  for (int slot = 0; slot < 2; slot++) {
    if (lastFeedDate[slot] == today) continue;
    if (currentMinute < feedMinute[slot]) continue;

    int lateBy = currentMinute - feedMinute[slot];
    lastFeedDate[slot] = today; // mark before motion to prevent repeat after reboot
    saveSettings();

    if (lateBy <= LATE_GRACE_MINUTES) {
      feedOnePortion(slot == 0 ? "HORARIO_1" : "HORARIO_2");
    } else {
      notifyBLE(String("HORARIO_PERDIDO: ") + (slot == 0 ? "1" : "2") +
                "; atraso maior que 30 minutos, alimentacao ignorada.");
    }
  }
}

void sendStatus() {
  struct tm t;
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
  s += ";porcoes_lote=" + String(portionsDispensed) + "/14";
  s += ";lote=" + String(awaitingReload ? "aguardando_recarga" : "pronto");
  s += ";referencia=" + String(referenceValid ? "confirmada" : "verificar");
  s += ";movimento_interrompido=" + String(motionInProgress ? "sim" : "nao");
  s += ";passos_volta=" + String(halfStepsPerRev);
  s += ";push=" + String(fcmTargetToken.isEmpty() ? "sem_destino" : "configurado");
  notifyBLE(s);
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

  if (key == "BLE_ON") {
    prefs.putBool("serialMode", false);
    notifyBLE("OK: BLE sera reativado apos reinicio");
    delay(250);
    ESP.restart();
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
  } else if (key == "STATUS") {
    sendStatus();
  } else if (key == "FEED") {
    feedOnePortion("MANUAL");
  } else if (key == "SET_HOME") {
    // Manual home only. Align the EMPTY pocket at outlet; DO NOT use
    // this command as a shortcut to bypass a physical jam.
    rotorPosition = 0;
    portionsDispensed = 0;
    referenceValid = true;
    awaitingReload = true; // requires explicit manual reload confirmation
    motionInProgress = false;
    saveSettings();
    notifyBLE("OK: home definido. Depois de abastecer 14 cavidades, envie RELOAD.");
  } else if (key == "RELOAD") {
    if (!referenceValid || rotorPosition != 0 || motionInProgress) {
      notifyBLE("ERRO: alinhe fisicamente cavidade vazia e use SET_HOME antes de RELOAD.");
      return;
    }
    portionsDispensed = 0;
    awaitingReload = false;
    saveSettings();
    notifyBLE("OK: lote de 14 porcoes confirmado manualmente.");
  } else if (key == "CAL_STEPS") {
    int n = value.toInt();
    if (n < 3900 || n > 4200) { notifyBLE("ERRO: CAL_STEPS aceita 3900..4200."); return; }
    halfStepsPerRev = (uint16_t)n;
    referenceValid = false; // require physical alignment after recalibration
    saveSettings();
    notifyBLE("OK: calibracao salva. Confira posicao e envie SET_HOME.");
  } else if (key == "FCM_BEGIN") {
    int n = value.toInt();
    if (n < 50 || n > 512) { notifyBLE("ERRO: comprimento FCM invalido"); return; }
    pendingFcmToken = ""; pendingFcmToken.reserve(n + 1);
    expectedFcmLength = n;
    notifyPushLog(String("recebendo novo token; tamanho=") + n);
    notifyBLE("OK: FCM_BEGIN");
  } else if (key == "FCM_ADD") {
    if (expectedFcmLength == 0 || value.isEmpty() ||
        pendingFcmToken.length() + value.length() > expectedFcmLength) {
      notifyPushLog(String("fragmento invalido: recebido=") + pendingFcmToken.length() +
                    ", fragmento=" + value.length() + ", esperado=" + expectedFcmLength);
      notifyBLE("ERRO: fragmento FCM invalido"); return;
    }
    pendingFcmToken += value;
    // Nao envie log por fragmento: isso congestiona o mesmo canal BLE usado
    // pelo PWA para transmitir o proximo FCM_ADD.
  } else if (key == "FCM_SAVE") {
    if (expectedFcmLength == 0 || pendingFcmToken.length() != expectedFcmLength) {
      notifyPushLog(String("FCM incompleto: recebido=") + pendingFcmToken.length() +
                    ", esperado=" + expectedFcmLength);
      notifyBLE("ERRO: FCM incompleto"); return;
    }
    if (!replaceFcmToken(pendingFcmToken)) {
      notifyBLE("ERRO: nao foi possivel salvar o destino FCM");
      pendingFcmToken = ""; expectedFcmLength = 0;
      return;
    }
    pendingFcmToken = ""; expectedFcmLength = 0;
    notifyPushLog("novo token FCM substituido e salvo no NVS");
    notifyBLE("OK: destino FCM salvo");
  } else if (key == "FCM_CLEAR") {
    fcmTargetToken = ""; prefs.remove("fcmToken");
    pendingFcmToken = ""; expectedFcmLength = 0;
    notifyBLE("OK: destino FCM removido");
  } else if (key == "FCM_TEST") {
    notifyPushLog("comando FCM_TEST recebido");
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
    notifyBLE("ERRO: comando desconhecido. Use STATUS, FEED, SET_HOME, RELOAD, CAL_STEPS, FCM_BEGIN, FCM_ADD, FCM_SAVE, FCM_CLEAR, FCM_TEST, BLE_ON.");
  }
}

void activateSerialMode() {
  if (serialMode) return;
  serialMode = true;
  prefs.putBool("serialMode", true);
  bleConnected = false;
  txCharacteristic = nullptr;
  // A porta serial recebeu um comando: encerra o stack BLE e libera a RAM
  // usada pelo NimBLE antes de iniciar OAuth/TLS.
  NimBLEDevice::deinit(true);
  Serial.printf("OK: SERIAL ativo; BLE desativado; heap=%u\n", ESP.getFreeHeap());
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
        activateSerialMode();
        handleCommand(command);
      }
      continue;
    }
    if (serialBuffer.length() < sizeof(pendingCommand) - 1) serialBuffer += c;
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
  uint8_t onWrite(NimBLECharacteristic* characteristic,
                  NimBLEAttValue& value,
                  NimBLEConnInfo& connInfo) override {
    if (value.length() == 0) return 0;
    String command(value.c_str());

    portENTER_CRITICAL(&commandMux);
    size_t n = command.length();
    if (n >= sizeof(pendingCommand)) n = sizeof(pendingCommand) - 1;
    memcpy(pendingCommand, command.c_str(), n);
    pendingCommand[n] = '\0';
    commandPending = true;
    portEXIT_CRITICAL(&commandMux);
    return 0;
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
  server->setCallbacks(serverCallbacks);

  NimBLEService* service = server->createService(SERVICE_UUID);

  txCharacteristic = service->createCharacteristic(
    TX_UUID, NIMBLE_PROPERTY::NOTIFY
  );

  NimBLECharacteristic* rx = service->createCharacteristic(
    RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  rx->setCallbacks(rxCallbacks);

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();
}

void setup() {
  Serial.begin(115200);

  // O projeto usa somente BLE. Liberar a memoria reservada ao Bluetooth
  // Classic aumenta a heap disponivel para o handshake TLS do FCM/OAuth.
  esp_err_t btMemory = esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.printf("BT Classic: memoria %s; heap=%u\n",
                btMemory == ESP_OK ? "liberada" : "ja liberada/indisponivel",
                ESP.getFreeHeap());

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(MOTOR_PINS[i], OUTPUT);
    digitalWrite(MOTOR_PINS[i], LOW);
  }

  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (!serialMode) {
    startBLE();
  } else {
    Serial.printf("SERIAL persistente: BLE nao inicializado; heap=%u\n", ESP.getFreeHeap());
  }

  notifyBLE("BettaFeeder V5: confira a referencia fisica, use SET_HOME; apos recarga, RELOAD.");
  if (motionInProgress) notifyBLE("ALERTA: energia interrompida durante movimento anterior. Verifique rotor.");
  if (!wifiSSID.isEmpty()) {
    lastWiFiAttempt = 0;
    connectWiFiIfNeeded();
  }
}

void loop() {
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

  if (hasCommand) handleCommand(String(localCommand));

  connectWiFiIfNeeded();
  processPush();

  if (millis() - lastStatusCheck >= 1000) {
    lastStatusCheck = millis();
    processSchedule();
  }

  delay(10);
}
