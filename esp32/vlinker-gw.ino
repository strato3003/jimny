#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <BLEDevice.h>

// --- Configuration ---
const char* app_name = "vLinker Gateway";
const char* version = "1.0.3";
const char* author = "F4IAE";

WiFiMulti wifiMulti;
const char* mqtt_server = "srv.lpb.ovh";
const char* vLinker_MAC = "04:47:07:6A:AD:87";

// --- FSM States ---
enum SystemState { BOOT, CONNECT_WIFI, SEARCH_VLINKER, INIT_OBD, READ_DATA };
SystemState currentState = BOOT;

// --- Globals ---
WiFiClient espClient;
PubSubClient client(espClient);
BLEClient* pClient = nullptr;
bool connectedBT = false;
unsigned long lastTick = 0;
unsigned long lastBLEAttempt = 0;

void setup() {
    Serial.begin(115200);
    while(!Serial && millis() < 3000);

    Serial.println("\n========================================");
    Serial.printf("  APP: %s\n", app_name);
    Serial.printf("  VER: %s\n", version);
    Serial.printf("  BY : %s\n", author);
    Serial.println("========================================");

    // --- AJOUT MULTI-WIFI ---
    wifiMulti.addAP("LPB", "lapet1teb0rde");          // Réseau Maison
    wifiMulti.addAP("<_JNoel iPhone_>", "bachibouzook"); // Ton iPhone
    
    Serial.println("[SYSTEM] WiFi Multi-AP configuré");

    setCpuFrequencyMhz(160);
    BLEDevice::init("CW-S3-F4IAE");
    pClient = BLEDevice::createClient();
    
    client.setServer(mqtt_server, 1883);
}

void loop() {
    // Maintenance WiFi Multi
    if (wifiMulti.run() == WL_CONNECTED) {
        if (!client.connected()) {
            if (client.connect("Jimny_S3", "van", ".V@n")) {
                Serial.printf("[WIFI] Connecté à : %s\n", WiFi.SSID().c_str());
                client.publish("jimny/status", "Gateway Multi-WiFi Ready");
            }
        }
        client.loop();
    }

    // MACHINE A ETATS (FSM)
    switch (currentState) {
        case BOOT:
            if (WiFi.status() == WL_CONNECTED) {
                currentState = CONNECT_WIFI;
            }
            break;

        case CONNECT_WIFI:
            if (client.connected()) {
                currentState = SEARCH_VLINKER;
            }
            break;

        case SEARCH_VLINKER:
            if (millis() - lastBLEAttempt > 10000) {
                lastBLEAttempt = millis();
                Serial.println("[FSM] Scan vLinker...");
                if (pClient->connect(BLEAddress(vLinker_MAC))) {
                    Serial.println("[FSM] vLinker Connecté !");
                    connectedBT = true;
                    currentState = INIT_OBD;
                }
            }
            break;

        case INIT_OBD:
            // Séquence AT...
            currentState = READ_DATA;
            break;

        case READ_DATA:
            if (millis() - lastTick > 5000) {
                lastTick = millis();
                if (client.connected()) {
                    char msg[64];
                    sprintf(msg, "IP: %s | SSID: %s | RSSI: %d", 
                            WiFi.localIP().toString().c_str(), 
                            WiFi.SSID().c_str(), 
                            WiFi.RSSI());
                    client.publish("jimny/heartbeat", msg);
                }
            }
            
            if (!pClient->isConnected()) {
                connectedBT = false;
                currentState = SEARCH_VLINKER;
            }
            break;
    }
}
