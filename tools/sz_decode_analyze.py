#!/usr/bin/env python3
"""
Analyse les données OCR synchronisées pour identifier les offsets et échelles
des 20 champs SZ Viewer dans les pages hexadécimales 21A0, 21A2, 21A5, 21CD.
"""

import json
import sys
from typing import Dict, List, Optional, Tuple

def hex_ascii_to_bytes(hex_ascii: str) -> bytes:
    """Convertit une chaîne hex ASCII (ex: "36314130...") en bytes."""
    # Enlève les préfixes/suffixes comme "0D0D3E" (CR CR >)
    hex_clean = hex_ascii.strip()
    # Convertit chaque paire de caractères ASCII en byte
    result = bytes()
    i = 0
    while i < len(hex_clean):
        if i + 1 < len(hex_clean):
            # Deux caractères ASCII représentent un byte hex
            # Ex: "36" (ASCII) = 0x36 = '6', "31" = 0x31 = '1'
            # Mais en fait, "3631" représente les caractères ASCII '6' et '1'
            # qui forment "61" en hex réel
            # Donc on doit décoder différemment...
            # En fait, "36314130" = "61A0" en ASCII, donc on doit extraire les paires
            # qui représentent des caractères hex valides
            pass
        i += 2
    
    # Approche plus simple: chercher les patterns hex valides
    # Les données semblent être en format ASCII hex, donc "36314130" = "61A0"
    # On cherche les séquences qui ressemblent à du hex OBD
    hex_str = ""
    for i in range(0, len(hex_clean), 2):
        if i + 1 < len(hex_clean):
            # Chaque paire représente un caractère ASCII
            ascii_val = int(hex_clean[i:i+2], 16)
            if 48 <= ascii_val <= 57 or 65 <= ascii_val <= 70 or 97 <= ascii_val <= 102:
                # C'est un caractère hex valide
                hex_str += chr(ascii_val)
    
    # Maintenant hex_str contient "61A0..." qu'on peut convertir en bytes
    return bytes.fromhex(hex_str)

def parse_jsonl_line(line: str) -> Optional[Dict]:
    """Parse une ligne JSONL."""
    try:
        return json.loads(line.strip())
    except:
        return None

def extract_page_bytes(raw_hex_ascii: str) -> bytes:
    """Extrait les bytes réels d'une page depuis le format hex ASCII."""
    if not raw_hex_ascii:
        return b""
    
    # Le format est: hex ASCII qui représente "61A0..."
    # Exemple: "363141304646..." où chaque paire représente un caractère ASCII
    # "36" = 0x36 = '6', "31" = 0x31 = '1', "41" = 0x41 = 'A', "30" = 0x30 = '0'
    # Donc "36314130" = "61A0" en texte hex
    
    hex_str = ""
    i = 0
    while i < len(raw_hex_ascii):
        if i + 1 < len(raw_hex_ascii):
            # Lire deux caractères hex ASCII
            try:
                ascii_byte = int(raw_hex_ascii[i:i+2], 16)
                # Vérifier que c'est un caractère hex valide (0-9, A-F, a-f)
                if (48 <= ascii_byte <= 57) or (65 <= ascii_byte <= 70) or (97 <= ascii_byte <= 102):
                    # C'est un caractère hex valide en ASCII
                    hex_str += chr(ascii_byte)
            except ValueError:
                pass
        i += 2
    
    # Convertir en bytes (hex_str contient maintenant "61A0FF...")
    try:
        return bytes.fromhex(hex_str)
    except:
        return b""

def analyze_field(field_name: str, values: List[float], pages: List[bytes], page_name: str) -> Optional[Tuple[int, float, str]]:
    """Analyse un champ pour trouver son offset et échelle."""
    if not values or all(v is None for v in values):
        return None
    
    # Filtrer les valeurs None
    valid_values = [(i, v) for i, v in enumerate(values) if v is not None]
    if len(valid_values) < 2:
        return None
    
    # Chercher les bytes qui varient en corrélation avec les valeurs
    # On compare les pages pour trouver les bytes qui changent
    if len(pages) < 2:
        return None
    
    # Pour chaque position dans la page, vérifier si elle varie avec les valeurs
    min_len = min(len(p) for p in pages)
    if min_len < 4:
        return None
    
    best_correlation = None
    best_offset = None
    best_scale = None
    
    # Essayer différentes positions et échelles
    for offset in range(0, min_len - 2, 2):  # Par pas de 2 (16-bit)
        # Essayer différentes échelles communes
        for scale in [1.0, 0.1, 0.01, 10.0, 100.0, 0.5, 2.0]:
            # Essayer 16-bit big-endian
            try:
                correlations = []
                for idx, val in valid_values:
                    if idx < len(pages) and offset + 1 < len(pages[idx]):
                        raw_val = (pages[idx][offset] << 8) | pages[idx][offset + 1]
                        decoded = raw_val * scale
                        if abs(decoded - val) < abs(val) * 0.1 + 1.0:  # 10% tolerance
                            correlations.append(abs(decoded - val))
                
                if correlations and len(correlations) >= len(valid_values) * 0.7:
                    avg_error = sum(correlations) / len(correlations)
                    if best_correlation is None or avg_error < best_correlation:
                        best_correlation = avg_error
                        best_offset = offset
                        best_scale = scale
            except:
                pass
    
    if best_offset is not None:
        return (best_offset, best_scale, page_name)
    return None

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 sz_decode_analyze.py <sz_sync_ocr.jsonl>")
        sys.exit(1)
    
    jsonl_path = sys.argv[1]
    
    # Charger toutes les données
    all_data = []
    with open(jsonl_path, 'r') as f:
        for line in f:
            data = parse_jsonl_line(line)
            if data and data.get('ocr_ok') and 'raw' in data and 'values' in data:
                all_data.append(data)
    
    if not all_data:
        print("Aucune donnée valide trouvée")
        sys.exit(1)
    
    print(f"Analysant {len(all_data)} frames...")
    
    # Extraire les pages et valeurs pour chaque frame
    pages_21A0 = []
    pages_21A2 = []
    pages_21A5 = []
    pages_21CD = []
    
    field_values = {field: [] for field in [
        'desired_idle_speed_rpm', 'accelerator_pct', 'intake_c', 'battery_v',
        'fuel_temp_c', 'bar_pressure_kpa', 'bar_pressure_mmhg', 'abs_pressure_mbar',
        'air_flow_estimate_mgcp', 'air_flow_request_mgcp', 'speed_kmh',
        'rail_pressure_bar', 'rail_pressure_control_bar', 'desired_egr_position_pct',
        'gear_ratio', 'egr_position_pct', 'engine_temp_c', 'air_temp_c',
        'requested_in_pressure_mbar', 'engine_rpm'
    ]}
    
    for data in all_data:
        raw = data.get('raw', {})
        values = data.get('values', {})
        
        # Extraire les bytes des pages (gérer None)
        if '21A0' in raw and raw['21A0']:
            pages_21A0.append(extract_page_bytes(raw['21A0']))
        else:
            pages_21A0.append(b"")
        if '21A2' in raw and raw['21A2']:
            pages_21A2.append(extract_page_bytes(raw['21A2']))
        else:
            pages_21A2.append(b"")
        if '21A5' in raw and raw['21A5']:
            pages_21A5.append(extract_page_bytes(raw['21A5']))
        else:
            pages_21A5.append(b"")
        if '21CD' in raw and raw['21CD']:
            pages_21CD.append(extract_page_bytes(raw['21CD']))
        else:
            pages_21CD.append(b"")
        
        # Extraire les valeurs
        for field in field_values:
            field_values[field].append(values.get(field))
    
    # Analyser chaque champ
    results = {}
    for field in field_values:
        # Essayer dans chaque page
        for page_name, pages in [('21A0', pages_21A0), ('21A2', pages_21A2), 
                                  ('21A5', pages_21A5), ('21CD', pages_21CD)]:
            result = analyze_field(field, field_values[field], pages, page_name)
            if result:
                results[field] = result
                print(f"{field}: offset={result[0]}, scale={result[1]}, page={result[2]}")
                break
    
    # Générer le code C++
    print("\n// Code C++ généré:")
    print("static void decodeSzFromPages(")
    print("  const uint8_t* a0, size_t a0Len,")
    print("  const uint8_t* a2, size_t a2Len,")
    print("  const uint8_t* a5, size_t a5Len,")
    print("  const uint8_t* cd, size_t cdLen,")
    print("  SzData& out")
    print(") {")
    print("  // Décodage basé sur l'analyse OCR")
    
    for field, (offset, scale, page) in results.items():
        page_var = {'21A0': 'a0', '21A2': 'a2', '21A5': 'a5', '21CD': 'cd'}[page]
        page_len = {'21A0': 'a0Len', '21A2': 'a2Len', '21A5': 'a5Len', '21CD': 'cdLen'}[page]
        
        if scale == 1.0:
            scale_str = ""
        elif scale == 0.1:
            scale_str = " / 10.0f"
        elif scale == 0.01:
            scale_str = " / 100.0f"
        elif scale == 10.0:
            scale_str = " * 10.0f"
        elif scale == 100.0:
            scale_str = " * 100.0f"
        else:
            scale_str = f" * {scale}f"
        
        print(f"  // {field}")
        print(f"  if ({page_len} > {offset + 1}) {{")
        print(f"    uint16_t raw_{field} = ({page_var}[{offset}] << 8) | {page_var}[{offset + 1}];")
        print(f"    out.{field} = (float)raw_{field}{scale_str};")
        print(f"  }}")
    
    print("}")

if __name__ == '__main__':
    main()
