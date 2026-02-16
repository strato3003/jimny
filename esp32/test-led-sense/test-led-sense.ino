/*
 * Test LEDs - XIAO ESP32S3 SENSE
 * Teste tous les pins possibles pour identifier les LEDs disponibles
 */

// Pins à tester (excluant les INPUT ONLY: 34, 35, 36, 39)
// Et les pins utilisés par la caméra sur le SENSE
int testPins[] = {
  21,  // LED_BUILTIN (LED utilisateur orange)
  // Pins GPIO disponibles (éviter ceux utilisés par la caméra)
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
  22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
  37, 38, 40, 41, 42, 43, 44, 45, 46, 47, 48
};
int numPins = sizeof(testPins) / sizeof(testPins[0]);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  
  Serial.println("\n========================================");
  Serial.println("  TEST LEDs - XIAO ESP32S3 SENSE");
  Serial.println("========================================");
  Serial.printf("Test de %d pins (logique normale et inversée)\n", numPins);
  Serial.println("Observez quelle(s) LED(s) s'allume(nt)");
  Serial.println("========================================\n");
  delay(300);
}

void testPin(int pin, bool inverted) {
  // Essayer de configurer le pin en OUTPUT
  // Certains pins peuvent ne pas être configurables
  pinMode(pin, OUTPUT);
  
  const char* logic = inverted ? "INVERSÉE (LOW=ON)" : "NORMALE (HIGH=ON)";
  Serial.printf("[TEST] Pin %d - Logique %s\n", pin, logic);
  
  // Test 1: Allumer
  if (inverted) {
    digitalWrite(pin, LOW);
  } else {
    digitalWrite(pin, HIGH);
  }
  Serial.printf("  → État: ON (attendu) - OBSERVEZ LA LED\n");
  delay(300);  // 2 secondes pour bien observer
  
  // Test 2: Éteindre
  if (inverted) {
    digitalWrite(pin, HIGH);
  } else {
    digitalWrite(pin, LOW);
  }
  Serial.printf("  → État: OFF (attendu)\n");
  delay(150);
  
  // Remettre à OFF
  if (inverted) {
    digitalWrite(pin, HIGH);
  } else {
    digitalWrite(pin, LOW);
  }
}

void loop() {
  Serial.println("\n=== DÉBUT DU TEST ===\n");
  Serial.println("Le test va parcourir tous les pins.");
  Serial.println("NOTEZ le(s) pin(s) qui font s'allumer une LED !\n");
  delay(3000);
  
  // Tester chaque pin avec les deux logiques
  for (int i = 0; i < numPins; i++) {
    int pin = testPins[i];
    
    // Test avec logique normale
    testPin(pin, false);
    delay(50);
    
    // Test avec logique inversée
    testPin(pin, true);
    delay(50);
    
    Serial.println("---\n");
  }
  
  Serial.println("\n=== FIN DU TEST ===\n");
  Serial.println("Si aucune LED rouge ne s'est allumée, il n'y a probablement qu'une seule LED (orange) sur cette board.");
  Serial.println("Dans ce cas, on utilisera cette LED unique avec des patterns combinés.\n");
  delay(5000);
}
