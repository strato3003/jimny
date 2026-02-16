/*
 * Test LED Rouge (charge) - XIAO ESP32S3
 * Pin 34, logique inversée (LOW = ON, HIGH = OFF)
 */

#define LED_BLE_PIN 34  // LED rouge (charge) - Pin 34, logique inversée

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  
  Serial.println("\n========================================");
  Serial.println("  TEST LED ROUGE (Pin 34)");
  Serial.println("========================================");
  
  pinMode(LED_BLE_PIN, OUTPUT);
  digitalWrite(LED_BLE_PIN, HIGH);  // Éteindre au démarrage (logique inversée)
  
  Serial.println("[LED] LED rouge initialisée (pin 34, logique inversée)");
  Serial.println("[LED] Test: clignotement toutes les 500ms");
  Serial.println("[LED] LOW = ON, HIGH = OFF");
}

void loop() {
  // Allumer la LED (LOW = ON avec logique inversée)
  Serial.println("[LED] ON (LOW)");
  digitalWrite(LED_BLE_PIN, LOW);
  delay(500);
  
  // Éteindre la LED (HIGH = OFF avec logique inversée)
  Serial.println("[LED] OFF (HIGH)");
  digitalWrite(LED_BLE_PIN, HIGH);
  delay(500);
}
