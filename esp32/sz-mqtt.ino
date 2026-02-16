#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <ArduinoJson.h>

// ============================================================
//  ESP32 → vLinker (BLE) → Requêtes type SZ Viewer → MQTT JSON
//  Inspiré de `esp32/vlinker-gw.ino`
//
//  Remarques importantes:
//  - Les 20 valeurs visibles dans SZ Viewer semblent issues de requêtes
//    propriétaires (ex: 21A0, 21A2, 21A5, 21CD, 2180, 17FF00…).
//  - Sans table de décodage complète (offsets/échelles), ce sketch publie:
//      - les 20 champs attendus (actuellement null si non décodés)
//      - les réponses brutes (hex) des pages 21xx pour permettre le RE
//  - UI/Commentaires en FR, code en EN.
// ============================================================

// --- Configuration (WiFi/MQTT/BLE) ---
static const char* app_name = "SZ→MQTT Gateway";
static const char* version  = "0.1.0";

WiFiMulti wifiMulti;
static const char* mqtt_server = "srv.lpb.ovh";
static const uint16_t mqtt_port = 1883;
static const char* mqtt_user = "van";
static const char* mqtt_pass = ".V@n";
static const char* mqtt_client_id = "Jimny_S3_SZ";

static const char* vLinker_MAC = "04:47:07:6A:AD:87";

static const char* topic_status = "jimny/status";
static const char* topic_sz     = "jimny/szviewer";
static const char* topic_debug  = "jimny/szviewer/raw";

// --- FSM States ---
enum SystemState {
  BOOT,
  CONNECT_WIFI,
  CONNECT_MQTT,
  CONNECT_BLE,
  INIT_ELM,
  POLL_SZ,
};
static SystemState currentState = BOOT;

// --- Globals ---
WiFiClient espClient;
PubSubClient mqtt(espClient);

static BLEClient* bleClient = nullptr;
static BLERemoteCharacteristic* chrWrite = nullptr;
static BLERemoteCharacteristic* chrNotify = nullptr;

static bool bleConnected = false;
static bool gotNotify = false;

static String rxBuf;
static unsigned long lastPollMs = 0;
static unsigned long lastBleAttemptMs = 0;

// --- vLinker: la plupart des dongles exposent un service type "UART" ---
// On scanne dynamiquement et on sélectionne une caractéristique "write" + une "notify".
// Ça évite de dépendre d'UUIDs fixes (NUS vs HM-10 like vs vendor).

static void notifyCallback(BLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
  gotNotify = true;
  for (size_t i = 0; i < len; i++) rxBuf += (char)data[i];
}

static bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  return mqtt.connect(mqtt_client_id, mqtt_user, mqtt_pass);
}

static void wifiTick() {
  if (wifiMulti.run() == WL_CONNECTED) return;
}

static bool bleFindUartLikeChars(BLEClient* client) {
  chrWrite = nullptr;
  chrNotify = nullptr;

  std::map<std::string, BLERemoteService*>* services = client->getServices();
  if (!services) return false;

  for (auto const& it : *services) {
    BLERemoteService* svc = it.second;
    if (!svc) continue;
    auto* chrs = svc->getCharacteristics();
    if (!chrs) continue;

    for (auto const& cit : *chrs) {
      BLERemoteCharacteristic* ch = cit.second;
      if (!ch) continue;

      // Heuristique:
      // - un char "write" (WRITE ou WRITE_NO_RSP)
      // - un char "notify" (NOTIFY) pour récupérer les réponses ELM ('>' etc.)
      if (!chrWrite && (ch->canWrite() || ch->canWriteNoResponse())) chrWrite = ch;
      if (!chrNotify && ch->canNotify()) chrNotify = ch;

      if (chrWrite && chrNotify) break;
    }
    if (chrWrite && chrNotify) break;
  }

  if (!chrWrite || !chrNotify) return false;
  chrNotify->registerForNotify(notifyCallback);
  return true;
}

static bool bleConnectVlinker() {
  rxBuf = "";
  gotNotify = false;

  if (!bleClient) bleClient = BLEDevice::createClient();
  if (bleClient->isConnected()) return true;

  BLEAddress addr(vLinker_MAC);
  if (!bleClient->connect(addr)) return false;

  if (!bleFindUartLikeChars(bleClient)) {
    bleClient->disconnect();
    return false;
  }
  return true;
}

// --- Helpers ELM style ---
static void elmClearRx() {
  rxBuf = "";
  gotNotify = false;
}

static bool elmWriteLine(const String& line) {
  if (!bleClient || !bleClient->isConnected() || !chrWrite) return false;
  String cmd = line;
  if (!cmd.endsWith("\r")) cmd += "\r";
  chrWrite->writeValue((uint8_t*)cmd.c_str(), cmd.length(), false);
  return true;
}

static bool elmReadUntilPrompt(String& out, uint32_t timeoutMs = 1500) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    // RX buffer rempli via notifications
    int idx = rxBuf.indexOf('>');
    if (idx >= 0) {
      out = rxBuf.substring(0, idx);
      // on garde ce qui suit le prompt (rare mais safe)
      rxBuf = rxBuf.substring(idx + 1);
      return true;
    }
    delay(5);
  }
  out = rxBuf;
  return false;
}

static String elmQuery(const String& line, uint32_t timeoutMs = 1500) {
  elmClearRx();
  elmWriteLine(line);
  String resp;
  elmReadUntilPrompt(resp, timeoutMs);
  resp.replace("\r", "");
  resp.replace("\n", "");
  resp.trim();
  return resp;
}

// Convertit une chaîne hex ASCII (ex: "61A0FF...") en bytes.
// Ignore espaces, ':' etc.
static size_t hexToBytes(const String& hex, uint8_t* out, size_t outMax) {
  auto isHex = [](char c) -> bool {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  auto hexVal = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    return (uint8_t)(10 + (c - 'A'));
  };

  size_t n = 0;
  int i = 0;
  while (i < (int)hex.length() && n < outMax) {
    while (i < (int)hex.length() && !isHex(hex[i])) i++;
    if (i >= (int)hex.length()) break;
    char hi = hex[i++];
    while (i < (int)hex.length() && !isHex(hex[i])) i++;
    if (i >= (int)hex.length()) break;
    char lo = hex[i++];
    out[n++] = (uint8_t)((hexVal(hi) << 4) | hexVal(lo));
  }
  return n;
}

// --- Décodage (à compléter) ---
// Ici on pose la structure: on reçoit les pages brutes et on sort les 20 champs.
// Tant que les offsets/échelles ne sont pas confirmés, on publie null + raw.
struct SzData {
  // 20 champs (noms calqués sur l'écran SZ Viewer)
  float desired_idle_speed_rpm = NAN;
  float accelerator_pct = NAN;
  float intake_c = NAN;
  float battery_v = NAN;
  float fuel_temp_c = NAN;
  float bar_pressure_kpa = NAN;
  float bar_pressure_mmhg = NAN;
  float abs_pressure_mbar = NAN;
  float air_flow_estimate_mgcp = NAN;
  float air_flow_request_mgcp = NAN;

  float speed_kmh = NAN;
  float rail_pressure_bar = NAN;
  float rail_pressure_control_bar = NAN;
  float desired_egr_position_pct = NAN;
  float gear_ratio = NAN;
  float egr_position_pct = NAN;
  float engine_temp_c = NAN;
  float air_temp_c = NAN;
  float requested_in_pressure_mbar = NAN;
  float engine_rpm = NAN;
};

static void decodeSzFromPages(
  const uint8_t* /*a0*/, size_t /*a0Len*/,
  const uint8_t* /*a2*/, size_t /*a2Len*/,
  const uint8_t* /*a5*/, size_t /*a5Len*/,
  const uint8_t* /*cd*/, size_t /*cdLen*/,
  SzData& out
) {
  // TODO (RE): compléter les offsets/échelles une fois la correspondance établie.
  // Stratégie conseillée:
  // - loguer en parallèle les valeurs SZ Viewer + raw 21A0/21A2/21A5/21CD
  // - identifier les bytes qui bougent (rpm, temp, pressions…)
  // - appliquer scale/offset (souvent /10, /100, -40 etc.)
  (void)out;
}

// --- JSON publish ---
static void publishSzJson(const SzData& d, const String& rawA0, const String& rawA2, const String& rawA5, const String& rawCD) {
  if (!mqttEnsureConnected()) return;

  StaticJsonDocument<1024> doc;
  doc["app"] = app_name;
  doc["ver"] = version;
  doc["ts_ms"] = (uint32_t)millis();

  auto putOrNull = [&](const char* k, float v) {
    if (isnan(v)) doc[k] = nullptr;
    else doc[k] = v;
  };

  // 20 champs
  putOrNull("desired_idle_speed_rpm", d.desired_idle_speed_rpm);
  putOrNull("accelerator_pct", d.accelerator_pct);
  putOrNull("intake_c", d.intake_c);
  putOrNull("battery_v", d.battery_v);
  putOrNull("fuel_temp_c", d.fuel_temp_c);
  putOrNull("bar_pressure_kpa", d.bar_pressure_kpa);
  putOrNull("bar_pressure_mmhg", d.bar_pressure_mmhg);
  putOrNull("abs_pressure_mbar", d.abs_pressure_mbar);
  putOrNull("air_flow_estimate_mgcp", d.air_flow_estimate_mgcp);
  putOrNull("air_flow_request_mgcp", d.air_flow_request_mgcp);

  putOrNull("speed_kmh", d.speed_kmh);
  putOrNull("rail_pressure_bar", d.rail_pressure_bar);
  putOrNull("rail_pressure_control_bar", d.rail_pressure_control_bar);
  putOrNull("desired_egr_position_pct", d.desired_egr_position_pct);
  putOrNull("gear_ratio", d.gear_ratio);
  putOrNull("egr_position_pct", d.egr_position_pct);
  putOrNull("engine_temp_c", d.engine_temp_c);
  putOrNull("air_temp_c", d.air_temp_c);
  putOrNull("requested_in_pressure_mbar", d.requested_in_pressure_mbar);
  putOrNull("engine_rpm", d.engine_rpm);

  // Debug raw (pour RE)
  JsonObject raw = doc.createNestedObject("raw");
  raw["21A0"] = rawA0;
  raw["21A2"] = rawA2;
  raw["21A5"] = rawA5;
  raw["21CD"] = rawCD;

  char payload[1024];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic_sz, (uint8_t*)payload, n);

  // En option: publier les raw seuls (topic dédié)
  if (rawA0.length() || rawA2.length() || rawA5.length() || rawCD.length()) {
    StaticJsonDocument<768> dbg;
    dbg["ts_ms"] = (uint32_t)millis();
    dbg["21A0"] = rawA0;
    dbg["21A2"] = rawA2;
    dbg["21A5"] = rawA5;
    dbg["21CD"] = rawCD;
    char p2[768];
    size_t n2 = serializeJson(dbg, p2, sizeof(p2));
    mqtt.publish(topic_debug, (uint8_t*)p2, n2);
  }
}

static bool elmInitLikeSzViewer() {
  // Séquence extraite de `medias/trames.log`
  // NB: certains dongles renvoient "BUS INIT: ERROR" au début; SZ Viewer réessaie.
  elmQuery("ATZ", 2000);
  elmQuery("ATD");
  elmQuery("ATE0");
  elmQuery("ATL0");
  elmQuery("ATS0");
  elmQuery("ATH0");
  elmQuery("ATD0");
  elmQuery("ATAL");
  elmQuery("ATIB10");
  elmQuery("ATKW0");
  elmQuery("ATSW00");
  elmQuery("ATAT0");
  elmQuery("ATCAF1");
  elmQuery("ATCFC1");
  elmQuery("ATFCSM0");
  elmQuery("ATTP5");
  elmQuery("ATSH817AF1");
  elmQuery("ATST19");

  String fi = elmQuery("ATFI", 2500);
  if (fi.indexOf("ERROR") >= 0) {
    // On retente une fois (comme SZ Viewer)
    delay(200);
    fi = elmQuery("ATFI", 2500);
  }
  // ATKW (optionnel; dans le log: "1:D0 2:8F")
  elmQuery("ATKW");
  return true;
}

static bool pollSzPagesAndPublish() {
  // Pages principales vues dans le log: 21A0, 21A2, 21A5, 21CD (+ parfois 2180/17FF00)
  String rA0 = elmQuery("21A0 1", 1200);
  String rA2 = elmQuery("21A2 1", 1200);
  String rA5 = elmQuery("21A5 1", 1200);
  String rCD = elmQuery("21CD 1", 1200);

  // Convert to bytes for decoding (future)
  uint8_t bA0[256], bA2[256], bA5[256], bCD[64];
  size_t nA0 = hexToBytes(rA0, bA0, sizeof(bA0));
  size_t nA2 = hexToBytes(rA2, bA2, sizeof(bA2));
  size_t nA5 = hexToBytes(rA5, bA5, sizeof(bA5));
  size_t nCD = hexToBytes(rCD, bCD, sizeof(bCD));

  SzData d;
  decodeSzFromPages(bA0, nA0, bA2, nA2, bA5, nA5, bCD, nCD, d);

  publishSzJson(d, rA0, rA2, rA5, rCD);
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("\n========================================");
  Serial.printf("  APP: %s\n", app_name);
  Serial.printf("  VER: %s\n", version);
  Serial.println("========================================");

  // --- WiFi Multi-AP (adapter à ton contexte) ---
  // IMPORTANT: garde tes APs existants si tu merges avec ton autre sketch.
  wifiMulti.addAP("LPB", "lapet1teb0rde");               // Réseau Maison
  wifiMulti.addAP("<_JNoel iPhone_>", "bachibouzook");   // iPhone

  mqtt.setServer(mqtt_server, mqtt_port);

  setCpuFrequencyMhz(160);
  BLEDevice::init("CW-S3-F4IAE");
  bleClient = BLEDevice::createClient();
}

void loop() {
  wifiTick();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (mqttEnsureConnected()) {
        mqtt.publish(topic_status, "WiFi OK, MQTT OK");
      }
    }
    mqtt.loop();
  }

  switch (currentState) {
    case BOOT:
      if (WiFi.status() == WL_CONNECTED) currentState = CONNECT_MQTT;
      break;

    case CONNECT_MQTT:
      if (mqttEnsureConnected()) currentState = CONNECT_BLE;
      break;

    case CONNECT_BLE:
      if (millis() - lastBleAttemptMs > 5000) {
        lastBleAttemptMs = millis();
        Serial.println("[FSM] Connexion BLE au vLinker…");
        if (bleConnectVlinker()) {
          bleConnected = true;
          Serial.println("[FSM] BLE OK (write+notify trouvés)");
          mqtt.publish(topic_status, "BLE OK");
          currentState = INIT_ELM;
        } else {
          bleConnected = false;
          Serial.println("[FSM] BLE KO (retry)");
        }
      }
      break;

    case INIT_ELM:
      if (!bleClient->isConnected()) {
        bleConnected = false;
        currentState = CONNECT_BLE;
        break;
      }
      Serial.println("[FSM] Init ELM like SZ Viewer…");
      elmInitLikeSzViewer();
      mqtt.publish(topic_status, "ELM init done");
      currentState = POLL_SZ;
      break;

    case POLL_SZ:
      if (!bleClient->isConnected()) {
        bleConnected = false;
        currentState = CONNECT_BLE;
        break;
      }
      if (millis() - lastPollMs > 500) { // à ajuster selon charge bus
        lastPollMs = millis();
        pollSzPagesAndPublish();
      }
      break;

    default:
      currentState = BOOT;
      break;
  }
}

