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

// --- Structure de données SZ Viewer (déclarée tôt pour visibilité) ---
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
static const char* version  = "0.4.4";

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

// Système de LEDs expressif
// Au moins une LED doit être allumée (fixe ou clignotante min 1s) si l'ESP est sous tension
// Fréquences: 1000ms (lent), 500ms (moyen), 250ms (rapide), 100ms (très rapide)
static uint32_t ledWifiLastToggle = 0;
static uint32_t ledBleLastToggle = 0;
static bool ledWifiState = false;
static bool ledBleState = false;

// Patterns LED: période en ms (0 = fixe), duty cycle (0-100)
struct LedPattern {
  uint32_t period_ms;  // 0 = fixe, >0 = clignotant
  uint8_t duty_cycle;  // 0-100 (% de temps ON)
};

// LED WiFi (orange): États WiFi/MQTT
static LedPattern getWifiPattern(SystemState state, bool wifiOk, bool mqttOk) {
  LedPattern p = {0, 100};
  
  if (state == BOOT) {
    // Boot: clignotement lent 1s (50% duty)
    p.period_ms = 1000;
    p.duty_cycle = 50;
  } else if (state == CONNECT_WIFI) {
    // Connexion WiFi: clignotement moyen 500ms (30% duty - faible)
    p.period_ms = 500;
    p.duty_cycle = 30;
  } else if (state == CONNECT_MQTT) {
    // Connexion MQTT: clignotement rapide 250ms (50% duty)
    p.period_ms = 250;
    p.duty_cycle = 50;
  } else if (wifiOk && mqttOk) {
    // WiFi + MQTT OK: fixe allumé
    p.period_ms = 0;
    p.duty_cycle = 100;
  } else if (wifiOk && !mqttOk) {
    // WiFi OK mais MQTT KO: clignotement lent 1s (70% duty)
    p.period_ms = 1000;
    p.duty_cycle = 70;
  } else {
    // WiFi KO: clignotement très lent 2s (20% duty - très faible)
    p.period_ms = 2000;
    p.duty_cycle = 20;
  }
  
  return p;
}

// LED BLE (rouge): États BLE/OBD
static LedPattern getBlePattern(SystemState state, bool bleOk) {
  LedPattern p = {0, 100};
  
  if (state == BOOT || state == CONNECT_WIFI || state == CONNECT_MQTT) {
    // Pas encore en mode BLE: éteint
    p.period_ms = 0;
    p.duty_cycle = 0;
  } else if (state == CONNECT_BLE) {
    // Scan/connexion BLE: clignotement moyen 500ms (50% duty)
    p.period_ms = 500;
    p.duty_cycle = 50;
  } else if (state == INIT_ELM) {
    // Init ELM: clignotement rapide 250ms (50% duty)
    p.period_ms = 250;
    p.duty_cycle = 50;
  } else if (state == POLL_SZ && bleOk) {
    // Polling actif: clignotement très rapide 100ms (30% duty - faible)
    p.period_ms = 100;
    p.duty_cycle = 30;
  } else if (bleOk) {
    // BLE connecté mais pas en polling: fixe allumé
    p.period_ms = 0;
    p.duty_cycle = 100;
  } else {
    // BLE déconnecté: clignotement lent 1s (20% duty - très faible)
    p.period_ms = 1000;
    p.duty_cycle = 20;
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
  Serial.printf("[FIFO] Mémoire libre avant allocation: %u bytes\n", freeHeapBefore);
  
  // Réserver seulement 20% de la mémoire libre pour le buffer FIFO
  // (le reste est nécessaire pour WiFi, MQTT, BLE, JSON, etc.)
  uint32_t availableForFifo = (freeHeapBefore * 20) / 100;
  Serial.printf("[FIFO] Mémoire réservée pour FIFO (20%%): %u bytes\n", availableForFifo);
  
  // Calculer le nombre d'entrées possibles
  int maxEntries = availableForFifo / ESTIMATED_ENTRY_SIZE;
  
  // Limite minimale uniquement (pour garantir un fonctionnement de base)
  // Pas de limite maximale : utiliser toute la mémoire disponible si nécessaire
  if (maxEntries < 10) {
    maxEntries = 10;
    Serial.println("[FIFO] Mémoire limitée, utilisation du minimum (10 entrées)");
  }
  
  // Afficher un avertissement si le buffer est très grand (pour info)
  if (maxEntries > 1000) {
    Serial.printf("[FIFO] Mémoire abondante: %d entrées allouées (~%u KB)\n", 
                  maxEntries, (maxEntries * ESTIMATED_ENTRY_SIZE) / 1024);
  }
  
  FIFO_BUFFER_SIZE = maxEntries;
  
  // Allouer le buffer
  fifoBuffer = new FifoEntry[FIFO_BUFFER_SIZE];
  if (!fifoBuffer) {
    Serial.println("[FIFO] ERREUR: Échec d'allocation mémoire !");
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
  
  Serial.printf("[FIFO] Buffer alloué: %d entrées\n", FIFO_BUFFER_SIZE);
  Serial.printf("[FIFO] Mémoire utilisée: %u bytes (estimation: ~%u bytes)\n",
                memoryUsed, FIFO_BUFFER_SIZE * ESTIMATED_ENTRY_SIZE);
  Serial.printf("[FIFO] Mémoire libre après allocation: %u bytes\n", freeHeapAfter);
  
  // Avertissement si la mémoire libre est trop faible pour MQTT
  if (freeHeapAfter < 50000) {
    Serial.printf("[FIFO] ATTENTION: Mémoire libre faible (%u bytes) - MQTT pourrait avoir des problèmes\n", freeHeapAfter);
  }
}

// Ajoute une trame au buffer FIFO (retourne false si buffer plein)
static bool fifoPush(const SzData& d, const String& rawA0, const String& rawA2, const String& rawA5, const String& rawCD) {
  if (fifoCount >= FIFO_BUFFER_SIZE) {
    Serial.printf("[FIFO] Buffer plein (%d/%d), perte de trame\n", fifoCount, FIFO_BUFFER_SIZE);
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
  String received;
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    rxBuf += c;
    received += c;
  }
  // Log seulement les caractères non-printables ou les réponses importantes
  if (received.indexOf(">") >= 0 || received.indexOf("OK") >= 0 || received.indexOf("ERROR") >= 0) {
    Serial.printf("[BLE] NOTIFY (%d bytes): %s\n", len, received.c_str());
  }
}

static bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  
  // Vérifier la mémoire avant connexion
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[MQTT] Tentative de connexion - mémoire libre: %u bytes\n", freeHeap);
  
  if (freeHeap < 20000) {
    Serial.printf("[MQTT] ATTENTION: Mémoire libre faible (%u bytes) - risque d'échec\n", freeHeap);
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
    
    Serial.printf("[MQTT] Échec de connexion - état: %d (%s), mémoire libre: %u bytes\n", 
                  mqttState, stateStr, ESP.getFreeHeap());
    Serial.printf("[MQTT] Vérifier: WiFi connecté ? (%d), serveur: %s:%d\n",
                  WiFi.status() == WL_CONNECTED, mqtt_server, mqtt_port);
  } else {
    Serial.printf("[MQTT] Connecté avec succès - mémoire libre: %u bytes\n", ESP.getFreeHeap());
  }
  
  return connected;
}

// Configure NTP si pas déjà fait
static void configureNTP() {
  if (ntpConfigured) return;
  
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    ntpConfigured = true;
    Serial.println("[NTP] Configuration NTP effectuée");
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
      Serial.printf("[STATUS] MQTT non connecté, message non publié: %s\n", message);
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
    Serial.printf("[STATUS] Publié: %s (%s) - WiFi:%s MQTT:%s BLE:%s\n", 
                  message, getReadableDateTime().c_str(), 
                  wifiStatus.c_str(), mqttStatus.c_str(), bleStatus.c_str());
    mqtt.loop();
  } else {
    Serial.printf("[STATUS] Échec publication: %s\n", message);
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
  Serial.println("[WiFi] Scan des réseaux disponibles...");
  int n = WiFi.scanNetworks();
  
  if (n == 0) {
    Serial.println("[WiFi] Aucun réseau trouvé");
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
  Serial.printf("[WiFi] Top %d réseau(x) (par RSSI):\n", topCount);
  for (int i = 0; i < topCount; i++) {
    const char* encStr = networks[i].isOpen ? "OUVERT" : "protégé";
    Serial.printf("  %d. %s (RSSI: %d dBm, %s)\n", 
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
        Serial.printf("    → Ajouté au WiFiMulti (ouvert, portail captif possible)\n");
      } else {
        Serial.printf("    → Ignoré (blacklisté): %s\n", networks[i].ssid.c_str());
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
    delay(100); // Petit délai entre les tentatives
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
    Serial.printf("[WiFi] SSID ajouté à la blacklist: %s (pour 5 minutes)\n", ssid.c_str());
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
    Serial.printf("[WiFi] SSID remplacé dans la blacklist: %s\n", ssid.c_str());
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
        Serial.printf("[WiFi] SSID retiré de la blacklist (timeout): %s\n", ssid.c_str());
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
  
  Serial.println("[WiFi] Portail captif détecté - contournement automatique non implémenté");
  Serial.println("[WiFi] Le réseau nécessite une authentification manuelle via navigateur");
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
      Serial.printf("[WiFi] Connecté à: %s (IP: %s, RSSI: %d dBm)\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      // Configurer NTP dès que WiFi est connecté
      configureNTP();
      captivePortalChecked = false; // Réinitialiser le flag pour le nouveau réseau
      lastCaptiveCheckMs = millis();
    } else {
      Serial.println("[WiFi] Déconnecté");
      ntpConfigured = false; // Réinitialiser pour la prochaine connexion
      captivePortalChecked = false;
    }
    lastWifiStatus = currentWifiStatus;
  }
  
  // Vérifier le portail captif après connexion (une seule fois, après 2 secondes)
  if (currentWifiStatus && !captivePortalChecked && millis() - lastCaptiveCheckMs > 2000) {
    Serial.println("[WiFi] Vérification portail captif...");
    if (wifiDetectCaptivePortal()) {
      Serial.println("[WiFi] ⚠️  PORTAL CAPTIF DÉTECTÉ");
      Serial.println("[WiFi] Le réseau nécessite une authentification via navigateur web");
      
      // Tenter un contournement
      if (!wifiBypassCaptivePortal()) {
        // Contournement impossible → déconnecter et chercher un autre réseau
        String currentSSID = WiFi.SSID();
        Serial.println("[WiFi] Contournement impossible → déconnexion et recherche d'un autre réseau");
        
        // Ajouter à la blacklist temporaire (5 minutes)
        wifiBlacklistSSID(currentSSID);
        
        // Déconnecter
        WiFi.disconnect();
        delay(500);
        
        // Réinitialiser le flag pour permettre une nouvelle vérification
        captivePortalChecked = false;
        lastWifiStatus = false; // Forcer la détection de changement
        
        // Forcer une nouvelle recherche
        Serial.println("[WiFi] Recherche d'un autre réseau WiFi...");
        return; // Sortir de la fonction pour permettre à wifiMulti.run() de chercher un autre réseau
      } else {
        Serial.println("[WiFi] ✓ Portail captif contourné avec succès");
      }
    } else {
      Serial.println("[WiFi] ✓ Pas de portail captif détecté");
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
  digitalWrite(LED_WIFI_PIN, LOW);
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
    digitalWrite(LED_WIFI_PIN, ledWifiState ? HIGH : LOW);
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
// NOTE: Avec une seule LED (orange), on combine les patterns WiFi et BLE
static void ledEnsureAtLeastOneOn(SystemState state, bool wifiOk, bool mqttOk, bool bleOk) {
  LedPattern pWifi = getWifiPattern(state, wifiOk, mqttOk);
  LedPattern pBle = getBlePattern(state, bleOk);
  
  bool wifiOn = (pWifi.duty_cycle > 0);
  bool bleOn = (pBle.duty_cycle > 0);
  
  // Avec une seule LED, on combine les patterns :
  // - Si WiFi et BLE sont tous les deux ON, utiliser le pattern le plus rapide
  // - Si un seul est ON, utiliser son pattern
  // - Si aucun n'est ON, forcer un pattern de secours
  
  uint32_t now = millis();
  bool shouldBeOn = false;
  uint32_t period = 1000;
  
  if (wifiOn && bleOn) {
    // Les deux sont actifs : utiliser le pattern le plus rapide (priorité à l'activité)
    uint32_t periodWifi = (pWifi.period_ms == 0) ? 1000 : pWifi.period_ms;
    uint32_t periodBle = (pBle.period_ms == 0) ? 1000 : pBle.period_ms;
    period = (periodWifi < periodBle) ? periodWifi : periodBle;
    uint32_t cyclePos = (now - ledWifiLastToggle) % period;
    uint32_t onTime = (period * 50) / 100; // 50% duty cycle pour pattern combiné
    shouldBeOn = (cyclePos < onTime);
  } else if (wifiOn) {
    // Seulement WiFi actif
    if (pWifi.period_ms == 0) {
      shouldBeOn = true;
    } else {
      uint32_t cyclePos = (now - ledWifiLastToggle) % pWifi.period_ms;
      uint32_t onTime = (pWifi.period_ms * pWifi.duty_cycle) / 100;
      shouldBeOn = (cyclePos < onTime);
    }
    period = pWifi.period_ms;
  } else if (bleOn) {
    // Seulement BLE actif
    if (pBle.period_ms == 0) {
      shouldBeOn = true;
    } else {
      uint32_t cyclePos = (now - ledBleLastToggle) % pBle.period_ms;
      uint32_t onTime = (pBle.period_ms * pBle.duty_cycle) / 100;
      shouldBeOn = (cyclePos < onTime);
    }
    period = pBle.period_ms;
  } else {
    // Aucun actif : pattern de secours (1s, 50%)
    period = 1000;
    uint32_t cyclePos = (now - ledWifiLastToggle) % period;
    shouldBeOn = (cyclePos < period / 2);
  }
  
  if (shouldBeOn != ledWifiState) {
    ledWifiState = shouldBeOn;
    digitalWrite(LED_WIFI_PIN, ledWifiState ? HIGH : LOW);
    if (ledWifiLastToggle == 0 || (now - ledWifiLastToggle) >= period) {
      ledWifiLastToggle = now;
    }
  }
}

static bool bleFindUartLikeChars(BLEClient* client) {
  chrWrite = nullptr;
  chrNotify = nullptr;

  std::map<std::string, BLERemoteService*>* services = client->getServices();
  if (!services) {
    Serial.println("[BLE] Aucun service trouvé");
    return false;
  }

  Serial.printf("[BLE] %d service(s) trouvé(s)\n", services->size());
  int serviceIdx = 0;
  for (auto const& it : *services) {
    BLERemoteService* svc = it.second;
    if (!svc) continue;
    auto* chrs = svc->getCharacteristics();
    if (!chrs) continue;

    Serial.printf("[BLE] Service %d: %d caractéristique(s)\n", serviceIdx++, chrs->size());
    int charIdx = 0;
    for (auto const& cit : *chrs) {
      BLERemoteCharacteristic* ch = cit.second;
      if (!ch) continue;

      // Heuristique:
      // - un char "write" (WRITE ou WRITE_NO_RSP)
      // - un char "notify" (NOTIFY) pour récupérer les réponses ELM ('>' etc.)
      if (!chrWrite && (ch->canWrite() || ch->canWriteNoResponse())) {
        chrWrite = ch;
        Serial.printf("[BLE] Caractéristique write trouvée (idx %d)\n", charIdx);
      }
      if (!chrNotify && ch->canNotify()) {
        chrNotify = ch;
        Serial.printf("[BLE] Caractéristique notify trouvée (idx %d)\n", charIdx);
      }

      if (chrWrite && chrNotify) break;
      charIdx++;
    }
    if (chrWrite && chrNotify) break;
  }

  if (!chrWrite) {
    Serial.println("[BLE] ERREUR: Aucune caractéristique write trouvée");
    return false;
  }
  if (!chrNotify) {
    Serial.println("[BLE] ERREUR: Aucune caractéristique notify trouvée");
    return false;
  }
  
  Serial.println("[BLE] Enregistrement callback notify...");
  chrNotify->registerForNotify(notifyCallback);
  Serial.println("[BLE] Callback notify enregistré");
  delay(100); // Délai pour que les notifications soient activées
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
    Serial.println("[BLE] BLE non initialisé");
    return;
  }

  Serial.println("[BLE] Scan des devices BLE...");
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  BLEScanResults results = scan->start(3, false);  // Scan de 3 secondes
  int deviceCount = results.getCount();
  Serial.printf("[BLE] Scan terminé: %d device(s) trouvé(s)\n", deviceCount);
  
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
  Serial.printf("[BLE] Top %d device(s) (par RSSI):\n", topCount);
  for (int i = 0; i < topCount; i++) {
    const char* vlinkerStr = devices[i].isVlinker ? " [vLinker]" : "";
    const char* nameStr = devices[i].name.length() > 0 ? devices[i].name.c_str() : "(sans nom)";
    Serial.printf("  %d. %s (%s, RSSI: %d dBm)%s\n", 
                  i + 1,
                  nameStr,
                  devices[i].address.c_str(),
                  devices[i].rssi,
                  vlinkerStr);
  }
  
  // DEBUG: Afficher TOUS les devices avec leurs noms complets
  Serial.println("[BLE] DEBUG - Liste complète des devices:");
  for (int i = 0; i < deviceCount; i++) {
    BLEAdvertisedDevice device = results.getDevice(i);
    String name = String(device.getName().c_str());
    String addr = String(device.getAddress().toString().c_str());
    Serial.printf("  [%d] Nom: '%s' (len=%d), Addr: %s, RSSI: %d\n", 
                  i, name.c_str(), name.length(), addr.c_str(), device.getRSSI());
  }
  
  scan->clearResults();
}

// Scan BLE pour trouver un device dont le nom contient "vLinker"
static bool bleScanForVlinker(uint32_t scanTimeMs = 5000) {
  // S'assurer qu'on n'est pas connecté avant de scanner
  if (bleClient) {
    if (bleClient->isConnected()) {
      Serial.println("[BLE] Déconnexion avant scan...");
      bleClient->disconnect();
    }
    // Détruire le client pour éviter les conflits
    delete bleClient;
    bleClient = nullptr;
    delay(300);
  }

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  Serial.println("[BLE] Démarrage scan...");
  BLEScanResults results = scan->start(scanTimeMs / 1000, false);
  Serial.printf("[BLE] Scan terminé: %d devices trouvés\n", results.getCount());
  
  // Attendre un peu que le scan soit complètement terminé
  delay(200);
  
  Serial.println("[BLE] DEBUG - Recherche vLinker dans les devices scannés...");
  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice device = results.getDevice(i);
    String name = String(device.getName().c_str());
    String addr = String(device.getAddress().toString().c_str());
    String nameUpper = name;
    nameUpper.toUpperCase();
    String pattern = String(vLinker_NamePattern);
    pattern.toUpperCase();
    
    Serial.printf("[BLE] DEBUG - Device %d: Nom='%s' (len=%d), Addr=%s\n", 
                  i, name.c_str(), name.length(), addr.c_str());
    
    if (nameUpper.indexOf(pattern) >= 0) {
      Serial.printf("[BLE] DEBUG - MATCH trouvé ! Nom contient '%s'\n", vLinker_NamePattern);
      vLinkerAddress = device.getAddress();
      // Créer une copie du device pour la connexion
      if (vLinkerDevice) {
        delete vLinkerDevice;
      }
      vLinkerDevice = new BLEAdvertisedDevice(device);
      vLinkerAddressValid = true;
      Serial.printf("[BLE] Trouvé vLinker: %s (%s, RSSI: %d dBm)\n", 
                    device.getName().c_str(), 
                    vLinkerAddress.toString().c_str(),
                    device.getRSSI());
      scan->clearResults(); // Nettoyer les résultats
      delay(300); // Délai avant de tenter la connexion
      return true;
    }
  }
  Serial.println("[BLE] DEBUG - Aucun device ne correspond au pattern 'vLinker'");
  Serial.printf("[BLE] DEBUG - Pattern recherché: '%s' (case insensitive)\n", vLinker_NamePattern);
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
      Serial.println("[BLE] vLinker non trouvé dans le scan");
      return false;
    }
  }

  // Toujours créer un nouveau client pour éviter les problèmes de réutilisation
  if (bleClient) {
    if (bleClient->isConnected()) {
      Serial.println("[BLE] Déconnexion du client existant...");
      bleClient->disconnect();
      delay(200);
    }
    // Détruire l'ancien client
    delete bleClient;
    bleClient = nullptr;
    delay(200);
  }

  // Créer un nouveau client
  Serial.println("[BLE] Création nouveau client BLE...");
  bleClient = BLEDevice::createClient();
  if (!bleClient) {
    Serial.println("[BLE] ERREUR: Impossible de créer le client BLE");
    return false;
  }

  // Connexion au device trouvé (utiliser l'objet device complet si disponible)
  Serial.printf("[BLE] Tentative de connexion à %s...\n", vLinkerAddress.toString().c_str());
  delay(200); // Délai avant la connexion
  
  bool connected = false;
  if (vLinkerDevice) {
    // Utiliser l'objet device complet (meilleure méthode)
    Serial.println("[BLE] Connexion via device object...");
    connected = bleClient->connect(vLinkerDevice);
  } else {
    // Fallback: utiliser l'adresse
    Serial.println("[BLE] Connexion via adresse MAC...");
    connected = bleClient->connect(vLinkerAddress);
  }
  
  if (!connected) {
    Serial.println("[BLE] Échec de connexion");
    Serial.println("[BLE] Vérifier que le vLinker n'est pas connecté à un autre appareil");
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
    delay(500);
    return false;
  }

  Serial.println("[BLE] Connecté ! Attente stabilisation...");
  delay(500); // Délai plus long pour que la connexion se stabilise

  Serial.println("[BLE] Recherche des caractéristiques UART...");
  if (!bleFindUartLikeChars(bleClient)) {
    Serial.println("[BLE] Caractéristiques UART non trouvées");
    bleClient->disconnect();
    delay(200);
    delete bleClient;
    bleClient = nullptr;
    if (vLinkerDevice) {
      delete vLinkerDevice;
      vLinkerDevice = nullptr;
    }
    vLinkerAddressValid = false;
    return false;
  }

  Serial.println("[BLE] Caractéristiques UART trouvées (write+notify)");
  return true;
}

// --- Helpers ELM style ---
static void elmClearRx() {
  rxBuf = "";
  gotNotify = false;
}

static bool elmWriteLine(const String& line) {
  if (!bleClient || !bleClient->isConnected() || !chrWrite) {
    Serial.println("[ELM] ERREUR: Client BLE non connecté ou caractéristique write absente");
    return false;
  }
  String cmd = line;
  if (!cmd.endsWith("\r")) cmd += "\r";
  Serial.printf("[ELM] TX: %s", cmd.c_str());
  chrWrite->writeValue((uint8_t*)cmd.c_str(), cmd.length(), false);
  delay(10); // Petit délai après l'envoi
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
      Serial.printf("[ELM] RX: %s>\n", out.c_str());
      return true;
    }
    // Afficher ce qui est reçu même sans prompt (pour debug)
    if (rxBuf.length() > 0 && (millis() - start) % 200 < 10) {
      Serial.printf("[ELM] RX (partiel, %d ms): %s\n", (int)(millis() - start), rxBuf.c_str());
    }
    delay(5);
  }
  out = rxBuf;
  if (out.length() > 0) {
    Serial.printf("[ELM] RX (timeout, %d ms): %s\n", timeoutMs, out.c_str());
  } else {
    Serial.printf("[ELM] RX (timeout, %d ms): Aucune réponse\n", timeoutMs);
  }
  return false;
}

static String elmQuery(const String& line, uint32_t timeoutMs = 1500) {
  elmClearRx();
  if (!elmWriteLine(line)) {
    Serial.println("[ELM] ERREUR: Impossible d'envoyer la commande");
    return "";
  }
  String resp;
  bool gotResponse = elmReadUntilPrompt(resp, timeoutMs);
  resp.replace("\r", "");
  resp.replace("\n", "");
  resp.trim();
  if (!gotResponse) {
    Serial.printf("[ELM] ATTENTION: Pas de réponse complète pour '%s'\n", line.c_str());
  }
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

// --- Décodage (basé sur analyse OCR) ---
// Décodage des 20 champs depuis les pages hexadécimales.
// Offsets et échelles identifiés via analyse des données OCR synchronisées.
// Note: Certains champs partagent le même offset (ex: speed_kmh et accelerator_pct à offset 6)
//       → À affiner avec plus de données ou reverse engineering manuel
static void decodeSzFromPages(
  const uint8_t* a0, size_t a0Len,
  const uint8_t* a2, size_t a2Len,
  const uint8_t* a5, size_t a5Len,
  const uint8_t* cd, size_t cdLen,
  SzData& out
) {
  // Page 21A0
  if (a0Len > 7) {
    // Note: speed_kmh et accelerator_pct partagent offset 6 → conflit à résoudre
    // Filtrer les valeurs aberrantes pour speed_kmh (0-300 km/h raisonnable)
    uint16_t raw_speed = (a0[6] << 8) | a0[7];
    float speed = (float)raw_speed;
    if (speed >= 0.0f && speed <= 300.0f) {
      out.speed_kmh = speed;
    }
    // accelerator_pct: même offset que speed_kmh, à utiliser si speed_kmh est invalide
    if (speed < 0.0f || speed > 300.0f) {
      out.accelerator_pct = speed;  // Probablement accelerator_pct si hors plage vitesse
    }
  }
  if (a0Len > 9) {
    // air_temp_c (offset 8, scale 0.25)
    uint16_t raw_air_temp = (a0[8] << 8) | a0[9];
    out.air_temp_c = (float)raw_air_temp * 0.25f;
  }
  if (a0Len > 19) {
    // abs_pressure_mbar (offset 18, scale 1.0)
    uint16_t raw_abs_press = (a0[18] << 8) | a0[19];
    out.abs_pressure_mbar = (float)raw_abs_press;
  }
  if (a0Len > 21) {
    // air_flow_estimate_mgcp (offset 20, scale 0.1)
    uint16_t raw_air_flow_est = (a0[20] << 8) | a0[21];
    out.air_flow_estimate_mgcp = (float)raw_air_flow_est * 0.1f;
  }
  if (a0Len > 23) {
    // air_flow_request_mgcp (offset 22, scale 1.0)
    uint16_t raw_air_flow_req = (a0[22] << 8) | a0[23];
    out.air_flow_request_mgcp = (float)raw_air_flow_req;
    // battery_v (offset 22, scale 0.05) - même offset, utiliser pour battery_v si air_flow_request semble invalide
    // Note: Conflit d'offset, à affiner
    float battery_candidate = (float)raw_air_flow_req * 0.05f;
    if (battery_candidate >= 10.0f && battery_candidate <= 15.0f) {
      out.battery_v = battery_candidate;
    }
  }
  if (a0Len > 27) {
    // rail_pressure_bar (offset 26, scale 0.1)
    // Note: fuel_temp_c pourrait aussi être ici (offset 26, scale 1.0), conflit à résoudre
    uint16_t raw_rail_press = (a0[26] << 8) | a0[27];
    out.rail_pressure_bar = (float)raw_rail_press * 0.1f;
    // fuel_temp_c candidat (offset 26, scale 1.0)
    float fuel_temp_candidate = (float)raw_rail_press * 1.0f;
    if (fuel_temp_candidate >= 20.0f && fuel_temp_candidate <= 50.0f) {
      out.fuel_temp_c = fuel_temp_candidate;
    }
  }
  if (a0Len > 29) {
    // rail_pressure_control_bar (offset 28, scale 0.1)
    uint16_t raw_rail_press_ctrl = (a0[28] << 8) | a0[29];
    out.rail_pressure_control_bar = (float)raw_rail_press_ctrl * 0.1f;
  }
  if (a0Len > 5) {
    // desired_idle_speed_rpm (offset 4, scale 0.25)
    uint16_t raw_idle_rpm = (a0[4] << 8) | a0[5];
    out.desired_idle_speed_rpm = (float)raw_idle_rpm * 0.25f;
  }
  if (a0Len > 13) {
    // engine_rpm (offset 12, scale 0.25) - approximation, à affiner
    uint16_t raw_rpm = (a0[12] << 8) | a0[13];
    float rpm = (float)raw_rpm * 0.25f;
    if (rpm >= 0.0f && rpm <= 10000.0f) {  // Filtrer valeurs aberrantes
      out.engine_rpm = rpm;
    }
  }
  if (a0Len > 35) {
    uint16_t raw_req_press = (a0[34] << 8) | a0[35];
    out.requested_in_pressure_mbar = (float)raw_req_press;
  }
  if (a0Len > 45) {
    // gear_ratio (offset 44, scale 1.0)
    uint16_t raw_gear = (a0[44] << 8) | a0[45];
    out.gear_ratio = (float)raw_gear;
  }
  
  // Page 21A2
  if (a2Len > 3) {
    uint16_t raw_bar_mmhg = (a2[2] << 8) | a2[3];
    out.bar_pressure_mmhg = (float)raw_bar_mmhg * 5.0f;
  }
  if (a2Len > 5) {
    // desired_egr_position_pct (offset 4, scale 100.0)
    // Note: Les valeurs OCR montrent 0-6403%, donc scale 100.0 semble correct
    // mais peut-être que c'est en centièmes de pourcent (64.03% au lieu de 6403%)
    uint16_t raw_egr_desired = (a2[4] << 8) | a2[5];
    float egr_pct = (float)raw_egr_desired * 100.0f;
    // Si > 100%, c'est probablement en centièmes, diviser par 100
    if (egr_pct > 100.0f) {
      out.desired_egr_position_pct = egr_pct / 100.0f;
    } else {
      out.desired_egr_position_pct = egr_pct;
    }
  }
  if (a2Len > 13) {
    // egr_position_pct (offset 12, scale 0.25)
    uint16_t raw_egr = (a2[12] << 8) | a2[13];
    out.egr_position_pct = (float)raw_egr * 0.25f;
  }
  if (a2Len > 21) {
    // engine_temp_c (offset 20, scale 0.01)
    uint16_t raw_engine_temp = (a2[20] << 8) | a2[21];
    out.engine_temp_c = (float)raw_engine_temp * 0.01f;
  }
  if (a2Len > 23) {
    uint16_t raw_bar_kpa = (a2[22] << 8) | a2[23];
    out.bar_pressure_kpa = (float)raw_bar_kpa / 10.0f;
  }
  
  // Page 21A5
  if (a5Len > 5) {
    // egr_position_pct (offset 4, scale 0.25) - alternative à 21A2
    // Note: 21A5 offset 4 semble plus précis selon l'analyse
    uint16_t raw_egr = (a5[4] << 8) | a5[5];
    out.egr_position_pct = (float)raw_egr * 0.25f;
  }
  // Note: battery_v et fuel_temp_c ont été déplacés vers 21A0 pour résoudre les conflits
  // Anciens offsets (21A5) étaient incorrects
  
  // Page 21CD
  // Note: requested_in_pressure_mbar pourrait aussi être ici (offset 2, scale 4.0)
  // mais 21A0 offset 34 semble plus fiable
  
  // Champs non encore identifiés avec certitude:
  // - engine_rpm: plusieurs candidats (offset 12 ou 28 dans 21A0, à vérifier)
  // - intake_c: valeur constante -50.0 dans les données, difficile à valider
}

// --- JSON publish ---
static void publishSzJson(const SzData& d, const String& rawA0, const String& rawA2, const String& rawA5, const String& rawCD) {
  // Appeler loop() pour maintenir la connexion MQTT
  mqtt.loop();
  
  if (!mqttEnsureConnected()) {
    Serial.println("[PUB] ERREUR: MQTT non connecté");
    return;
  }

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
  
  // Vérifier l'état de la connexion avant publication
  if (!mqtt.connected()) {
    Serial.printf("[PUB] ERREUR: MQTT déconnecté avant publication\n");
    if (!mqttEnsureConnected()) {
      Serial.println("[PUB] ERREUR: Impossible de reconnecter MQTT");
      return;
    }
  }
  
  // Vérifier la mémoire avant publication
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 10000) {
    Serial.printf("[PUB] ATTENTION: Mémoire libre très faible (%u bytes) avant publication\n", freeHeap);
  }
  
  // Utiliser la signature avec uint8_t* et longueur (plus fiable)
  bool pubOk = mqtt.publish(topic_sz, (uint8_t*)payload, n, false); // QoS 0
  if (pubOk) {
    Serial.printf("[PUB] OK: %s (%d bytes) - mémoire libre: %u bytes\n", topic_sz, n, ESP.getFreeHeap());
    // Appeler loop() après publication pour envoyer le message
    mqtt.loop();
  } else {
    Serial.printf("[PUB] FAIL: %s (%d bytes) - connected=%d, state=%d, mémoire=%u bytes\n", 
                  topic_sz, n, mqtt.connected(), mqtt.state(), ESP.getFreeHeap());
    // Tenter de reconnecter si déconnecté
    if (!mqtt.connected()) {
      Serial.println("[PUB] MQTT déconnecté, tentative de reconnexion...");
      mqttEnsureConnected();
    }
  }

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
      Serial.printf("[ELM] ATFI réussi après %d tentative(s)\n", i + 1);
      atfiOk = true;
      break;
    }
    Serial.printf("[ELM] ATFI échoué (tentative %d/%d): %s\n", i + 1, maxRetries, fi.c_str());
    if (i < maxRetries - 1) {
      delay(500); // Petit délai avant de réessayer
      doConfigSequence(); // Refaire la séquence de configuration
    }
  }
  
  if (!atfiOk) {
    Serial.println("[ELM] ERREUR: ATFI a échoué après toutes les tentatives");
    Serial.println("[ELM] Vérifiez que le contact est allumé et que le vLinker est bien connecté");
    return false;
  }
  
  // ATKW (optionnel; dans le log: "1:D0 2:8F")
  elmQuery("ATKW");
  return true;
}

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
    Serial.println("[DTC] ERREUR: MQTT non connecté");
    return false;
  }
  
  Serial.println("[DTC] Lecture des DTC...");
  String response = elmQuery("03", 2000);
  
  if (response.indexOf("NO DATA") >= 0 || response.indexOf("ERROR") >= 0) {
    Serial.println("[DTC] Pas de DTC ou erreur de lecture");
    // Publier un JSON vide pour indiquer qu'il n'y a pas de DTC
    StaticJsonDocument<256> doc;
    doc["app"] = app_name;
    doc["ver"] = version;
    doc["ts_ms"] = (uint32_t)millis();
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
    Serial.printf("[DTC] OK: %d DTC(s) publié(s) - %s\n", dtcCount, topic_dtc);
    mqtt.loop();
    return true;
  } else {
    Serial.printf("[DTC] FAIL: Publication échouée\n");
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
    Serial.println("[ELM] NO DATA détecté → contact coupé, réinitialisation ELM...");
    return false;  // Indique qu'il faut réinitialiser
  }

  // Convert to bytes for decoding
  uint8_t bA0[256], bA2[256], bA5[256], bCD[64];
  size_t nA0 = hexToBytes(rA0, bA0, sizeof(bA0));
  size_t nA2 = hexToBytes(rA2, bA2, sizeof(bA2));
  size_t nA5 = hexToBytes(rA5, bA5, sizeof(bA5));
  size_t nCD = hexToBytes(rCD, bCD, sizeof(bCD));

  SzData d;
  decodeSzFromPages(bA0, nA0, bA2, nA2, bA5, nA5, bCD, nCD, d);

  // Ajouter au buffer FIFO (priorité: collecte continue)
  if (fifoPush(d, rA0, rA2, rA5, rCD)) {
    Serial.printf("[FIFO] Trame ajoutée (%d/%d)\n", fifoCount, FIFO_BUFFER_SIZE);
  } else {
    Serial.printf("[FIFO] Buffer plein, trame perdue\n");
  }
  
  return true;
}

// Envoi MQTT depuis le buffer FIFO (si WiFi OK)
static void fifoSendToMqtt() {
  static unsigned long lastDebugMs = 0;
  
  if (!mqttEnsureConnected()) {
    if (millis() - lastDebugMs > 5000) {
      Serial.printf("[FIFO] DEBUG - MQTT non connecté, buffer: %d/%d trames\n", fifoCount, FIFO_BUFFER_SIZE);
      lastDebugMs = millis();
    }
    return;  // Pas de WiFi/MQTT, on garde les trames dans le buffer
  }
  
  if (fifoCount == 0) {
    if (millis() - lastDebugMs > 10000) {
      Serial.println("[FIFO] DEBUG - Buffer vide (pas de données collectées)");
      lastDebugMs = millis();
    }
    return;
  }
  
  Serial.printf("[FIFO] DEBUG - Envoi de %d trame(s) depuis le buffer\n", (fifoCount < 5) ? fifoCount : 5);
  
  // Envoyer jusqu'à 5 trames par cycle pour ne pas surcharger
  int sent = 0;
  while (sent < 5 && fifoCount > 0) {
    SzData d;
    String rawA0, rawA2, rawA5, rawCD;
    uint32_t timestamp_ms;
    
    if (fifoPop(d, rawA0, rawA2, rawA5, rawCD, timestamp_ms)) {
      publishSzJson(d, rawA0, rawA2, rawA5, rawCD);
      sent++;
      Serial.printf("[FIFO] Trame envoyée (%d restantes)\n", fifoCount);
    } else {
      break;
    }
  }
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
  Serial.println("[LED] LEDs initialisées");
  Serial.println("[LED] NOTE: Pin 34 est INPUT ONLY sur ESP32");
  Serial.println("[LED] Utilisation d'une seule LED (orange) pour WiFi et BLE");
  Serial.println("[LED] Les patterns seront combinés selon l'état");
  
  // Test de la LED orange au démarrage
  Serial.println("[LED] Test LED orange (WiFi/BLE combiné)...");
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_WIFI_PIN, HIGH);
    delay(200);
    digitalWrite(LED_WIFI_PIN, LOW);
    delay(200);
  }
  Serial.println("[LED] Test LED terminé");
}

void loop() {
  wifiTick();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = mqtt.connected();

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
          Serial.println("[MQTT] Connexion perdue, tentative de reconnexion...");
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

  switch (currentState) {
    case BOOT:
      currentState = CONNECT_WIFI;
      Serial.println("[FSM] État: CONNECT_WIFI");
      break;

    case CONNECT_WIFI:
      if (wifiOk) {
        Serial.printf("[FSM] WiFi connecté à: %s (IP: %s)\n", 
                      WiFi.SSID().c_str(), 
                      WiFi.localIP().toString().c_str());
        currentState = CONNECT_MQTT;
        Serial.println("[FSM] État: CONNECT_MQTT");
      }
      break;

    case CONNECT_MQTT:
      if (mqttOk) {
        Serial.println("[FSM] MQTT connecté");
        currentState = CONNECT_BLE;
        Serial.println("[FSM] État: CONNECT_BLE");
      }
      break;

    case CONNECT_BLE:
      if (millis() - lastBleAttemptMs > 5000) {
        lastBleAttemptMs = millis();
        if (!vLinkerAddressValid) {
          Serial.println("[FSM] Scan BLE pour vLinker…");
          // Lister les 5 meilleurs devices BLE
          bleScanAndListTop5();
        } else {
          Serial.printf("[FSM] Tentative de connexion BLE à %s…\n", vLinkerAddress.toString().c_str());
        }
        if (bleConnectVlinker()) {
          bleConnected = true;
          Serial.println("[FSM] BLE OK (write+notify trouvés)");
          if (mqttOk) {
          mqttPublishStatus("BLE OK");
          }
          currentState = INIT_ELM;
        } else {
          bleConnected = false;
          Serial.println("[FSM] BLE KO (retry dans 5s)");
          // Réinitialiser le flag pour forcer un nouveau scan au prochain essai
          if (!vLinkerAddressValid) {
            delay(100); // Petit délai avant de réessayer
          }
        }
      }
      break;

    case INIT_ELM:
      if (!bleClient || !bleClient->isConnected()) {
        Serial.println("[FSM] INIT_ELM: BLE déconnecté, retour à CONNECT_BLE");
        bleConnected = false;
        vLinkerAddressValid = false; // Force un nouveau scan
        if (bleClient) {
          bleClient->disconnect();
          delay(100);
        }
        lastBleAttemptMs = millis(); // Réinitialiser le timer
        currentState = CONNECT_BLE;
        break;
      }
      Serial.println("[FSM] Init ELM like SZ Viewer…");
      if (elmInitLikeSzViewer()) {
        if (mqttOk) {
          mqttPublishStatus("ELM init done");
        }
        currentState = POLL_SZ;
      } else {
        Serial.println("[FSM] Échec init ELM, réessai dans 5s...");
        delay(5000);
        // Reste en INIT_ELM pour réessayer
      }
      break;

    case POLL_SZ:
      if (!bleClient || !bleClient->isConnected()) {
        Serial.println("[FSM] POLL_SZ: BLE déconnecté, retour à CONNECT_BLE");
        bleConnected = false;
        vLinkerAddressValid = false; // Force un nouveau scan
        if (bleClient) {
          bleClient->disconnect();
          delay(100);
        }
        lastBleAttemptMs = millis(); // Réinitialiser le timer
        currentState = CONNECT_BLE;
        break;
      }
      if (millis() - lastPollMs > 500) { // à ajuster selon charge bus
        lastPollMs = millis();
        // Collecte continue (priorité: toujours collecter, même sans WiFi)
        if (!pollSzPagesAndCollect()) {
          // NO DATA détecté → réinitialiser ELM
          Serial.println("[FSM] NO DATA → retour à INIT_ELM");
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

