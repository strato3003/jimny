#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
// Buffer MQTT: doit être défini AVANT d'inclure PubSubClient
// Les messages JSON font ~1500 bytes, on prend une marge
#define MQTT_MAX_PACKET_SIZE 2048
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ============================================================
//  ESP32 "AU CHAUD" : Relecture d'un fichier de trames / synchro
//  et publication MQTT des données SZ Viewer en JSON.
//
//  Deux modes:
//  1) RECOMMANDÉ: `sz_sync_ocr.jsonl`
//     - contient `raw` (21A0/21A2/21A5/21CD) + `values` (20 champs OCR)
//     - permet de simuler directement le flux JSON final
//
//  2) Fallback: `trames.log`
//     - publie uniquement `raw` regroupé par seconde (21A0/21A2/21A5/21CD)
//     - (pas de décodage des 20 champs sans table d'échelles/offsets)
//
//  MODE EMBARQUÉ (sans SPIFFS):
//  - Définis USE_EMBEDDED_DATA pour utiliser les données compilées dans le sketch.
//  - Génère le tableau avec: python3 tools/sz_embed.py medias/sz_sync_ocr.jsonl
//  - Copie-colle le résultat dans sz_data.h (ou inclus-le ici).
// ============================================================

// Active le mode embarqué (désactive SPIFFS)
#define USE_EMBEDDED_DATA

#ifdef USE_EMBEDDED_DATA
  // Inclus le fichier généré par sz_embed.py
  #include "sz_data.h"
#endif

// --- Identité ---
static const char* app_name = "SZ Replay MQTT";
static const char* version  = "0.2.0";

// --- WiFi/MQTT ---
WiFiMulti wifiMulti;
static const char* mqtt_server = "srv.lpb.ovh";
static const uint16_t mqtt_port = 1883;
static const char* mqtt_user = "van";
static const char* mqtt_pass = ".V@n";
static const char* mqtt_client_id = "Jimny_S3_SZ_REPLAY";

static const char* topic_status = "jimny/status";
static const char* topic_sz     = "jimny/szviewer";
static const char* topic_debug  = "jimny/szviewer/raw";

// --- Fichiers SPIFFS ---
static const char* PATH_SYNC_OCR = "/sz_sync_ocr.jsonl";
static const char* PATH_TRAMES   = "/trames.log";

// --- Timing replay ---
// En mode sync jsonl: on publie 1 ligne toutes les X ms.
static const uint32_t REPLAY_PERIOD_MS = 500;

WiFiClient espClient;
PubSubClient mqtt(espClient);

enum ReplayMode { MODE_NONE, MODE_SYNC_OCR, MODE_TRAMES_RAW };
static ReplayMode mode = MODE_NONE;

static File replayFile;
static uint32_t lastPublishMs = 0;

#ifdef USE_EMBEDDED_DATA
  static uint16_t embeddedLineIdx = 0;
  static const uint16_t embeddedLineCount = SZ_DATA_LINE_COUNT;
#endif

static bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  return mqtt.connect(mqtt_client_id, mqtt_user, mqtt_pass);
}

static void wifiTick() {
  wifiMulti.run();
}

// --- Utils lecture ligne ---
static bool readLine(File& f, String& out) {
  out = "";
  if (!f || !f.available()) return false;
  out = f.readStringUntil('\n');
  out.trim();
  return out.length() > 0;
}

// --- Publication d'un JSON (déjà formatté) ---
static void publishJson(const char* topic, const String& json) {
  if (!mqttEnsureConnected()) return;
  mqtt.publish(topic, json.c_str());
}

// --- MODE 1 : lecture `sz_sync_ocr.jsonl` ---
static bool replaySyncOcrLine(const String& line) {
  // Chaque ligne est déjà un JSON complet:
  // { frame, frame_idx, t_offset_s, log_hhmmss, raw:{...}, values:{...}, ... }
  // On republie un JSON "compact" orienté flux capteurs.

  StaticJsonDocument<4096> in;
  DeserializationError err = deserializeJson(in, line);
  if (err) return false;

  StaticJsonDocument<1536> out;
  out["app"] = app_name;
  out["ver"] = version;
  out["mode"] = "replay";
  out["ts_ms"] = (uint32_t)millis();
  out["log_hhmmss"] = in["log_hhmmss"] | "";
  out["frame_idx"] = in["frame_idx"] | 0;

  // 20 champs SZ Viewer (issus OCR)
  JsonObject valuesIn = in["values"].as<JsonObject>();
  JsonObject valuesOut = out.createNestedObject("values");
  for (JsonPair kv : valuesIn) {
    valuesOut[kv.key()] = kv.value();
  }

  // Raw pages
  JsonObject rawIn = in["raw"].as<JsonObject>();
  JsonObject rawOut = out.createNestedObject("raw");
  rawOut["21A0"] = rawIn["21A0"];
  rawOut["21A2"] = rawIn["21A2"];
  rawOut["21A5"] = rawIn["21A5"];
  rawOut["21CD"] = rawIn["21CD"];

  char payload[1536];
  size_t n = serializeJson(out, payload, sizeof(payload));
  if (n >= sizeof(payload)) {
    Serial.printf("[PUB] ERREUR: JSON trop gros (%d >= %d)\n", n, sizeof(payload));
    return false;
  }
  if (!mqtt.connected()) {
    Serial.println("[PUB] ERREUR: MQTT non connecté");
    return false;
  }
  bool pub1 = mqtt.publish(topic_sz, (uint8_t*)payload, n, false); // QoS 0, no retain
  Serial.printf("[PUB] %s: %s (%d bytes, buffer=%d)\n", topic_sz, pub1 ? "OK" : "FAIL", n, mqtt.getBufferSize());

  // Optionnel: raw-only debug
  StaticJsonDocument<768> dbg;
  dbg["ts_ms"] = (uint32_t)millis();
  dbg["log_hhmmss"] = in["log_hhmmss"] | "";
  dbg["21A0"] = rawIn["21A0"];
  dbg["21A2"] = rawIn["21A2"];
  dbg["21A5"] = rawIn["21A5"];
  dbg["21CD"] = rawIn["21CD"];
  char payload2[1024]; // Augmenté à 1024 pour avoir une marge
  size_t n2 = serializeJson(dbg, payload2, sizeof(payload2));
  if (n2 >= sizeof(payload2)) {
    Serial.printf("[PUB] ERREUR: JSON debug trop gros (%d >= %d)\n", n2, sizeof(payload2));
    return false;
  }
  bool pub2 = mqtt.publish(topic_debug, (uint8_t*)payload2, n2, false); // QoS 0, no retain
  Serial.printf("[PUB] %s: %s (%d bytes)\n", topic_debug, pub2 ? "OK" : "FAIL", n2);

  return true;
}

// --- MODE 2 : parsing minimal `trames.log` (raw-only) ---
// Objectif: regrouper les dernières réponses 21A0/21A2/21A5/21CD par seconde HH:MM:SS
static bool parseTramesAndPublishChunk(File& f) {
  String line;
  String currentTs = "";
  String currentCmd = "";
  String recvBuf = "";

  // On avance jusqu'à capter une commande 21A0/21A2/21A5/21CD + ses RECV
  while (readLine(f, line)) {
    // format: [17:36:53] send: 21A0 1
    int lb = line.indexOf('[');
    int rb = line.indexOf(']');
    if (lb < 0 || rb < 0) continue;
    String ts = line.substring(lb + 1, rb);

    int sIdx = line.indexOf("send:");
    int rIdx = line.indexOf("RECV:");
    if (sIdx > 0) {
      String cmd = line.substring(sIdx + 5);
      cmd.trim();
      currentTs = ts;
      currentCmd = cmd;
      recvBuf = "";
      break;
    } else if (rIdx > 0) {
      continue;
    }
  }

  if (currentCmd.length() == 0) return false;
  String cmdU = currentCmd;
  cmdU.toUpperCase();
  if (!(cmdU.startsWith("21A0") || cmdU.startsWith("21A2") || cmdU.startsWith("21A5") || cmdU.startsWith("21CD"))) {
    return true; // on ignore et on continue
  }

  // Accumule RECV jusqu'au prochain "send:"
  while (f.available()) {
    int pos = f.position();
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.indexOf("send:") > 0) {
      // rewind: on veut que la prochaine itération relise cette ligne "send"
      f.seek(pos);
      break;
    }
    int rIdx = l.indexOf("RECV:");
    if (rIdx > 0) {
      String part = l.substring(rIdx + 5);
      part.trim();
      recvBuf += part;
    }
  }

  // Publie un JSON raw-only
  StaticJsonDocument<768> out;
  out["app"] = app_name;
  out["ver"] = version;
  out["mode"] = "trames_raw";
  out["ts_ms"] = (uint32_t)millis();
  out["log_hhmmss"] = currentTs;
  out["cmd"] = currentCmd;
  out["raw_hex_ascii"] = recvBuf; // brut comme dans le log

  char payload[768];
  size_t n = serializeJson(out, payload, sizeof(payload));
  mqtt.publish(topic_debug, (uint8_t*)payload, n);
  return true;
}

static void openReplayFile() {
#ifdef USE_EMBEDDED_DATA
  // Mode embarqué: utilise les données compilées
  mode = MODE_SYNC_OCR;
  embeddedLineIdx = 0;
  Serial.printf("[REPLAY] Mode SYNC_OCR (EMBEDDED): %d lignes\n", embeddedLineCount);
  if (mqtt.connected()) mqtt.publish(topic_status, "Replay mode: SYNC_OCR (embedded)");
  return;
#else
  // Mode SPIFFS: lit depuis le filesystem
  if (SPIFFS.exists(PATH_SYNC_OCR)) {
    mode = MODE_SYNC_OCR;
    replayFile = SPIFFS.open(PATH_SYNC_OCR, "r");
    Serial.printf("[REPLAY] Mode SYNC_OCR: %s\n", PATH_SYNC_OCR);
    mqtt.publish(topic_status, "Replay mode: SYNC_OCR");
    return;
  }
  if (SPIFFS.exists(PATH_TRAMES)) {
    mode = MODE_TRAMES_RAW;
    replayFile = SPIFFS.open(PATH_TRAMES, "r");
    Serial.printf("[REPLAY] Mode TRAMES_RAW: %s\n", PATH_TRAMES);
    mqtt.publish(topic_status, "Replay mode: TRAMES_RAW");
    return;
  }
  mode = MODE_NONE;
  Serial.println("[REPLAY] Aucun fichier trouvé dans SPIFFS.");
  mqtt.publish(topic_status, "Replay mode: NONE (missing files)");
#endif
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("\n========================================");
  Serial.printf("  APP: %s\n", app_name);
  Serial.printf("  VER: %s\n", version);
  Serial.println("========================================");

  // --- WiFi Multi-AP (adapter) ---
  wifiMulti.addAP("LPB", "lapet1teb0rde");
  wifiMulti.addAP("<_JNoel iPhone_>", "bachibouzook");

  mqtt.setServer(mqtt_server, mqtt_port);
  // Force la taille du buffer MQTT (nécessaire sur ESP32)
  mqtt.setBufferSize(2048);
  Serial.printf("[MQTT] Buffer size: %d bytes\n", mqtt.getBufferSize());

#ifdef USE_EMBEDDED_DATA
  Serial.println("[FS] Mode EMBARQUÉ (pas de SPIFFS)");
  Serial.printf("[FS] Données: %d lignes compilées\n", SZ_DATA_LINE_COUNT);
#else
  if (!SPIFFS.begin(true)) {
    Serial.println("[FS] SPIFFS init FAILED");
  } else {
    Serial.println("[FS] SPIFFS OK");
    Serial.printf("[FS] has %s: %s\n", PATH_SYNC_OCR, SPIFFS.exists(PATH_SYNC_OCR) ? "YES" : "NO");
    Serial.printf("[FS] has %s: %s\n", PATH_TRAMES, SPIFFS.exists(PATH_TRAMES) ? "YES" : "NO");
  }
#endif
}

void loop() {
  wifiTick();
  
  // Logs de diagnostic WiFi/MQTT
  static uint32_t lastDiagMs = 0;
  if (millis() - lastDiagMs > 5000) {
    lastDiagMs = millis();
    Serial.printf("[DIAG] WiFi: %s, MQTT: %s, Mode: %d\n", 
                  WiFi.status() == WL_CONNECTED ? "OK" : "KO",
                  mqtt.connected() ? "OK" : "KO",
                  mode);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[DIAG] SSID: %s, IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      Serial.println("[MQTT] Tentative de connexion...");
      if (mqttEnsureConnected()) {
        Serial.println("[MQTT] Connecté !");
        mqtt.publish(topic_status, "WiFi OK, MQTT OK (replay)");
      } else {
        Serial.println("[MQTT] Échec connexion");
      }
    }
    mqtt.loop();
  }

  if (mode == MODE_NONE) {
    // En mode embarqué, on peut démarrer même sans WiFi (pour debug)
#ifdef USE_EMBEDDED_DATA
    openReplayFile();
#else
    if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
      if (SPIFFS.begin(true)) {
        openReplayFile();
      }
    }
#endif
    delay(200);
    return;
  }

#ifndef USE_EMBEDDED_DATA
  if (!replayFile) {
    // fin de fichier ou erreur: on repart au début
    openReplayFile();
    delay(200);
    return;
  }
#endif

  if (millis() - lastPublishMs < REPLAY_PERIOD_MS) return;
  lastPublishMs = millis();

  if (mode == MODE_SYNC_OCR) {
    String line;
#ifdef USE_EMBEDDED_DATA
    // Mode embarqué: lit depuis le tableau PROGMEM
    if (embeddedLineIdx >= embeddedLineCount) {
      embeddedLineIdx = 0; // reboucle
    }
    line = String(sz_data_lines[embeddedLineIdx]);
    embeddedLineIdx++;
#else
    // Mode SPIFFS: lit depuis le fichier
    if (!readLine(replayFile, line)) {
      replayFile.close();
      openReplayFile();
      return;
    }
#endif
    if (!replaySyncOcrLine(line)) {
      // si ligne illisible, on saute
      return;
    }
  } else if (mode == MODE_TRAMES_RAW) {
    if (!parseTramesAndPublishChunk(replayFile)) {
      replayFile.close();
      openReplayFile();
      return;
    }
  }
}

