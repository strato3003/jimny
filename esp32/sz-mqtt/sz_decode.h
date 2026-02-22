// Décodage des pages OBD (21A0, 21A2, 21A5, 21CD) → SzData
// Fichier à part pour modifier offsets/échelles sans retoucher au reste du sketch.
//
// KWP2000: la réponse à une requête utilise SID_requête + 0x40.
// Ex.: requête 21 A0 → réponse 61 A0 (0x21+0x40=0x61). Les buffers a0/a2/a5/cd
// passés ici sont la trame brute: octets 0-1 = en-tête réponse (61 A0, 61 A2, …),
// le payload suit à partir de l'octet 2.
#ifndef SZ_DECODE_H
#define SZ_DECODE_H

#include <Arduino.h>

// En-tête KWP2000: premier octet = SID requête + 0x40
#define KWP2000_RESPONSE_SID_OFFSET 0x40
// Pour vérifier qu'on a bien une réponse 21A0 / 21A2 / 21A5 / 21CD
#define SZ_A0_RESPONSE_HI  0x61
#define SZ_A0_RESPONSE_LO  0xA0
#define SZ_A2_RESPONSE_LO  0xA2
#define SZ_A5_RESPONSE_LO  0xA5
#define SZ_CD_RESPONSE_LO  0xCD
#define SZ_PAYLOAD_OFFSET  2

// --- Structure de données SZ Viewer (20 champs) ---
struct SzData {
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

// Convertit une chaîne hex ASCII (ex: "61A0FF...") en bytes
static inline size_t szDecodeHexToBytes(const String& hex, uint8_t* out, size_t outMax) {
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

// Décodage des 20 champs depuis les pages hex (21A0, 21A2, 21A5, 21CD)
// Modifier uniquement ce fichier pour ajuster offsets/échelles.
static inline void decodeSzFromPages(
  const uint8_t* a0, size_t a0Len,
  const uint8_t* a2, size_t a2Len,
  const uint8_t* a5, size_t a5Len,
  const uint8_t* cd, size_t cdLen,
  SzData& out
) {
// Généré par tools/sz_decode_from_ocr_jsonl.py à partir de sz_sync_ocr.jsonl
// Coller le contenu de decodeSzFromPages (remplacer l’existant) ou appliquer manuellement.

  // desired_idle_speed_rpm (mae=7.71 n=510)
  if (a0Len > 45) {
    uint16_t raw = (a0[44] << 8) | a0[45];
    out.desired_idle_speed_rpm = (float)raw * 0.06220588595295306f + 844.9560488396442f;
  }

  // accelerator_pct (mae=2.61 n=510)
  if (a2Len > 5) {
    uint16_t raw = (a2[4] << 8) | a2[5];
    out.accelerator_pct = (float)raw * 0.26064220780047914f + -16.498404044868188f;
  }

  // intake_c (mae=0.00 n=510)
  if (a0Len > 5) {
    uint16_t raw = (a0[4] << 8) | a0[5];
    out.intake_c = (float)raw * 0.0f + -50.0f;
  }

  // battery_v (mae=0.03 n=510)
  if (a2Len > 27) {
    uint16_t raw = (a2[26] << 8) | a2[27];
    out.battery_v = (float)raw * 0.040871480833465604f + -110.9969629393979f;
  }

  // fuel_temp_c (mae=0.01 n=510)
  if (a0Len > 41) {
    uint16_t raw = (a0[40] << 8) | a0[41];
    out.fuel_temp_c = (float)raw * -0.049803335431089446f + 35.89450912523603f;
  }

  // bar_pressure_kpa (mae=0.00 n=510)
  if (a0Len > 7) {
    uint16_t raw = (a0[6] << 8) | a0[7];
    out.bar_pressure_kpa = (float)raw * 0.0f + 102.5f;
  }

  // bar_pressure_mmhg (mae=0.00 n=510)
  if (a0Len > 13) {
    uint16_t raw = (a0[12] << 8) | a0[13];
    out.bar_pressure_mmhg = (float)raw * 0.0f + 768.813f;
  }

  // abs_pressure_mbar (mae=2.87 n=510)
  if (a0Len > 19) {
    uint16_t raw = (a0[18] << 8) | a0[19];
    out.abs_pressure_mbar = (float)raw;
  }

  // air_flow_estimate_mgcp (mae=5.32 n=509)
  if (a0Len > 21) {
    uint16_t raw = (a0[20] << 8) | a0[21];
    out.air_flow_estimate_mgcp = (float)raw / 10.0f;
  }

  // speed_kmh (mae=0.78 n=510)
  if (a0Len > 25) {
    uint16_t raw = (a0[24] << 8) | a0[25];
    out.speed_kmh = (float)raw * 0.0076758257804214565f + 0.5328163584628314f;
  }

  // rail_pressure_bar (mae=18.99 n=510)
  if (a0Len > 27) {
    uint16_t raw = (a0[26] << 8) | a0[27];
    out.rail_pressure_bar = (float)raw / 10.0f;
  }

  // rail_pressure_control_bar (mae=0.01 n=400)
  if (a0Len > 15) {
    uint16_t raw = (a0[14] << 8) | a0[15];
    out.rail_pressure_control_bar = (float)raw / 1000.0f;
  }

  // desired_egr_position_pct (mae=0.01 n=510)
  if (a5Len > 7) {
    uint16_t raw = (a5[6] << 8) | a5[7];
    out.desired_egr_position_pct = (float)raw * -4.2779983956687014e-05f + 35.34682533998348f;
  }

  // egr_position_pct (mae=0.83 n=510)
  if (a0Len > 37) {
    uint16_t raw = (a0[36] << 8) | a0[37];
    out.egr_position_pct = (float)raw * 0.09544619466159861f + 26.493944429533997f;
  }

  // engine_temp_c (mae=0.88 n=510)
  if (a2Len > 25) {
    uint16_t raw = (a2[24] << 8) | a2[25];
    out.engine_temp_c = (float)raw * 0.07335171995784256f + -183.22408684236524f;
  }

  // air_temp_c (mae=0.04 n=510)
  if (a2Len > 21) {
    uint16_t raw = (a2[20] << 8) | a2[21];
    out.air_temp_c = (float)raw * 0.09561560498782616f + -260.3825049781947f;
  }

  // engine_rpm (mae=inf n=0)
  if (a2Len > 13) {
    uint16_t raw = (a2[12] << 8) | a2[13];
    out.engine_rpm = (float)raw * 8.0f;
  }

  }














#endif // SZ_DECODE_H
