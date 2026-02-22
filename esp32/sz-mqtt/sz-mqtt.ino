// NOTE Arduino (pré-processeur .ino):
// Arduino génère automatiquement des prototypes de fonctions en haut du fichier.
// Si un prototype mentionne un type défini plus bas (ex: LedPattern), la compilation échoue.
// → On déclare LedPattern AVANT les #include pour être visible dans les prototypes auto-générés.
struct LedPattern {
  unsigned long period_ms;     // 0 = fixe, >0 = clignotant
  unsigned char duty_cycle;    // 0-100 (% de temps ON)
};

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <time.h>
// Buffer MQTT: doit être défini AVANT d'inclure PubSubClient
// Les messages JSON font ~985 bytes, on prend une marge
#define MQTT_MAX_PACKET_SIZE 2048
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <ArduinoJson.h>
#include "sz_decode.h"
#include "obd2_decode.h"

// 1 = protocole OBD2 (PIDs mode 01, comme Car Scanner) ; 0 = pages KWP SZ (21A0/21A2/21A5/21CD)
#ifndef USE_OBD2_PROTOCOL
#define USE_OBD2_PROTOCOL 1
#endif

// ============================================================
//  ESP32 → vLinker (BLE) → Requêtes type SZ Viewer → MQTT JSON
//  Inspiré de `esp32/vlinker-gw.ino`
//
//  Contexte véhicule:
//  - Adaptateur: vLinker (OBD2 Bluetooth)
//  - Véhicule: Suzuki Jimny DDIS 1.5 (2008)
//  - Moteur: Renault K9K (diesel)
//  - Protocole: KWP2000 (Keyword Protocol 2000) - PIDs propriétaires
//  - PIDs utilisés: 21A0, 21A2, 21A5, 21CD (PIDs étendus/propriétaires)
//
//  Remarques importantes:
//  - Les 20 valeurs visibles dans SZ Viewer sont issues de requêtes
//    propriétaires (ex: 21A0, 21A2, 21A5, 21CD, 2180, 17FF00…).
//  - Décodage des 20 champs implémenté via analyse OCR des frames SZ Viewer
//  - Les réponses brutes (hex) sont également publiées pour debug
//  - UI/Commentaires en FR, code en EN.
// ============================================================

// --- Configuration (WiFi/MQTT/BLE) ---
static const char* app_name = "SZ→MQTT Gateway";
static const char* version  = "0.4.13";

WiFiMulti wifiMulti;
static const char* mqtt_server = "srv.lpb.ovh";
static const uint16_t mqtt_port = 1883;
static const char* mqtt_user = "van";
static const char* mqtt_pass = ".V@n";
static const char* mqtt_client_id = "Jimny_S3_SZ";

// Configuration NTP pour obtenir l'heure réelle
static const char* ntpServer = "pool.ntp.org";
static const long gmtOffset_sec = 0;  // UTC par défaut (ajuster si nécessaire: +3600 pour UTC+1, etc.)
static const int daylightOffset_sec = 0;
static bool ntpConfigured = false;

// Recherche par nom BLE (contient "vLinker") au lieu de MAC fixe
static const char* vLinker_NamePattern = "vLinker";

static const char* topic_status = "jimny/status";
static const char* topic_sz     = "jimny/szviewer";
static const char* topic_debug  = "jimny/szviewer/raw";
static const char* topic_dtc    = "jimny/dtc";

// --- FSM States ---
// Priorité: 1) Internet (WiFi + NTP) pour traces horodatées, 2) MQTT, 3) BLE.
// Si le WiFi tombe en CONNECT_BLE/INIT_ELM/POLL_SZ → retour CONNECT_WIFI pour le rétablir.
enum SystemState {
  BOOT,
  CONNECT_WIFI,
  CONNECT_MQTT,
  CONNECT_BLE,
  INIT_ELM,
  POLL_SZ,
};
static SystemState currentState = BOOT;

// --- LEDs (XIAO ESP32S3: 2 LEDs simples) ---
// LED orange (utilisateur) pour WiFi/MQTT
// LED rouge pour BLE/OBD
#ifndef LED_BUILTIN
  #define LED_BUILTIN 21  // Pin par défaut si non défini
#endif
#define LED_WIFI_PIN LED_BUILTIN  // LED orange (utilisateur) - généralement pin 21
// NOTE: Le pin 34 est INPUT ONLY sur ESP32 (GPIO 34, 35, 36, 39 sont INPUT ONLY)
// Solution: Utiliser UNIQUEMENT la LED orange avec des patterns combinés
// La LED rouge n'est pas accessible en GPIO sur cette board
#define USE_SINGLE_LED true       // Utiliser une seule LED pour WiFi et BLE

// Certaines boards ont une LED "active low" (LOW=ON, HIGH=OFF).
// XIAO ESP32S3: LED user souvent active-low.
#ifndef LED_WIFI_ACTIVE_LOW
  #if defined(ARDUINO_XIAO_ESP32S3) || defined(ARDUINO_SEEED_XIAO_ESP32S3) || defined(ARDUINO_SEEED_XIAO_ESP32S3_SENSE)
    #define LED_WIFI_ACTIVE_LOW 1
  #else
    #define LED_WIFI_ACTIVE_LOW 1
  #endif
#endif

static inline void ledWifiWrite(bool on) {
#if LED_WIFI_ACTIVE_LOW
  digitalWrite(LED_WIFI_PIN, on ? LOW : HIGH);
#else
  digitalWrite(LED_WIFI_PIN, on ? HIGH : LOW);
#endif
}

// Système de LEDs expressif
// Au moins une LED doit être allumée (fixe ou clignotante min 1s) si l'ESP est sous tension
// Fréquences: 1000ms (lent), 500ms (moyen), 250ms (rapide), 100ms (très rapide)
static uint32_t ledWifiLastToggle = 0;
static uint32_t ledBleLastToggle = 0;
static bool ledWifiState = false;
static bool ledBleState = false;

// Activité (overlay) — une impulsion visuelle à chaque échange BLE/ELM et chaque envoi MQTT.
// Une seule LED sur XIAO ESP32S3 SENSE, donc on encode l'activité par des "bursts" courts.
static volatile uint32_t ledBleActivityMs = 0;
static volatile uint32_t ledMqttActivityMs = 0;

// Marqueur "étape" (lié aux logs stdout): reset de phase + pattern distinct
static volatile uint32_t ledStepAtMs = 0;
static volatile uint32_t ledStepId = 0;

static inline void ledNoteBleActivity() { ledBleActivityMs = millis(); }
static inline void ledNoteMqttActivity() { ledMqttActivityMs = millis(); }

static inline void ledMarkStep() {
  uint32_t now = millis();
  ledStepAtMs = now;
  ledStepId++;
  // reset de phase pour que le changement d'étape soit visible immédiatement
  ledWifiLastToggle = now;
  ledBleLastToggle = now;
}

// Log + changement de pattern LED (chaque ligne stdout = step visible)
#define STEPLN(...) do { Serial.println(__VA_ARGS__); ledMarkStep(); } while(0)
#define STEPF(...)  do { Serial.printf(__VA_ARGS__); ledMarkStep(); } while(0)

// --- Morse méthode Koch (trains de clignotement = lettre en Morse) ---
// Ordre Koch classique: 1er train = K, 2e = M, 3e = R, etc. (. = dit, - = dah)
static const char* const kochMorse[] = {
  "-.-",   /* K */  "--",    /* M */  ".-.",   /* R */  "...",   /* S */  "..-",   /* U */
  ".-",    /* A */  ".--.",  /* P */  "-",     /* T */  ".",     /* E */  "-.",    /* N */
  "..",    /* I */  "---",   /* O */  "--.",   /* G */  "--.-",  /* Q */  "--..",  /* Z */
  "-.--",  /* Y */  "-.-.",  /* C */  "-..-",  /* X */  "-...",  /* B */  ".---",  /* J */
  ".-..",  /* L */  "..-.",  /* F */  "....", /* H */  "...-",  /* V */  "-..",   /* D */
  ".--"    /* W */
};
static const int kochCount = sizeof(kochMorse) / sizeof(kochMorse[0]);

// Retourne true si la LED doit être ON pour le Morse Koch (état mis à jour en interne).
static bool ledMorseKochShouldBeOn(uint32_t now) {
  const uint32_t unit_ms = 55;  // 1 unité = 55 ms (dit=55, dah=165, espace lettre=165)
  static int letterIndex = -1;
  static int symbolIndex = 0;
  static int subPhase = 0;      // 0=ON (dit/dah), 1=OFF (gap sym), 2=OFF (gap lettre)
  static uint32_t phaseStartMs = 0;

  int stepLetter = (int)((uint32_t)ledStepId % (uint32_t)kochCount);
  if (letterIndex != stepLetter || phaseStartMs == 0) {
    letterIndex = stepLetter;
    symbolIndex = 0;
    subPhase = 0;
    phaseStartMs = now;
  }

  const char* m = kochMorse[letterIndex];
  int len = (int)strlen(m);
  uint32_t duration = 0;
  if (subPhase == 0) {
    duration = (m[symbolIndex] == '-') ? (3 * unit_ms) : (1 * unit_ms);
  } else if (subPhase == 1) {
    duration = 1 * unit_ms;
  } else {
    duration = 3 * unit_ms;
  }

  for (int guard = 0; guard < 64 && (uint32_t)(now - phaseStartMs) >= duration; guard++) {
    phaseStartMs += duration;
    if (subPhase == 0) {
      subPhase = 1;
      duration = 1 * unit_ms;
    } else if (subPhase == 1) {
      symbolIndex++;
      if (symbolIndex >= len) {
        subPhase = 2;
        duration = 3 * unit_ms;
      } else {
        subPhase = 0;
        duration = (m[symbolIndex] == '-') ? (3 * unit_ms) : (1 * unit_ms);
      }
    } else {
      symbolIndex = 0;
      subPhase = 0;
      duration = (m[symbolIndex] == '-') ? (3 * unit_ms) : (1 * unit_ms);
    }
  }

  return (subPhase == 0);
}

// LED WiFi: heartbeat (jamais fixe ON), vitesse + "intensité" (duty) selon l'état.
// Contrainte: fréquence >= 0.5 Hz (période <= 2000 ms)
static LedPattern getWifiPattern(SystemState state, bool wifiOk, bool mqttOk) {
  LedPattern p = {1000, 35}; // 1 Hz par défaut
  if (state == BOOT) {
    p.period_ms = 200; p.duty_cycle = 50;
  } else if (state == CONNECT_WIFI) {
    p.period_ms = 300; p.duty_cycle = 20;
  } else if (state == CONNECT_MQTT) {
    p.period_ms = 250; p.duty_cycle = 30;
  } else if (wifiOk && mqttOk) {
    p.period_ms = 1000; p.duty_cycle = 40;
  } else if (wifiOk && !mqttOk) {
    p.period_ms = 500; p.duty_cycle = 25;
  } else {
    p.period_ms = 800; p.duty_cycle = 25;
  }
  return p;
}

// LED BLE: pas de "fixe ON", juste des pulses (et overlay d'activité par dessus).
static LedPattern getBlePattern(SystemState state, bool bleOk) {
  LedPattern p = {1000, 25};
  if (state == BOOT || state == CONNECT_WIFI || state == CONNECT_MQTT) {
    p.period_ms = 0;   p.duty_cycle = 0;
  } else if (state == CONNECT_BLE) {
    p.period_ms = 250; p.duty_cycle = 25;
  } else if (state == INIT_ELM) {
    p.period_ms = 200; p.duty_cycle = 35;
  } else if (state == POLL_SZ && bleOk) {
    p.period_ms = 500; p.duty_cycle = 20;
  } else if (bleOk) {
    p.period_ms = 800; p.duty_cycle = 25;
  } else {
    p.period_ms = 400; p.duty_cycle = 20;
  }
  return p;
}

// --- Globals ---
WiFiClient espClient;
PubSubClient mqtt(espClient);

static BLEClient* bleClient = nullptr;
static BLERemoteCharacteristic* chrWrite = nullptr;
static BLERemoteCharacteristic* chrNotify = nullptr;

static bool bleConnected = false;
static bool gotNotify = false;
static BLEAddress vLinkerAddress("00:00:00:00:00:00"); // Adresse BLE du vLinker trouvé (init avec adresse vide)
static BLEAdvertisedDevice* vLinkerDevice = nullptr; // Device complet du vLinker (pour connexion)
static bool vLinkerAddressValid = false;

static String rxBuf;
static unsigned long lastPollMs = 0;
static unsigned long lastBleAttemptMs = 0;

// --- Buffer FIFO pour les trames collectées ---
// Priorité: collecte continue même sans WiFi, envoi MQTT conditionnel
// Taille calculée dynamiquement en fonction de la mémoire disponible
struct FifoEntry {
  SzData data;
  String rawA0;
  String rawA2;
  String rawA5;
  String rawCD;
  uint32_t timestamp_ms;
  bool valid;
};

// Estimation de la taille d'une entrée (SzData + 4 String + overhead)
// SzData: 20 floats = 80 bytes
// 4 String avec réponses hex (~200 bytes chacune) + overhead String (~16 bytes) = ~864 bytes
// timestamp_ms: 4 bytes, bool: 1 byte
// Total estimé: ~950 bytes par entrée (avec marge)
#define ESTIMATED_ENTRY_SIZE 1000  // Estimation conservatrice en bytes

static FifoEntry* fifoBuffer = nullptr;  // Buffer alloué dynamiquement
static int FIFO_BUFFER_SIZE = 0;  // Taille calculée au démarrage
static int fifoHead = 0;  // Index d'écriture (prochaine position libre)
static int fifoTail = 0;  // Index de lecture (prochaine trame à envoyer)
static int fifoCount = 0; // Nombre de trames dans le buffer

// Calcule et alloue le buffer FIFO en fonction de la mémoire disponible
static void fifoInit() {
  // Obtenir la mémoire libre disponible
  uint32_t freeHeapBefore = ESP.getFreeHeap();
  STEPF("[FIFO] Mémoire libre avant allocation: %u bytes\n", freeHeapBefore);
  
  // Réserver seulement 20% de la mémoire libre pour le buffer FIFO
  // (le reste est nécessaire pour WiFi, MQTT, BLE, JSON, etc.)
  uint32_t availableForFifo = (freeHeapBefore * 20) / 100;
  STEPF("[FIFO] Mémoire réservée pour FIFO (20%%): %u bytes\n", availableForFifo);
  
  // Calculer le nombre d'entrées possibles
  int maxEntries = availableForFifo / ESTIMATED_ENTRY_SIZE;
  
  // Limite minimale uniquement (pour garantir un fonctionnement de base)
  // Pas de limite maximale : utiliser toute la mémoire disponible si nécessaire
  if (maxEntries < 10) {
    maxEntries = 10;
    STEPLN("[FIFO] Mémoire limitée, utilisation du minimum (10 entrées)");
  }
  
  // Afficher un avertissement si le buffer est très grand (pour info)
  if (maxEntries > 1000) {
    STEPF("[FIFO] Mémoire abondante: %d entrées allouées (~%u KB)\n", 
                  maxEntries, (maxEntries * ESTIMATED_ENTRY_SIZE) / 1024);
  }
  
  FIFO_BUFFER_SIZE = maxEntries;
  
  // Allouer le buffer
  fifoBuffer = new FifoEntry[FIFO_BUFFER_SIZE];
  if (!fifoBuffer) {
    STEPLN("[FIFO] ERREUR: Échec d'allocation mémoire !");
    FIFO_BUFFER_SIZE = 0;
    return;
  }
  
  // Initialiser toutes les entrées
  for (int i = 0; i < FIFO_BUFFER_SIZE; i++) {
    fifoBuffer[i].valid = false;
  }
  
  // Vérifier la mémoire après allocation
  uint32_t freeHeapAfter = ESP.getFreeHeap();
  uint32_t memoryUsed = freeHeapBefore - freeHeapAfter;
  
  STEPF("[FIFO] Buffer alloué: %d entrées\n", FIFO_BUFFER_SIZE);
  STEPF("[FIFO] Mémoire utilisée: %u bytes (estimation: ~%u bytes)\n",
                memoryUsed, FIFO_BUFFER_SIZE * ESTIMATED_ENTRY_SIZE);
  STEPF("[FIFO] Mémoire libre après allocation: %u bytes\n", freeHeapAfter);
  
  // Avertissement si la mémoire libre est trop faible pour MQTT
  if (freeHeapAfter < 50000) {
    STEPF("[FIFO] ATTENTION: Mémoire libre faible (%u bytes) - MQTT pourrait avoir des problèmes\n", freeHeapAfter);
  }
}

// Ajoute une trame au buffer FIFO (retourne false si buffer plein)
static bool fifoPush(const SzData& d, const String& rawA0, const String& rawA2, const String& rawA5, const String& rawCD) {
  if (fifoCount >= FIFO_BUFFER_SIZE) {
    STEPF("[FIFO] Buffer plein (%d/%d), perte de trame\n", fifoCount, FIFO_BUFFER_SIZE);
    return false;
  }
  
  fifoBuffer[fifoHead].data = d;
  fifoBuffer[fifoHead].rawA0 = rawA0;
  fifoBuffer[fifoHead].rawA2 = rawA2;
  fifoBuffer[fifoHead].rawA5 = rawA5;
  fifoBuffer[fifoHead].rawCD = rawCD;
  fifoBuffer[fifoHead].timestamp_ms = millis();
  fifoBuffer[fifoHead].valid = true;
  
  fifoHead = (fifoHead + 1) % FIFO_BUFFER_SIZE;
  fifoCount++;
  return true;
}

// Récupère une trame du buffer FIFO (retourne false si buffer vide)
static bool fifoPop(SzData& d, String& rawA0, String& rawA2, String& rawA5, String& rawCD, uint32_t& timestamp_ms) {
  if (fifoCount == 0) {
    return false;
  }
  
  d = fifoBuffer[fifoTail].data;
  rawA0 = fifoBuffer[fifoTail].rawA0;
  rawA2 = fifoBuffer[fifoTail].rawA2;
  rawA5 = fifoBuffer[fifoTail].rawA5;
  rawCD = fifoBuffer[fifoTail].rawCD;
  timestamp_ms = fifoBuffer[fifoTail].timestamp_ms;
  
  fifoBuffer[fifoTail].valid = false;
  fifoTail = (fifoTail + 1) % FIFO_BUFFER_SIZE;
  fifoCount--;
  return true;
}

// --- vLinker: la plupart des dongles exposent un service type "UART" ---
// On scanne dynamiquement et on sélectionne une caractéristique "write" + une "notify".
// Ça évite de dépendre d'UUIDs fixes (NUS vs HM-10 like vs vendor).

static void notifyCallback(BLERemoteCharacteristic* /*c*/, uint8_t* data, size_t len, bool /*isNotify*/) {
  gotNotify = true;
  ledNoteBleActivity();
  String received;
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    rxBuf += c;
    received += c;
  }
  // Log seulement les caractères non-printables ou les réponses importantes
  if (received.indexOf(">") >= 0 || received.indexOf("OK") >= 0 || received.indexOf("ERROR") >= 0) {
    STEPF("[BLE] NOTIFY (%d bytes): %s\n", len, received.c_str());
  }
}

static bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  
  // Vérifier la mémoire avant connexion
  uint32_t freeHeap = ESP.getFreeHeap();
  STEPF("[MQTT] Tentative de connexion - mémoire libre: %u bytes\n", freeHeap);
  
  if (freeHeap < 20000) {
    STEPF("[MQTT] ATTENTION: Mémoire libre faible (%u bytes) - risque d'échec\n", freeHeap);
  }
  
  bool connected = mqtt.connect(mqtt_client_id, mqtt_user, mqtt_pass);
  
  if (!connected) {
    int mqttState = mqtt.state();
    const char* stateStr = "UNKNOWN";
    if (mqttState == -4) stateStr = "CONNECTION_TIMEOUT";
    else if (mqttState == -3) stateStr = "CONNECTION_LOST";
    else if (mqttState == -2) stateStr = "CONNECT_FAILED";
    else if (mqttState == -1) stateStr = "DISCONNECTED";
    else if (mqttState == 0) stateStr = "CONNECTED";
    else if (mqttState == 1) stateStr = "CONNECT_BAD_PROTOCOL";
    else if (mqttState == 2) stateStr = "CONNECT_BAD_CLIENT_ID";
    else if (mqttState == 3) stateStr = "CONNECT_UNAVAILABLE";
    else if (mqttState == 4) stateStr = "CONNECT_BAD_CREDENTIALS";
    else if (mqttState == 5) stateStr = "CONNECT_UNAUTHORIZED";
    
    STEPF("[MQTT] Échec de connexion - état: %d (%s), mémoire libre: %u bytes\n", 
                  mqttState, stateStr, ESP.getFreeHeap());
    STEPF("[MQTT] Vérifier: WiFi connecté ? (%d), serveur: %s:%d\n",
                  WiFi.status() == WL_CONNECTED, mqtt_server, mqtt_port);
  } else {
    STEPF("[MQTT] Connecté avec succès - mémoire libre: %u bytes\n", ESP.getFreeHeap());
    STEPF("[MQTT] Pour recevoir les messages: abonnez-vous à 'jimny/#' sur %s:%d\n", mqtt_server, mqtt_port);
  }
  
  return connected;
}

// Configure NTP si pas déjà fait
static void configureNTP() {
  if (ntpConfigured) return;
  
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    ntpConfigured = true;
    STEPLN("[NTP] Configuration NTP effectuée");
  }
}

// Obtient un datetime lisible (format: YYYY-MM-DD HH:MM:SS)
static String getReadableDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Si NTP n'a pas encore synchronisé, retourner un timestamp relatif
    uint32_t secs = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "UPTIME: %02lu:%02lu:%02lu", 
             secs / 3600, (secs % 3600) / 60, secs % 60);
    return String(buf);
  }
  
  // Format lisible: YYYY-MM-DD HH:MM:SS
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// Publie un message de statut avec horodatage et informations de debug
static void mqttPublishStatus(const char* message) {
  if (!mqtt.connected()) {
    if (!mqttEnsureConnected()) {
      STEPF("[STATUS] MQTT non connecté, message non publié: %s\n", message);
      return;
    }
  }
  
  // Configurer NTP si nécessaire
  configureNTP();
  
  // Récupérer les états actuels pour le debug
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = mqtt.connected();
  String wifiStatus = wifiOk ? "OK" : "KO";
  String mqttStatus = mqttOk ? "OK" : "KO";
  String bleStatus = bleConnected ? "OK" : "KO";
  String wifiSSID = wifiOk ? WiFi.SSID() : "N/A";
  String wifiIP = wifiOk ? WiFi.localIP().toString() : "N/A";
  int wifiRSSI = wifiOk ? WiFi.RSSI() : 0;
  
  // Créer un JSON avec le message, l'horodatage et les infos de debug
  StaticJsonDocument<512> doc;
  doc["status"] = message;
  doc["datetime"] = getReadableDateTime();
  doc["ts_ms"] = (uint32_t)millis();
  doc["ts_s"] = (uint32_t)(millis() / 1000);
  
  // Informations de debug
  doc["debug"]["wifi"] = wifiStatus;
  doc["debug"]["mqtt"] = mqttStatus;
  doc["debug"]["ble"] = bleStatus;
  if (wifiOk) {
    doc["debug"]["wifi_ssid"] = wifiSSID;
    doc["debug"]["wifi_ip"] = wifiIP;
    doc["debug"]["wifi_rssi"] = wifiRSSI;
  }
  doc["debug"]["fifo_count"] = fifoCount;
  doc["debug"]["fifo_max"] = FIFO_BUFFER_SIZE;
  doc["debug"]["state"] = currentState; // État FSM (0=BOOT, 1=CONNECT_WIFI, etc.)
  
  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  
  if (mqtt.publish(topic_status, (uint8_t*)payload, n, false)) {
    STEPF("[STATUS] Publié: %s (%s) - WiFi:%s MQTT:%s BLE:%s\n", 
                  message, getReadableDateTime().c_str(), 
                  wifiStatus.c_str(), mqttStatus.c_str(), bleStatus.c_str());
    ledNoteMqttActivity();
    for (int i = 0; i < 5; i++) { mqtt.loop(); delay(2); }
  } else {
    STEPF("[STATUS] Échec publication: %s\n", message);
  }
}

// Structure pour trier les réseaux WiFi par RSSI
struct WifiNetwork {
  String ssid;
  int rssi;
  wifi_auth_mode_t encryption;
  bool isOpen;
};

// Scanner et lister les 5 meilleurs réseaux WiFi (par RSSI)
static void wifiScanAndListTop5() {
  STEPLN("[WiFi] Scan des réseaux disponibles...");
  int n = WiFi.scanNetworks();
  
  if (n == 0) {
    STEPLN("[WiFi] Aucun réseau trouvé");
    return;
  }
  
  // Créer un tableau de réseaux
  WifiNetwork networks[n];
  for (int i = 0; i < n; i++) {
    networks[i].ssid = WiFi.SSID(i);
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].encryption = WiFi.encryptionType(i);
    networks[i].isOpen = (networks[i].encryption == WIFI_AUTH_OPEN);
  }
  
  // Trier par RSSI (décroissant)
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (networks[j].rssi > networks[i].rssi) {
        WifiNetwork temp = networks[i];
        networks[i] = networks[j];
        networks[j] = temp;
      }
    }
  }
  
  // Afficher les 5 meilleurs
  int topCount = (n < 5) ? n : 5;
  STEPF("[WiFi] Top %d réseau(x) (par RSSI):\n", topCount);
  for (int i = 0; i < topCount; i++) {
    const char* encStr = networks[i].isOpen ? "OUVERT" : "protégé";
    STEPF("  %d. %s (RSSI: %d dBm, %s)\n", 
                  i + 1,
                  networks[i].ssid.c_str(), 
                  networks[i].rssi,
                  encStr);
    
    // Ajouter automatiquement les réseaux ouverts au WiFiMulti
    // Note: Les portails captifs seront détectés après connexion
    // Ne pas ajouter les SSIDs blacklistés
    if (networks[i].isOpen && networks[i].ssid.length() > 0) {
      if (!wifiIsBlacklisted(networks[i].ssid)) {
        wifiMulti.addAP(networks[i].ssid.c_str(), "");  // Password vide pour réseau ouvert
        STEPF("    → Ajouté au WiFiMulti (ouvert, portail captif possible)\n");
      } else {
        STEPF("    → Ignoré (blacklisté): %s\n", networks[i].ssid.c_str());
      }
    }
  }
}

// Détecte si le réseau WiFi actuel a un portail captif
// Retourne true si portail captif détecté, false sinon
static bool wifiDetectCaptivePortal() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  HTTPClient http;
  http.setTimeout(3000); // Timeout de 3 secondes
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); // Ne pas suivre les redirections automatiquement
  
  // URLs de test pour détecter les portails captifs
  // Les portails captifs redirigent généralement ces URLs vers leur page d'authentification
  const char* testUrls[] = {
    "http://www.google.com/generate_204",  // URL standard Android/iOS
    "http://captive.apple.com/hotspot-detect.html",  // URL Apple
    "http://connectivitycheck.gstatic.com/generate_204",  // URL Google
    "http://www.msftconnecttest.com/connecttest.txt"  // URL Microsoft
  };
  
  for (int i = 0; i < 4; i++) {
    http.begin(testUrls[i]);
    int httpCode = http.GET();
    
    // Si on reçoit une redirection (302, 301) ou une page HTML (200 avec contenu HTML), c'est probablement un portail captif
    // Utiliser les valeurs numériques directement (ESP32 HTTPClient n'a pas toutes les constantes)
    if (httpCode == 302 ||  // HTTP_CODE_MOVED_TEMPORARILY
        httpCode == 301 ||  // HTTP_CODE_MOVED_PERMANENTLY
        (httpCode == 200 && http.getString().indexOf("captive") >= 0)) {  // HTTP_CODE_OK avec "captive" dans le contenu
      http.end();
      return true; // Portail captif détecté
    }
    
    // Si on reçoit un 204 (No Content), c'est bon signe : pas de portail captif
    if (httpCode == 204) {  // HTTP_CODE_NO_CONTENT
      http.end();
      return false; // Pas de portail captif
    }
    
    http.end();
    szDelay(100); // Petit délai entre les tentatives
  }
  
  // Si toutes les URLs échouent ou timeout, on considère qu'il y a peut-être un portail captif
  // (mais on ne peut pas être sûr, donc on retourne false pour ne pas bloquer)
  return false;
}

// Liste des SSIDs à éviter temporairement (portails captifs non contournables)
#define MAX_BLACKLISTED_SSIDS 10
static String blacklistedSSIDs[MAX_BLACKLISTED_SSIDS];
static int blacklistedCount = 0;
static unsigned long blacklistTimestamps[MAX_BLACKLISTED_SSIDS];

// Ajoute un SSID à la blacklist temporaire (évite de boucler sur le même réseau)
static void wifiBlacklistSSID(const String& ssid) {
  // Vérifier si déjà dans la blacklist
  for (int i = 0; i < blacklistedCount; i++) {
    if (blacklistedSSIDs[i] == ssid) {
      // Réinitialiser le timestamp
      blacklistTimestamps[i] = millis();
      return;
    }
  }
  
  // Ajouter si pas plein
  if (blacklistedCount < MAX_BLACKLISTED_SSIDS) {
    blacklistedSSIDs[blacklistedCount] = ssid;
    blacklistTimestamps[blacklistedCount] = millis();
    blacklistedCount++;
    STEPF("[WiFi] SSID ajouté à la blacklist: %s (pour 5 minutes)\n", ssid.c_str());
  } else {
    // Remplacer le plus ancien
    int oldestIdx = 0;
    for (int i = 1; i < MAX_BLACKLISTED_SSIDS; i++) {
      if (blacklistTimestamps[i] < blacklistTimestamps[oldestIdx]) {
        oldestIdx = i;
      }
    }
    blacklistedSSIDs[oldestIdx] = ssid;
    blacklistTimestamps[oldestIdx] = millis();
    STEPF("[WiFi] SSID remplacé dans la blacklist: %s\n", ssid.c_str());
  }
}

// Vérifie si un SSID est dans la blacklist (et si le timeout de 5 minutes est expiré)
static bool wifiIsBlacklisted(const String& ssid) {
  for (int i = 0; i < blacklistedCount; i++) {
    if (blacklistedSSIDs[i] == ssid) {
      // Vérifier si le timeout de 5 minutes est expiré
      if (millis() - blacklistTimestamps[i] < 300000) { // 5 minutes
        return true;
      } else {
        // Timeout expiré, retirer de la blacklist
        STEPF("[WiFi] SSID retiré de la blacklist (timeout): %s\n", ssid.c_str());
        for (int j = i; j < blacklistedCount - 1; j++) {
          blacklistedSSIDs[j] = blacklistedSSIDs[j + 1];
          blacklistTimestamps[j] = blacklistTimestamps[j + 1];
        }
        blacklistedCount--;
        return false;
      }
    }
  }
  return false;
}

// Tente de contourner un portail captif simple (si possible)
// Retourne true si le portail a été contourné, false sinon
static bool wifiBypassCaptivePortal() {
  // Pour l'instant, on ne fait que détecter
  // Une implémentation future pourrait :
  // - Envoyer une requête POST avec des credentials par défaut
  // - Utiliser un User-Agent spécifique
  // - Essayer des endpoints connus de portails captifs
  
  STEPLN("[WiFi] Portail captif détecté - contournement automatique non implémenté");
  STEPLN("[WiFi] Le réseau nécessite une authentification manuelle via navigateur");
  return false; // Contournement impossible
}

static void wifiTick() {
  static bool lastWifiStatus = false;
  static bool scanDone = false;
  static unsigned long lastScanMs = 0;
  static bool captivePortalChecked = false;
  static unsigned long lastCaptiveCheckMs = 0;
  
  bool currentWifiStatus = (WiFi.status() == WL_CONNECTED);
  
  // Scanner les réseaux une seule fois au démarrage (dans les 5 premières secondes)
  if (!scanDone && millis() < 5000 && millis() - lastScanMs > 2000) {
    wifiScanAndListTop5();
    scanDone = true;
    lastScanMs = millis();
  }
  
  // Afficher la connexion WiFi quand elle change
  if (currentWifiStatus != lastWifiStatus) {
    if (currentWifiStatus) {
      STEPF("[WiFi] Connecté à: %s (IP: %s, RSSI: %d dBm)\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      // Configurer NTP dès que WiFi est connecté
      configureNTP();
      captivePortalChecked = false; // Réinitialiser le flag pour le nouveau réseau
      lastCaptiveCheckMs = millis();
    } else {
      STEPLN("[WiFi] Déconnecté");
      ntpConfigured = false; // Réinitialiser pour la prochaine connexion
      captivePortalChecked = false;
    }
    lastWifiStatus = currentWifiStatus;
  }
  
  // Vérifier le portail captif après connexion (une seule fois, après 2 secondes)
  if (currentWifiStatus && !captivePortalChecked && millis() - lastCaptiveCheckMs > 2000) {
    STEPLN("[WiFi] Vérification portail captif...");
    if (wifiDetectCaptivePortal()) {
      STEPLN("[WiFi] ⚠️  PORTAL CAPTIF DÉTECTÉ");
      STEPLN("[WiFi] Le réseau nécessite une authentification via navigateur web");
      
      // Tenter un contournement
      if (!wifiBypassCaptivePortal()) {
        // Contournement impossible → déconnecter et chercher un autre réseau
        String currentSSID = WiFi.SSID();
        STEPLN("[WiFi] Contournement impossible → déconnexion et recherche d'un autre réseau");
        
        // Ajouter à la blacklist temporaire (5 minutes)
        wifiBlacklistSSID(currentSSID);
        
        // Déconnecter
        WiFi.disconnect();
        szDelay(500);
        
        // Réinitialiser le flag pour permettre une nouvelle vérification
        captivePortalChecked = false;
        lastWifiStatus = false; // Forcer la détection de changement
        
        // Forcer une nouvelle recherche
        STEPLN("[WiFi] Recherche d'un autre réseau WiFi...");
        return; // Sortir de la fonction pour permettre à wifiMulti.run() de chercher un autre réseau
      } else {
        STEPLN("[WiFi] ✓ Portail captif contourné avec succès");
      }
    } else {
      STEPLN("[WiFi] ✓ Pas de portail captif détecté");
    }
    captivePortalChecked = true;
  }
  
  // Vérifier les SSIDs blacklistés avant de se connecter
  // (cette vérification se fait dans wifiScanAndListTop5, mais on peut aussi vérifier ici)
  
  if (wifiMulti.run() == WL_CONNECTED) return;
}

// --- Contrôle LEDs simples (ON/OFF) ---
static void ledInit() {
  pinMode(LED_WIFI_PIN, OUTPUT);
  ledWifiWrite(false);
  // NOTE: LED_BLE_PIN n'est pas utilisée (pin 34 est INPUT ONLY)
  // On utilise uniquement LED_WIFI_PIN pour les deux fonctions
}

// LED WiFi (orange): pattern selon l'état WiFi/MQTT
static void ledWifiUpdate(SystemState state, bool wifiOk, bool mqttOk) {
  uint32_t now = millis();
  LedPattern p = getWifiPattern(state, wifiOk, mqttOk);
  bool shouldBeOn = false;

  // Initialiser le timer si nécessaire
  if (p.period_ms > 0 && ledWifiLastToggle == 0) {
    ledWifiLastToggle = now;
  }
  
  if (p.period_ms == 0) {
    // Fixe
    shouldBeOn = (p.duty_cycle > 0);
  } else {
    // Clignotant avec duty cycle
    uint32_t elapsed = (now >= ledWifiLastToggle) ? (now - ledWifiLastToggle) : 0;
    uint32_t cyclePos = elapsed % p.period_ms;
    uint32_t onTime = (p.period_ms * p.duty_cycle) / 100;
    shouldBeOn = (cyclePos < onTime);
    
    // Réinitialiser le cycle si nécessaire
    if (elapsed >= p.period_ms) {
      ledWifiLastToggle = now;
    }
  }
  
  if (shouldBeOn != ledWifiState) {
    ledWifiState = shouldBeOn;
    ledWifiWrite(ledWifiState);
  }
}

// LED BLE (rouge): pattern selon l'état BLE/OBD
// NOTE: XIAO ESP32S3 SENSE n'a qu'une seule LED (orange), donc cette fonction
// ne contrôle plus de pin séparé. Les patterns BLE sont combinés avec WiFi
// dans ledEnsureAtLeastOneOn().
static void ledBleUpdate(SystemState state, bool bleOk) {
  // Ne rien faire ici - la LED BLE est gérée via ledEnsureAtLeastOneOn()
  // On garde juste la fonction pour la compatibilité du code
  (void)state;
  (void)bleOk;
}

// Vérifie qu'au moins une LED est allumée (garantie visuelle que l'ESP est sous tension)
// Clignotement = Morse méthode Koch : 1er train = K (-.-), 2e = M (--), 3e = R (.-.), etc.
static void ledEnsureAtLeastOneOn(SystemState state, bool wifiOk, bool mqttOk, bool bleOk) {
  uint32_t now = millis();
  (void)state;
  (void)wifiOk;
  (void)mqttOk;
  (void)bleOk;

  bool baseShouldBeOn = ledMorseKochShouldBeOn(now);

  // Overlay d'activité
  bool shouldBeOn = baseShouldBeOn;
  const uint32_t BLE_BURST_MS = 100;
  const uint32_t MQTT_PULSE_MS = 80;
  if (ledBleActivityMs != 0) {
    uint32_t ageBle = now - (uint32_t)ledBleActivityMs;
    if (ageBle < BLE_BURST_MS) {
      shouldBeOn = ((ageBle % 24) < 20);  // 20 ms ON / 4 ms OFF
    }
  }
  if (shouldBeOn == baseShouldBeOn && ledMqttActivityMs != 0) {
    uint32_t ageMqtt = now - (uint32_t)ledMqttActivityMs;
    if (ageMqtt < MQTT_PULSE_MS && ageMqtt < 60) shouldBeOn = true;
  }

  // Failsafe: si la LED est restée OFF > 800 ms, forcer ON (évite trous d'1 min)
  static uint32_t lastLedOffMs = 0;
  if (shouldBeOn) lastLedOffMs = 0;
  if (lastLedOffMs != 0 && (now - lastLedOffMs) > 800) {
    shouldBeOn = true;
    lastLedOffMs = 0;
  }

  if (shouldBeOn != ledWifiState) {
    if (!shouldBeOn) lastLedOffMs = now;  // mémoriser instant du passage à OFF
    ledWifiState = shouldBeOn;
    ledWifiWrite(ledWifiState);
  }
}

// Tick LED utilisable depuis des boucles/délais bloquants
static inline void ledTickNow() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = mqtt.connected();
  ledEnsureAtLeastOneOn(currentState, wifiOk, mqttOk, bleConnected);
}

// Delay "non bloquant visuellement": maintient LED + MQTT pendant les waits
static void szDelay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    if (WiFi.status() == WL_CONNECTED) mqtt.loop();
    ledTickNow();
    uint32_t slice = (ms < 25) ? 1 : 5;
    ::delay(slice);
  }
}

static bool bleFindUartLikeChars(BLEClient* client) {
  chrWrite = nullptr;
  chrNotify = nullptr;

  std::map<std::string, BLERemoteService*>* services = client->getServices();
  if (!services) {
    STEPLN("[BLE] Aucun service trouvé");
    return false;
  }

  STEPF("[BLE] %d service(s) trouvé(s)\n", services->size());
  int serviceIdx = 0;
  for (auto const& it : *services) {
    BLERemoteService* svc = it.second;
    if (!svc) continue;
    auto* chrs = svc->getCharacteristics();
    if (!chrs) continue;

    STEPF("[BLE] Service %d: %d caractéristique(s)\n", serviceIdx++, chrs->size());
    int charIdx = 0;
    for (auto const& cit : *chrs) {
      BLERemoteCharacteristic* ch = cit.second;
      if (!ch) continue;

      // Heuristique:
      // - un char "write" (WRITE ou WRITE_NO_RSP)
      // - un char "notify" (NOTIFY) pour récupérer les réponses ELM ('>' etc.)
      if (!chrWrite && (ch->canWrite() || ch->canWriteNoResponse())) {
        chrWrite = ch;
        STEPF("[BLE] Caractéristique write trouvée (idx %d)\n", charIdx);
      }
      if (!chrNotify && ch->canNotify()) {
        chrNotify = ch;
        STEPF("[BLE] Caractéristique notify trouvée (idx %d)\n", charIdx);
      }

      if (chrWrite && chrNotify) break;
      charIdx++;
    }
    if (chrWrite && chrNotify) break;
  }

  if (!chrWrite) {
    STEPLN("[BLE] ERREUR: Aucune caractéristique write trouvée");
    return false;
  }
  if (!chrNotify) {
    STEPLN("[BLE] ERREUR: Aucune caractéristique notify trouvée");
    return false;
  }
  
  STEPLN("[BLE] Enregistrement callback notify...");
  chrNotify->registerForNotify(notifyCallback);
  STEPLN("[BLE] Callback notify enregistré");
  szDelay(100); // Délai pour que les notifications soient activées
  return true;
}

// Structure pour trier les devices BLE par RSSI
struct BleDevice {
  String name;
  String address;
  int rssi;
  bool isVlinker;
};

// Scanner et lister les 5 meilleurs devices BLE (par RSSI)
static void bleScanAndListTop5() {
  if (!BLEDevice::getInitialized()) {
    STEPLN("[BLE] BLE non initialisé");
    return;
  }

  STEPLN("[BLE] Scan des devices BLE...");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  BLEScanResults results = scan->start(3, false);  // Scan de 3 secondes
  int deviceCount = results.getCount();
  STEPF("[BLE] Scan terminé: %d device(s) trouvé(s)\n", deviceCount);
  
  if (deviceCount == 0) {
    scan->clearResults();
    return;
  }
  
  // Créer un tableau de devices
  BleDevice devices[deviceCount];
  for (int i = 0; i < deviceCount; i++) {
    BLEAdvertisedDevice device = results.getDevice(i);
    devices[i].name = String(device.getName().c_str());
    devices[i].address = String(device.getAddress().toString().c_str());
    devices[i].rssi = device.getRSSI();
    String nameUpper = devices[i].name;
    nameUpper.toUpperCase();
    String pattern = String(vLinker_NamePattern);
    pattern.toUpperCase();
    devices[i].isVlinker = (nameUpper.indexOf(pattern) >= 0);
  }
  
  // Trier par RSSI (décroissant)
  for (int i = 0; i < deviceCount - 1; i++) {
    for (int j = i + 1; j < deviceCount; j++) {
      if (devices[j].rssi > devices[i].rssi) {
        BleDevice temp = devices[i];
        devices[i] = devices[j];
        devices[j] = temp;
      }
    }
  }
  
  // Afficher les 5 meilleurs
  int topCount = (deviceCount < 5) ? deviceCount : 5;
  STEPF("[BLE] Top %d device(s) (par RSSI):\n", topCount);
  for (int i = 0; i < topCount; i++) {
    const char* vlinkerStr = devices[i].isVlinker ? " [vLinker]" : "";
    const char* nameStr = devices[i].name.length() > 0 ? devices[i].name.c_str() : "(sans nom)";
    STEPF("  %d. %s (%s, RSSI: %d dBm)%s\n", 
                  i + 1,
                  nameStr,
                  devices[i].address.c_str(),
                  devices[i].rssi,
                  vlinkerStr);
  }
  
  // DEBUG: Afficher TOUS les devices avec leurs noms complets
  STEPLN("[BLE] DEBUG - Liste complète des devices:");
  for (int i = 0; i < deviceCount; i++) {
    BLEAdvertisedDevice device = results.getDevice(i);
    String name = String(device.getName().c_str());
    String addr = String(device.getAddress().toString().c_str());
    STEPF("  [%d] Nom: '%s' (len=%d), Addr: %s, RSSI: %d\n", 
                  i, name.c_str(), name.length(), addr.c_str(), device.getRSSI());
  }
  
  scan->clearResults();
}

// Scan BLE pour trouver un device dont le nom contient "vLinker"
static bool bleScanForVlinker(uint32_t scanTimeMs = 5000) {
  // S'assurer qu'on n'est pas connecté avant de scanner
  if (bleClient) {
    if (bleClient->isConnected()) {
      STEPLN("[BLE] Déconnexion avant scan...");
      bleClient->disconnect();
    }
    // Détruire le client pour éviter les conflits
    delete bleClient;
    bleClient = nullptr;
    szDelay(300);
  }

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  STEPLN("[BLE] Démarrage scan...");
  BLEScanResults results = scan->start(scanTimeMs / 1000, false);
  STEPF("[BLE] Scan terminé: %d devices trouvés\n", results.getCount());
  
  // Attendre un peu que le scan soit complètement terminé
  szDelay(200);
  
  STEPLN("[BLE] DEBUG - Recherche vLinker dans les devices scannés...");
  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice device = results.getDevice(i);
    String name = String(device.getName().c_str());
    String addr = String(device.getAddress().toString().c_str());
    String nameUpper = name;
    nameUpper.toUpperCase();
    String pattern = String(vLinker_NamePattern);
    pattern.toUpperCase();
    
    STEPF("[BLE] DEBUG - Device %d: Nom='%s' (len=%d), Addr=%s\n", 
                  i, name.c_str(), name.length(), addr.c_str());
    
    if (nameUpper.indexOf(pattern) >= 0) {
      STEPF("[BLE] DEBUG - MATCH trouvé ! Nom contient '%s'\n", vLinker_NamePattern);
      vLinkerAddress = device.getAddress();
      // Créer une copie du device pour la connexion
      if (vLinkerDevice) {
        delete vLinkerDevice;
      }
      vLinkerDevice = new BLEAdvertisedDevice(device);
      vLinkerAddressValid = true;
      STEPF("[BLE] Trouvé vLinker: %s (%s, RSSI: %d dBm)\n", 
                    device.getName().c_str(), 
                    vLinkerAddress.toString().c_str(),
                    device.getRSSI());
      scan->clearResults(); // Nettoyer les résultats
      szDelay(300); // Délai avant de tenter la connexion
      return true;
    }
  }
  STEPLN("[BLE] DEBUG - Aucun device ne correspond au pattern 'vLinker'");
  STEPF("[BLE] DEBUG - Pattern recherché: '%s' (case insensitive)\n", vLinker_NamePattern);
  vLinkerAddressValid = false;
  scan->clearResults(); // Nettoyer les résultats
  return false;
}

static bool bleConnectVlinker() {
  rxBuf = "";
  gotNotify = false;

  // Si on n'a pas encore l'adresse, on scanne
  if (!vLinkerAddressValid) {
    if (!bleScanForVlinker(5000)) {
      STEPLN("[BLE] vLinker non trouvé dans le scan");
      return false;
    }
  }

  // Toujours créer un nouveau client pour éviter les problèmes de réutilisation
  if (bleClient) {
    if (bleClient->isConnected()) {
      STEPLN("[BLE] Déconnexion du client existant...");
      bleClient->disconnect();
      szDelay(200);
    }
    // Détruire l'ancien client
    delete bleClient;
    bleClient = nullptr;
    szDelay(200);
  }

  // Créer un nouveau client
  STEPLN("[BLE] Création nouveau client BLE...");
  bleClient = BLEDevice::createClient();
  if (!bleClient) {
    STEPLN("[BLE] ERREUR: Impossible de créer le client BLE");
    return false;
  }

  // Connexion au device trouvé (utiliser l'objet device complet si disponible)
  STEPF("[BLE] Tentative de connexion à %s...\n", vLinkerAddress.toString().c_str());
  szDelay(200); // Délai avant la connexion
  
  bool connected = false;
  if (vLinkerDevice) {
    // Utiliser l'objet device complet (meilleure méthode)
    STEPLN("[BLE] Connexion via device object...");
    connected = bleClient->connect(vLinkerDevice);
  } else {
    // Fallback: utiliser l'adresse
    STEPLN("[BLE] Connexion via adresse MAC...");
    connected = bleClient->connect(vLinkerAddress);
  }
  
  if (!connected) {
    STEPLN("[BLE] Échec de connexion");
    STEPLN("[BLE] Vérifier que le vLinker n'est pas connecté à un autre appareil");
    // Nettoyer le client
    if (bleClient) {
      delete bleClient;
      bleClient = nullptr;
    }
    // Nettoyer le device
    if (vLinkerDevice) {
      delete vLinkerDevice;
      vLinkerDevice = nullptr;
    }
    vLinkerAddressValid = false; // Réessayer le scan au prochain appel
    szDelay(500);
    return false;
  }

  STEPLN("[BLE] Connecté ! Attente stabilisation...");
  szDelay(500); // Délai plus long pour que la connexion se stabilise

  STEPLN("[BLE] Recherche des caractéristiques UART...");
  if (!bleFindUartLikeChars(bleClient)) {
    STEPLN("[BLE] Caractéristiques UART non trouvées");
    bleClient->disconnect();
    szDelay(200);
    delete bleClient;
    bleClient = nullptr;
    if (vLinkerDevice) {
      delete vLinkerDevice;
      vLinkerDevice = nullptr;
    }
    vLinkerAddressValid = false;
    return false;
  }

  STEPLN("[BLE] Caractéristiques UART trouvées (write+notify)");
  return true;
}

// --- Helpers ELM style ---
static void elmClearRx() {
  rxBuf = "";
  gotNotify = false;
}

static bool elmWriteLine(const String& line) {
  if (!bleClient || !bleClient->isConnected() || !chrWrite) {
    STEPLN("[ELM] ERREUR: Client BLE non connecté ou caractéristique write absente");
    return false;
  }
  String cmd = line;
  if (!cmd.endsWith("\r")) cmd += "\r";
  STEPF("[ELM] TX: %s", cmd.c_str());
  ledNoteBleActivity();
  chrWrite->writeValue((uint8_t*)cmd.c_str(), cmd.length(), false);
  szDelay(10); // Petit délai après l'envoi
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
      STEPF("[ELM] RX: %s>\n", out.c_str());
      return true;
    }
    // Afficher ce qui est reçu même sans prompt (pour debug)
    if (rxBuf.length() > 0 && (millis() - start) % 200 < 10) {
      STEPF("[ELM] RX (partiel, %d ms): %s\n", (int)(millis() - start), rxBuf.c_str());
    }
    ledTickNow();
    ::delay(5);
  }
  out = rxBuf;
  if (out.length() > 0) {
    STEPF("[ELM] RX (timeout, %d ms): %s\n", timeoutMs, out.c_str());
  } else {
    STEPF("[ELM] RX (timeout, %d ms): Aucune réponse\n", timeoutMs);
  }
  return false;
}

static String elmQuery(const String& line, uint32_t timeoutMs = 1500) {
  elmClearRx();
  if (!elmWriteLine(line)) {
    STEPLN("[ELM] ERREUR: Impossible d'envoyer la commande");
    return "";
  }
  String resp;
  bool gotResponse = elmReadUntilPrompt(resp, timeoutMs);
  resp.replace("\r", "");
  resp.replace("\n", "");
  resp.trim();
  if (!gotResponse) {
    STEPF("[ELM] ATTENTION: Pas de réponse complète pour '%s'\n", line.c_str());
  }
  return resp;
}

// Extrait la vitesse (km/h) depuis une réponse OBD-II standard PID 010D.
// Recherche la séquence "410D" dans la réponse (robuste aux espaces / entêtes / formatage ELM).
static bool parseObdPid010D_SpeedKmh(const String& resp, float& outSpeedKmh) {
  String s = resp;
  s.toUpperCase();

  // Garder uniquement [0-9A-F] pour faciliter la recherche.
  String hex;
  hex.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    bool isHex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    if (isHex) hex += c;
  }

  int idx = hex.indexOf("410D");
  if (idx < 0) return false;
  if (idx + 6 > (int)hex.length()) return false;

  String byteHex = hex.substring(idx + 4, idx + 6);
  int v = (int)strtol(byteHex.c_str(), nullptr, 16);
  if (v < 0 || v > 255) return false;
  outSpeedKmh = (float)v;
  return true;
}

// --- JSON publish ---
static void publishSzJson(const SzData& d, const String& rawA0, const String& rawA2, const String& rawA5, const String& rawCD) {
  // Appeler loop() pour maintenir la connexion MQTT
  mqtt.loop();
  
  if (!mqttEnsureConnected()) {
    STEPLN("[PUB] ERREUR: MQTT non connecté");
    return;
  }

  // datetime lisible (si NTP OK) ou "UPTIME: ..." sinon
  configureNTP();

  StaticJsonDocument<1280> doc;
  doc["app"] = app_name;
  doc["ver"] = version;
  doc["ts_ms"] = (uint32_t)millis();
  doc["datetime"] = getReadableDateTime();
#if USE_OBD2_PROTOCOL
  doc["protocol"] = "obd2";
#else
  doc["protocol"] = "sz";
#endif

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

  char payload[1400];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  
  // Vérifier l'état de la connexion avant publication
  if (!mqtt.connected()) {
    STEPF("[PUB] ERREUR: MQTT déconnecté avant publication\n");
    if (!mqttEnsureConnected()) {
      STEPLN("[PUB] ERREUR: Impossible de reconnecter MQTT");
      return;
    }
  }
  
  // Vérifier la mémoire avant publication
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 10000) {
    STEPF("[PUB] ATTENTION: Mémoire libre très faible (%u bytes) avant publication\n", freeHeap);
  }
  
  // Utiliser la signature avec uint8_t* et longueur (plus fiable)
  bool pubOk = mqtt.publish(topic_sz, (uint8_t*)payload, n, false); // QoS 0
  if (pubOk) {
    STEPF("[PUB] OK: %s (%d bytes) - mémoire libre: %u bytes\n", topic_sz, n, ESP.getFreeHeap());
    ledNoteMqttActivity();
    // Flush: plusieurs loop() + court délai pour que le paquet TCP soit bien envoyé
    for (int i = 0; i < 10; i++) {
      mqtt.loop();
      delay(2);
    }
  } else {
    STEPF("[PUB] FAIL: %s (%d bytes) - connected=%d, state=%d, mémoire=%u bytes\n", 
                  topic_sz, n, mqtt.connected(), mqtt.state(), ESP.getFreeHeap());
    // Tenter de reconnecter si déconnecté
    if (!mqtt.connected()) {
      STEPLN("[PUB] MQTT déconnecté, tentative de reconnexion...");
      mqttEnsureConnected();
    }
  }

  // En option: publier les raw seuls (topic dédié)
  if (rawA0.length() || rawA2.length() || rawA5.length() || rawCD.length()) {
    StaticJsonDocument<768> dbg;
    dbg["ts_ms"] = (uint32_t)millis();
    dbg["datetime"] = getReadableDateTime();
    dbg["21A0"] = rawA0;
    dbg["21A2"] = rawA2;
    dbg["21A5"] = rawA5;
    dbg["21CD"] = rawCD;
    char p2[768];
    size_t n2 = serializeJson(dbg, p2, sizeof(p2));
    if (mqtt.publish(topic_debug, (uint8_t*)p2, n2)) {
      ledNoteMqttActivity();
      for (int i = 0; i < 5; i++) { mqtt.loop(); delay(2); }
    }
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

  // ATFI (Fast Init) - peut échouer plusieurs fois, réessayer jusqu'à 6 fois
  // Dans le log, SZ Viewer réessaie jusqu'à 6 fois en refaisant la séquence de config
  const int maxRetries = 6;
  bool atfiOk = false;
  
  // Fonction pour refaire la séquence de configuration
  auto doConfigSequence = []() {
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
  };
  
  for (int i = 0; i < maxRetries; i++) {
    String fi = elmQuery("ATFI", 2500);
    // Vérifier si la réponse contient "OK" (pas "ERROR")
    // La réponse peut être "BUS INIT: OK" ou juste "OK"
    if (fi.indexOf("OK") >= 0 && fi.indexOf("ERROR") < 0) {
      STEPF("[ELM] ATFI réussi après %d tentative(s)\n", i + 1);
      atfiOk = true;
      break;
    }
    STEPF("[ELM] ATFI échoué (tentative %d/%d): %s\n", i + 1, maxRetries, fi.c_str());
    if (i < maxRetries - 1) {
      szDelay(500); // Petit délai avant de réessayer
      doConfigSequence(); // Refaire la séquence de configuration
    }
  }
  
  if (!atfiOk) {
    STEPLN("[ELM] ERREUR: ATFI a échoué après toutes les tentatives");
    STEPLN("[ELM] Vérifiez que le contact est allumé et que le vLinker est bien connecté");
    return false;
  }
  
  // ATKW (optionnel; dans le log: "1:D0 2:8F")
  elmQuery("ATKW");
  return true;
}

#if USE_OBD2_PROTOCOL
// Init ELM pour OBD2 (profil Suzuki OBD-II / EOBD, comme Car Scanner). Pas d'ATSH fixe, pas d'ATFI.
static bool elmInitObd2() {
  elmQuery("ATZ", 2000);
  elmQuery("ATE0");
  elmQuery("ATD0");
  elmQuery("ATH1");
  elmQuery("ATSP0");   // auto protocol
  elmQuery("ATST64");  // timeout
  elmQuery("ATM0");
  elmQuery("ATS0");
  elmQuery("ATAT1");
  elmQuery("ATAL");
  String r0100 = elmQuery("0100", 2000);
  if (r0100.indexOf("NO DATA") >= 0 || r0100.indexOf("UNABLE") >= 0) {
    STEPLN("[ELM] OBD2 init: 0100 sans réponse, véhicule peut ne pas supporter mode 01");
  } else {
    STEPF("[ELM] OBD2 init OK (0100 réponse reçue)\n");
  }
  return true;
}

// Collecte OBD2: envoie PIDs 0105, 010B, 010C, 010D, 0110, 0111, 0123 + ATRV, décode vers SzData.
static bool pollObd2AndCollect() {
  String r0105 = elmQuery("0105", 800);
  String r010B = elmQuery("010B", 800);
  String r010C = elmQuery("010C", 800);
  String r010D = elmQuery("010D", 800);
  String r0110 = elmQuery("0110", 800);
  String r0111 = elmQuery("0111", 800);
  String r0123 = elmQuery("0123", 800);
  String atrv = elmQuery("ATRV", 400);

  if (r010C.indexOf("NO DATA") >= 0 && r010D.indexOf("NO DATA") >= 0) {
    STEPLN("[ELM] OBD2 NO DATA → contact coupé ?");
    return false;
  }

  SzData d;
  decodeObd2ToSzData(
    nullptr, r0105.c_str(), r010B.c_str(),
    r010C.c_str(), r010D.c_str(), r0110.c_str(), r0111.c_str(),
    r0123.c_str(), atrv.c_str(),
    d
  );

  static SzData lastSz;
  #define OBD2_MERGE(field) do { \
    if (isnan(d.field)) { if (!isnan(lastSz.field)) d.field = lastSz.field; } \
    else { lastSz.field = d.field; } \
  } while(0)
  OBD2_MERGE(desired_idle_speed_rpm);
  OBD2_MERGE(accelerator_pct);
  OBD2_MERGE(intake_c);
  OBD2_MERGE(battery_v);
  OBD2_MERGE(fuel_temp_c);
  OBD2_MERGE(bar_pressure_kpa);
  OBD2_MERGE(bar_pressure_mmhg);
  OBD2_MERGE(abs_pressure_mbar);
  OBD2_MERGE(air_flow_estimate_mgcp);
  OBD2_MERGE(air_flow_request_mgcp);
  OBD2_MERGE(speed_kmh);
  OBD2_MERGE(rail_pressure_bar);
  OBD2_MERGE(rail_pressure_control_bar);
  OBD2_MERGE(desired_egr_position_pct);
  OBD2_MERGE(gear_ratio);
  OBD2_MERGE(egr_position_pct);
  OBD2_MERGE(engine_temp_c);
  OBD2_MERGE(air_temp_c);
  OBD2_MERGE(requested_in_pressure_mbar);
  OBD2_MERGE(engine_rpm);
  #undef OBD2_MERGE

  String rawObd2;
  rawObd2 += "0105:"; rawObd2 += r0105;
  rawObd2 += " 010B:"; rawObd2 += r010B;
  rawObd2 += " 010C:"; rawObd2 += r010C;
  rawObd2 += " 010D:"; rawObd2 += r010D;
  rawObd2 += " 0110:"; rawObd2 += r0110;
  rawObd2 += " 0111:"; rawObd2 += r0111;
  rawObd2 += " 0123:"; rawObd2 += r0123;
  rawObd2 += " ATRV:"; rawObd2 += atrv;

  if (fifoPush(d, rawObd2, "", "", "")) {
    STEPF("[FIFO] OBD2 trame ajoutée (%d/%d)\n", fifoCount, FIFO_BUFFER_SIZE);
  } else {
    STEPF("[FIFO] Buffer plein, trame perdue\n");
  }
  return true;
}
#endif

// Parse les DTC depuis une réponse hex (format: "43 01 33 00 00 00 00")
// Retourne le nombre de DTC trouvés et les stocke dans le tableau dtcs
static int parseDTCs(const String& hexResponse, String* dtcs, int maxDtcs) {
  int count = 0;
  
  // Nettoyer la réponse (enlever espaces, retours à la ligne, etc.)
  String clean = hexResponse;
  clean.replace(" ", "");
  clean.replace("\r", "");
  clean.replace("\n", "");
  clean.replace("43", ""); // Enlever le préfixe "43" (Mode 03 response)
  clean.toUpperCase();
  
  // Parser les DTC (chaque DTC = 2 bytes = 4 caractères hex)
  // Format: 2 bytes par DTC, chaque byte = 2 caractères hex
  for (int i = 0; i < clean.length() - 3 && count < maxDtcs; i += 4) {
    String dtcHex = clean.substring(i, i + 4);
    if (dtcHex.length() == 4 && dtcHex != "0000") {
      // Convertir hex en DTC (ex: "0133" -> "P0133")
      uint8_t byte1 = strtol(dtcHex.substring(0, 2).c_str(), nullptr, 16);
      uint8_t byte2 = strtol(dtcHex.substring(2, 4).c_str(), nullptr, 16);
      
      // Décoder le type de DTC selon le format OBD-II
      // Byte 1: bits 7-6 = type (00=P, 01=C, 10=B, 11=U)
      char dtcType;
      uint8_t dtcTypeBits = (byte1 >> 6) & 0x03;
      if (dtcTypeBits == 0) dtcType = 'P';      // Powertrain
      else if (dtcTypeBits == 1) dtcType = 'C'; // Chassis
      else if (dtcTypeBits == 2) dtcType = 'B'; // Body
      else dtcType = 'U'; // Network
      
      // Construire le code DTC (ex: P0133)
      // Format OBD-II: 
      // - Byte 1 bits 5-4 = premier chiffre (0-3)
      // - Byte 1 bits 3-0 = deuxième chiffre (0-F)
      // - Byte 2 bits 7-6 = troisième chiffre (0-3)
      // - Byte 2 bits 5-0 = quatrième chiffre (0-3F, mais on limite à F pour affichage standard)
      uint8_t digit1 = (byte1 >> 4) & 0x03;
      uint8_t digit2 = byte1 & 0x0F;
      uint8_t digit3 = (byte2 >> 6) & 0x03;
      uint8_t digit4 = byte2 & 0x3F;
      if (digit4 > 0x0F) digit4 = 0x0F; // Limiter à F pour affichage standard
      
      String dtcCode;
      dtcCode += dtcType;
      // Convertir en hex (sprintf avec %X génère directement des majuscules)
      char hexStr[6];
      sprintf(hexStr, "%X%X%X%X", digit1, digit2, digit3, digit4);
      dtcCode += hexStr;
      
      dtcs[count++] = dtcCode;
    }
  }
  
  return count;
}

// Lit les DTC via ELM327 (Mode 03: Request DTCs)
static bool readAndPublishDTCs() {
  if (!mqttEnsureConnected()) {
    STEPLN("[DTC] ERREUR: MQTT non connecté");
    return false;
  }
  
  STEPLN("[DTC] Lecture des DTC...");
  String response = elmQuery("03", 2000);
  
  if (response.indexOf("NO DATA") >= 0 || response.indexOf("ERROR") >= 0) {
    STEPLN("[DTC] Pas de DTC ou erreur de lecture");
    // Publier un JSON vide pour indiquer qu'il n'y a pas de DTC
    StaticJsonDocument<256> doc;
    doc["app"] = app_name;
    doc["ver"] = version;
    doc["ts_ms"] = (uint32_t)millis();
    doc["datetime"] = getReadableDateTime();
    doc["dtcs"] = JsonArray();
    doc["count"] = 0;
    
    char payload[256];
    size_t n = serializeJson(doc, payload, sizeof(payload));
    mqtt.publish(topic_dtc, (uint8_t*)payload, n, false);
    return true;
  }
  
  // Parser les DTC (max 10 DTC)
  String dtcs[10];
  int dtcCount = parseDTCs(response, dtcs, 10);
  
  // Publier sur MQTT
  StaticJsonDocument<1024> doc;
  doc["app"] = app_name;
  doc["ver"] = version;
  doc["ts_ms"] = (uint32_t)millis();
  doc["datetime"] = getReadableDateTime();
  doc["count"] = dtcCount;
  
  JsonArray dtcArray = doc.createNestedArray("dtcs");
  for (int i = 0; i < dtcCount; i++) {
    dtcArray.add(dtcs[i]);
  }
  
  // Ajouter aussi la réponse brute pour debug
  doc["raw"] = response;
  
  char payload[1024];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  
  if (mqtt.publish(topic_dtc, (uint8_t*)payload, n, false)) {
    STEPF("[DTC] OK: %d DTC(s) publié(s) - %s\n", dtcCount, topic_dtc);
    ledNoteMqttActivity();
    for (int i = 0; i < 8; i++) { mqtt.loop(); delay(2); }
    return true;
  } else {
    STEPF("[DTC] FAIL: Publication échouée\n");
    return false;
  }
}

// Collecte des données SZ (priorité: toujours collecter, même sans WiFi)
static bool pollSzPagesAndCollect() {
  // Pages principales vues dans le log: 21A0, 21A2, 21A5, 21CD (+ parfois 2180/17FF00)
  String rA0 = elmQuery("21A0 1", 1200);
  String rA2 = elmQuery("21A2 1", 1200);
  String rA5 = elmQuery("21A5 1", 1200);
  String rCD = elmQuery("21CD 1", 1200);

  // Détecter "NO DATA" → contact coupé, réinitialiser ELM
  if (rA0.indexOf("NO DATA") >= 0 || rA2.indexOf("NO DATA") >= 0 || 
      rA5.indexOf("NO DATA") >= 0 || rCD.indexOf("NO DATA") >= 0) {
    STEPLN("[ELM] NO DATA détecté → contact coupé, réinitialisation ELM...");
    return false;  // Indique qu'il faut réinitialiser
  }

  // Vitesse standard OBD-II (PID 010D) : plus fiable que l'offset propriétaire.
  // Limiter la fréquence pour ne pas surcharger le bus.
  static unsigned long last010Dms = 0;
  float speed010D = NAN;
  if (millis() - last010Dms > 500) {
    last010Dms = millis();
    // Certains ECU ne répondent au mode 01 qu'avec l'entête "standard" KWP (686AF1).
    // On bascule temporairement l'entête, puis on restaure celui de SZ Viewer (817AF1).
    elmQuery("ATSH686AF1");
    String r010D = elmQuery("010D", 900);
    elmQuery("ATSH817AF1");
    (void)parseObdPid010D_SpeedKmh(r010D, speed010D);
  }

  // Convert to bytes for decoding
  uint8_t bA0[256], bA2[256], bA5[256], bCD[64];
  size_t nA0 = szDecodeHexToBytes(rA0, bA0, sizeof(bA0));
  size_t nA2 = szDecodeHexToBytes(rA2, bA2, sizeof(bA2));
  size_t nA5 = szDecodeHexToBytes(rA5, bA5, sizeof(bA5));
  size_t nCD = szDecodeHexToBytes(rCD, bCD, sizeof(bCD));

  SzData d;
  decodeSzFromPages(bA0, nA0, bA2, nA2, bA5, nA5, bCD, nCD, d);

  // Conserver la dernière valeur connue pour les champs non remontés (trame courte / absente)
  static SzData lastSz;
  #define SZ_MERGE(field) do { \
    if (isnan(d.field)) { if (!isnan(lastSz.field)) d.field = lastSz.field; } \
    else { lastSz.field = d.field; } \
  } while(0)
  SZ_MERGE(desired_idle_speed_rpm);
  SZ_MERGE(accelerator_pct);
  SZ_MERGE(intake_c);
  SZ_MERGE(battery_v);
  SZ_MERGE(fuel_temp_c);
  SZ_MERGE(bar_pressure_kpa);
  SZ_MERGE(bar_pressure_mmhg);
  SZ_MERGE(abs_pressure_mbar);
  SZ_MERGE(air_flow_estimate_mgcp);
  SZ_MERGE(air_flow_request_mgcp);
  SZ_MERGE(speed_kmh);
  SZ_MERGE(rail_pressure_bar);
  SZ_MERGE(rail_pressure_control_bar);
  SZ_MERGE(desired_egr_position_pct);
  SZ_MERGE(gear_ratio);
  SZ_MERGE(egr_position_pct);
  SZ_MERGE(engine_temp_c);
  SZ_MERGE(air_temp_c);
  SZ_MERGE(requested_in_pressure_mbar);
  SZ_MERGE(engine_rpm);
  #undef SZ_MERGE

  // Override speed_kmh si PID 010D disponible et plausible
  if (!isnan(speed010D) && speed010D >= 0.0f && speed010D <= 250.0f) {
    d.speed_kmh = speed010D;
    lastSz.speed_kmh = speed010D;
  }

  // Ajouter au buffer FIFO (priorité: collecte continue)
  if (fifoPush(d, rA0, rA2, rA5, rCD)) {
    STEPF("[FIFO] Trame ajoutée (%d/%d)\n", fifoCount, FIFO_BUFFER_SIZE);
  } else {
    STEPF("[FIFO] Buffer plein, trame perdue\n");
  }
  
  return true;
}

// Envoi MQTT depuis le buffer FIFO (si WiFi OK)
static void fifoSendToMqtt() {
  static unsigned long lastDebugMs = 0;
  
  if (!mqttEnsureConnected()) {
    if (millis() - lastDebugMs > 5000) {
      STEPF("[FIFO] DEBUG - MQTT non connecté, buffer: %d/%d trames\n", fifoCount, FIFO_BUFFER_SIZE);
      lastDebugMs = millis();
    }
    return;  // Pas de WiFi/MQTT, on garde les trames dans le buffer
  }
  
  if (fifoCount == 0) {
    if (millis() - lastDebugMs > 10000) {
      STEPLN("[FIFO] DEBUG - Buffer vide (pas de données collectées)");
      lastDebugMs = millis();
    }
    return;
  }
  
  STEPF("[FIFO] DEBUG - Envoi de %d trame(s) depuis le buffer\n", (fifoCount < 5) ? fifoCount : 5);
  
  // Envoyer jusqu'à 5 trames par cycle pour ne pas surcharger
  int sent = 0;
  while (sent < 5 && fifoCount > 0) {
    SzData d;
    String rawA0, rawA2, rawA5, rawCD;
    uint32_t timestamp_ms;
    
    if (fifoPop(d, rawA0, rawA2, rawA5, rawCD, timestamp_ms)) {
      publishSzJson(d, rawA0, rawA2, rawA5, rawCD);
      sent++;
      STEPF("[FIFO] Trame envoyée (%d restantes)\n", fifoCount);
    } else {
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  STEPLN("\n========================================");
  STEPF("  APP: %s\n", app_name);
  STEPF("  VER: %s\n", version);
#if USE_OBD2_PROTOCOL
  STEPLN("  PROTOCOL: OBD2 (Car Scanner PIDs)");
#else
  STEPLN("  PROTOCOL: SZ (21A0/21A2/21A5/21CD)");
#endif
  STEPLN("========================================");

  // --- WiFi Multi-AP (adapter à ton contexte) ---
  // IMPORTANT: garde tes APs existants si tu merges avec ton autre sketch.
  wifiMulti.addAP("LPB", "lapet1teb0rde");               // Réseau Maison
  wifiMulti.addAP("<_JNoel iPhone_>", "bachibouzook");   // iPhone
  // Note: Les réseaux ouverts seront ajoutés automatiquement lors du scan

  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setBufferSize(2048); // Buffer pour messages JSON (~985 bytes)

  setCpuFrequencyMhz(160);
  
  // Initialiser le buffer FIFO en premier (utilise la mémoire disponible)
  fifoInit();
  
  BLEDevice::init("CW-S3-F4IAE");
  // Note: bleClient sera créé uniquement lors de la connexion BLE
  
  // Initialisation LEDs
  ledInit();
  STEPLN("[LED] LEDs initialisées");
  STEPLN("[LED] NOTE: Pin 34 est INPUT ONLY sur ESP32");
  STEPLN("[LED] Utilisation d'une seule LED (orange) pour WiFi et BLE");
  STEPLN("[LED] Les patterns seront combinés selon l'état");
  
  // Test de la LED orange au démarrage
  STEPLN("[LED] Test LED orange (WiFi/BLE combiné)...");
  for (int i = 0; i < 5; i++) {
    ledWifiWrite(true);
    delay(200);
    ledWifiWrite(false);
    delay(200);
  }
  STEPLN("[LED] Test LED terminé");
}

void loop() {
  wifiTick();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = mqtt.connected();

  // Étapes visibles: si l'état change, on "resynchronise" la LED pour que le pattern change
  static SystemState lastLedState = BOOT;
  if (currentState != lastLedState) {
    ledMarkStep();
    lastLedState = currentState;
  }

  // Toujours appeler mqtt.loop() si WiFi est connecté (même si MQTT est déconnecté)
  // Cela permet à PubSubClient de gérer la reconnexion automatique
  if (wifiOk) {
    // Maintenir la connexion MQTT même sans données à publier
    if (!mqttOk) {
      static unsigned long lastMqttReconnectAttempt = 0;
      if (millis() - lastMqttReconnectAttempt > 5000) { // Tenter reconnexion toutes les 5 secondes
        lastMqttReconnectAttempt = millis();
        if (mqttEnsureConnected()) {
          mqttPublishStatus("WiFi OK, MQTT OK");
        }
      }
    } else {
      // MQTT connecté : heartbeat périodique pour maintenir la connexion
      static unsigned long lastHeartbeat = 0;
      if (millis() - lastHeartbeat > 30000) { // Heartbeat toutes les 30 secondes
        lastHeartbeat = millis();
        // Vérifier que la connexion est toujours active
        if (!mqtt.connected()) {
          STEPLN("[MQTT] Connexion perdue, tentative de reconnexion...");
          mqttEnsureConnected();
        }
      }
    }
    
    // Appeler loop() régulièrement pour maintenir la connexion
    mqtt.loop();
    
    // Envoi MQTT depuis le buffer FIFO (si WiFi OK)
    fifoSendToMqtt();
  }

  // Mise à jour LED unique (orange) avec patterns combinés WiFi + BLE
  // NOTE: XIAO ESP32S3 SENSE n'a qu'une seule LED accessible (orange, pin 21)
  // ledBleUpdate() ne contrôle plus de pin séparé, les patterns sont combinés dans ledEnsureAtLeastOneOn()
  ledEnsureAtLeastOneOn(currentState, wifiOk, mqttOk, bleConnected);

  // Priorité Internet: si on est en BLE/ELM/POLL et que le WiFi tombe, on revient à CONNECT_WIFI
  // pour rétablir l'accès (et NTP) avant de reprendre le BLE.
  if (!wifiOk && (currentState == CONNECT_BLE || currentState == INIT_ELM || currentState == POLL_SZ)) {
    STEPLN("[FSM] Perte WiFi → priorité reconnexion internet (retour CONNECT_WIFI)");
    if (currentState == POLL_SZ || currentState == INIT_ELM) {
      bleConnected = false;
      if (bleClient) {
        bleClient->disconnect();
        szDelay(100);
      }
    }
    currentState = CONNECT_WIFI;
    STEPLN("[FSM] État: CONNECT_WIFI");
  }

  switch (currentState) {
    case BOOT:
      currentState = CONNECT_WIFI;
      STEPLN("[FSM] État: CONNECT_WIFI");
      break;

    case CONNECT_WIFI:
      if (wifiOk) {
        STEPF("[FSM] WiFi connecté à: %s (IP: %s)\n", 
                      WiFi.SSID().c_str(), 
                      WiFi.localIP().toString().c_str());
        // Priorité 1: Internet + NTP pour traces horodatées avant toute chose
        configureNTP();
        STEPLN("[FSM] NTP configuré (traces horodatées) → CONNECT_MQTT");
        currentState = CONNECT_MQTT;
        STEPLN("[FSM] État: CONNECT_MQTT");
      }
      break;

    case CONNECT_MQTT:
      if (mqttOk) {
        STEPLN("[FSM] MQTT connecté");
        currentState = CONNECT_BLE;
        STEPLN("[FSM] État: CONNECT_BLE");
      }
      break;

    case CONNECT_BLE:
      if (millis() - lastBleAttemptMs > 5000) {
        lastBleAttemptMs = millis();
        if (!vLinkerAddressValid) {
          STEPLN("[FSM] Scan BLE pour vLinker…");
          // Lister les 5 meilleurs devices BLE
          bleScanAndListTop5();
        } else {
          STEPF("[FSM] Tentative de connexion BLE à %s…\n", vLinkerAddress.toString().c_str());
        }
        if (bleConnectVlinker()) {
          bleConnected = true;
          STEPLN("[FSM] BLE OK (write+notify trouvés)");
          if (mqttOk) {
          mqttPublishStatus("BLE OK");
          }
          currentState = INIT_ELM;
        } else {
          bleConnected = false;
          STEPLN("[FSM] BLE KO (retry dans 5s)");
          // Réinitialiser le flag pour forcer un nouveau scan au prochain essai
          if (!vLinkerAddressValid) {
            szDelay(100); // Petit délai avant de réessayer
          }
        }
      }
      break;

    case INIT_ELM:
      if (!bleClient || !bleClient->isConnected()) {
        STEPLN("[FSM] INIT_ELM: BLE déconnecté, retour à CONNECT_BLE");
        bleConnected = false;
        vLinkerAddressValid = false; // Force un nouveau scan
        if (bleClient) {
          bleClient->disconnect();
          szDelay(100);
        }
        lastBleAttemptMs = millis(); // Réinitialiser le timer
        currentState = CONNECT_BLE;
        break;
      }
#if USE_OBD2_PROTOCOL
      STEPLN("[FSM] Init ELM OBD2 (Car Scanner style)…");
      if (elmInitObd2()) {
#else
      STEPLN("[FSM] Init ELM like SZ Viewer…");
      if (elmInitLikeSzViewer()) {
#endif
        if (mqttOk) {
          mqttPublishStatus("ELM init done");
        }
        currentState = POLL_SZ;
      } else {
        STEPLN("[FSM] Échec init ELM, réessai dans 5s...");
        szDelay(5000);
        // Reste en INIT_ELM pour réessayer
      }
      break;

    case POLL_SZ:
      if (!bleClient || !bleClient->isConnected()) {
        STEPLN("[FSM] POLL_SZ: BLE déconnecté, retour à CONNECT_BLE");
        bleConnected = false;
        vLinkerAddressValid = false; // Force un nouveau scan
        if (bleClient) {
          bleClient->disconnect();
          szDelay(100);
        }
        lastBleAttemptMs = millis(); // Réinitialiser le timer
        currentState = CONNECT_BLE;
        break;
      }
      if (millis() - lastPollMs > 500) { // à ajuster selon charge bus
        lastPollMs = millis();
#if USE_OBD2_PROTOCOL
        if (!pollObd2AndCollect()) {
#else
        if (!pollSzPagesAndCollect()) {
#endif
          STEPLN("[FSM] NO DATA → retour à INIT_ELM");
          currentState = INIT_ELM;
        }
      }
      // Envoi MQTT depuis le buffer FIFO (si WiFi OK)
      fifoSendToMqtt();
      
      // Lecture des DTC toutes les 30 secondes (moins fréquent que les données SZ)
      static uint32_t lastDtcReadMs = 0;
      if (millis() - lastDtcReadMs > 30000) {
        lastDtcReadMs = millis();
        readAndPublishDTCs();
      }
      break;

    default:
      currentState = BOOT;
      break;
  }
}

