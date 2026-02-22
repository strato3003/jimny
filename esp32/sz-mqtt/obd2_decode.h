// Décodage OBD2 (mode 01) depuis réponses KWP vLinker → SzData
// Réponses type 83F17A41XX... ou 84F17A41XX... (41 = mode 01, XX = PID, puis données)
// Formules SAE J1979. Inclure après sz_decode.h pour avoir SzData.

#ifndef OBD2_DECODE_H
#define OBD2_DECODE_H

#include <Arduino.h>

// Retourne l'index dans hex (chaîne sans espaces, majuscules) de "41" + pidHex (ex. "410C"), ou -1
static inline int obd2FindPidInHex(const String& hex, const char* pidHex) {
  String needle = String("41") + pidHex;
  needle.toUpperCase();
  return hex.indexOf(needle);
}

// Extrait 1 octet de données après "41" + pidHex (2 caractères hex). Retourne true si OK.
static inline bool obd2ParseByte1(const String& resp, const char* pidHex, uint8_t& outByte) {
  String s = resp;
  s.replace(" ", "");
  s.replace("\r", "");
  s.replace("\n", "");
  s.toUpperCase();
  String hex;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex += c;
  }
  int idx = obd2FindPidInHex(hex, pidHex);
  if (idx < 0) return false;
  int dataStart = idx + 4;  // après "41XX"
  if (dataStart + 2 > (int)hex.length()) return false;
  outByte = (uint8_t)strtoul(hex.substring(dataStart, dataStart + 2).c_str(), nullptr, 16);
  return true;
}

// Extrait 2 octets (A, B) après "41" + pidHex. Retourne true si OK.
static inline bool obd2ParseByte2(const String& resp, const char* pidHex, uint8_t& outA, uint8_t& outB) {
  String s = resp;
  s.replace(" ", "");
  s.replace("\r", "");
  s.replace("\n", "");
  s.toUpperCase();
  String hex;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex += c;
  }
  int idx = obd2FindPidInHex(hex, pidHex);
  if (idx < 0) return false;
  int dataStart = idx + 4;
  if (dataStart + 4 > (int)hex.length()) return false;
  outA = (uint8_t)strtoul(hex.substring(dataStart, dataStart + 2).c_str(), nullptr, 16);
  outB = (uint8_t)strtoul(hex.substring(dataStart + 2, dataStart + 4).c_str(), nullptr, 16);
  return true;
}

// Extrait 4 octets (A,B,C,D) après "41" + pidHex pour PIDs 4 octets de données
static inline bool obd2ParseByte4(const String& resp, const char* pidHex, uint8_t& outA, uint8_t& outB, uint8_t& outC, uint8_t& outD) {
  String s = resp;
  s.replace(" ", "");
  s.replace("\r", "");
  s.replace("\n", "");
  s.toUpperCase();
  String hex;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex += c;
  }
  int idx = obd2FindPidInHex(hex, pidHex);
  if (idx < 0) return false;
  int dataStart = idx + 4;
  if (dataStart + 8 > (int)hex.length()) return false;
  outA = (uint8_t)strtoul(hex.substring(dataStart, dataStart + 2).c_str(), nullptr, 16);
  outB = (uint8_t)strtoul(hex.substring(dataStart + 2, dataStart + 4).c_str(), nullptr, 16);
  outC = (uint8_t)strtoul(hex.substring(dataStart + 4, dataStart + 6).c_str(), nullptr, 16);
  outD = (uint8_t)strtoul(hex.substring(dataStart + 6, dataStart + 8).c_str(), nullptr, 16);
  return true;
}

// Parse ATRV "12.3V" → 12.3f
static inline bool obd2ParseATRV(const String& resp, float& outVolts) {
  String s = resp;
  s.trim();
  int i = 0;
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) i++;
  int start = i;
  while (i < (int)s.length() && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) i++;
  if (i <= start) return false;
  String num = s.substring(start, i);
  outVolts = num.toFloat();
  return outVolts > 0.0f && outVolts < 20.0f;
}

// Remplit SzData à partir des réponses OBD2 (map: clé = "010C", valeur = réponse brute)
// responses: tableau de paires (pid, response). Les champs non fournis restent NAN.
static inline void decodeObd2ToSzData(
  const char* pid0104, const char* pid0105, const char* pid010B,
  const char* pid010C, const char* pid010D, const char* pid0110, const char* pid0111,
  const char* pid0123, const char* atrv,
  SzData& out
) {
  uint8_t a, b;

  // 0104 Engine load % (pas de champ SZ dédié, ignoré ou à mapper)
  (void)pid0104;
  // 0105 Coolant temp °C : A - 40
  if (pid0105 && obd2ParseByte1(pid0105, "05", a))
    out.engine_temp_c = (float)((int)a - 40);
  // 010B Intake manifold absolute pressure kPa
  if (pid010B && obd2ParseByte1(pid010B, "0B", a)) {
    out.bar_pressure_kpa = (float)a;
    out.requested_in_pressure_mbar = (float)a * 10.0f;
  }
  // 010C Engine RPM : (A*256+B)/4
  if (pid010C && obd2ParseByte2(pid010C, "0C", a, b))
    out.engine_rpm = (float)((a * 256u) + b) / 4.0f;
  // 010D Vehicle speed km/h : A
  if (pid010D && obd2ParseByte1(pid010D, "0D", a))
    out.speed_kmh = (float)a;
  // 0110 MAF g/s : (A*256+B)/100
  if (pid0110 && obd2ParseByte2(pid0110, "10", a, b))
    out.air_flow_estimate_mgcp = (float)((a * 256u) + b) / 100.0f;  // g/s, même unité approchée
  // 0111 Throttle % : A*100/255
  if (pid0111 && obd2ParseByte1(pid0111, "11", a))
    out.accelerator_pct = (float)a * 100.0f / 255.0f;
  // 0123 Fuel rail pressure (A*256+B)*10 kPa → bar = /100
  if (pid0123 && obd2ParseByte2(pid0123, "23", a, b))
    out.rail_pressure_bar = (float)((a * 256u) + b) * 10.0f / 100.0f;
  // ATRV battery V
  if (atrv && obd2ParseATRV(atrv, out.battery_v)) {}
}

#endif
