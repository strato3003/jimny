/*
 * Test LED Rouge - XIAO ESP32S3
 * Teste plusieurs pins et logiques pour trouver la bonne configuration
 */

// Pins possibles pour la LED rouge
int testPins[] = {34, 38, 35, 36, 46};
int numPins = sizeof(testPins) / sizeof(testPins[0]);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  
  Serial.println("\n========================================");
  Serial.println("  TEST LED ROUGE - MULTI PINS");
  Serial.println("========================================");
  Serial.println("Teste les pins: 34, 38, 35, 36, 46");
  Serial.println("Avec logique normale ET inversée");
  Serial.println("========================================\n");
}

void testPin(int pin, bool inverted) {
  pinMode(pin, OUTPUT);
  
  const char* logic = inverted ? "INVERSÉE (LOW=ON)" : "NORMALE (HIGH=ON)";
  Serial.printf("\n[TEST] Pin %d - Logique %s\n", pin, logic);
  
  // Test 1: Allumer
  if (inverted) {
    digitalWrite(pin, LOW);
  } else {
    digitalWrite(pin, HIGH);
  }
  Serial.printf("  → État: ON (attendu)\n");
  delay(1000);
  
  // Test 2: Éteindre
  if (inverted) {
    digitalWrite(pin, HIGH);
  } else {
    digitalWrite(pin, LOW);
  }
  Serial.printf("  → État: OFF (attendu)\n");
  delay(1000);
  
  // Remettre à OFF
  if (inverted) {
    digitalWrite(pin, HIGH);
  } else {
    digitalWrite(pin, LOW);
  }
}

void loop() {
  Serial.println("\n=== CYCLE DE TEST ===\n");
  
  // Tester chaque pin avec les deux logiques
  for (int i = 0; i < numPins; i++) {
    int pin = testPins[i];
    
    // Test avec logique normale
    testPin(pin, false);
    delay(500);
    
    // Test avec logique inversée
    testPin(pin, true);
    delay(500);
  }
  
  Serial.println("\n=== FIN DU CYCLE ===\n");
  Serial.println("Observez quelle LED s'allume et notez le pin + logique\n");
  delay(3000);
}
