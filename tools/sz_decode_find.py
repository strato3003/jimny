#!/usr/bin/env python3
"""
Analyse les données OCR pour trouver les offsets et échelles des champs.
Approche: comparer les valeurs OCR avec les bytes bruts pour identifier les correspondances.
"""

import json
import sys

def hex_ascii_to_hex(hex_ascii):
    """Convertit hex ASCII (ex: "36314130") en hex réel (ex: "61A0")."""
    if not hex_ascii:
        return ""
    hex_str = ""
    for i in range(0, len(hex_ascii), 2):
        if i + 1 < len(hex_ascii):
            try:
                ascii_byte = int(hex_ascii[i:i+2], 16)
                if (48 <= ascii_byte <= 57) or (65 <= ascii_byte <= 70) or (97 <= ascii_byte <= 102):
                    hex_str += chr(ascii_byte)
            except:
                pass
    return hex_str

def hex_to_bytes(hex_str):
    """Convertit une chaîne hex en bytes."""
    try:
        return bytes.fromhex(hex_str)
    except:
        return b""

def find_value_in_bytes(target_value, bytes_data, tolerance=0.2):
    """Cherche une valeur dans les bytes avec différentes échelles et offsets."""
    if not bytes_data or len(bytes_data) < 2:
        return None
    
    # Essayer différentes échelles communes (plus de variantes)
    scales = [1.0, 0.1, 0.01, 10.0, 100.0, 0.5, 2.0, 0.05, 20.0, 0.2, 5.0, 0.001, 1000.0, 0.25, 4.0]
    offsets = []
    
    # Essayer 16-bit big-endian
    for offset in range(0, len(bytes_data) - 1, 2):
        raw_val = (bytes_data[offset] << 8) | bytes_data[offset + 1]
        
        for scale in scales:
            decoded = raw_val * scale
            # Tolérance adaptative
            if abs(target_value) < 1.0:
                tol = 0.2
            elif abs(target_value) < 10.0:
                tol = max(0.2, abs(target_value) * tolerance)
            elif abs(target_value) < 100.0:
                tol = max(1.0, abs(target_value) * tolerance)
            else:
                tol = max(5.0, abs(target_value) * tolerance)
            
            error = abs(decoded - target_value)
            if error < tol:
                offsets.append((offset, scale, error, decoded, 'be16'))
    
    # Essayer aussi 16-bit little-endian
    for offset in range(0, len(bytes_data) - 1, 2):
        raw_val = bytes_data[offset] | (bytes_data[offset + 1] << 8)
        
        for scale in scales:
            decoded = raw_val * scale
            if abs(target_value) < 1.0:
                tol = 0.2
            elif abs(target_value) < 10.0:
                tol = max(0.2, abs(target_value) * tolerance)
            elif abs(target_value) < 100.0:
                tol = max(1.0, abs(target_value) * tolerance)
            else:
                tol = max(5.0, abs(target_value) * tolerance)
            
            error = abs(decoded - target_value)
            if error < tol:
                offsets.append((offset, scale, error, decoded, 'le16'))
    
    # Essayer 8-bit (pour les petites valeurs)
    if abs(target_value) < 255.0:
        for offset in range(len(bytes_data)):
            raw_val = bytes_data[offset]
            
            for scale in [1.0, 0.1, 0.01, 10.0, 100.0]:
                decoded = raw_val * scale
                tol = max(0.5, abs(target_value) * tolerance)
                error = abs(decoded - target_value)
                if error < tol:
                    offsets.append((offset, scale, error, decoded, 'u8'))
    
    if offsets:
        # Retourner le meilleur match
        offsets.sort(key=lambda x: x[2])  # Trier par erreur
        return offsets[0][:4]  # Retourner sans le type
    return None

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 sz_decode_find.py <sz_sync_ocr.jsonl>")
        sys.exit(1)
    
    jsonl_path = sys.argv[1]
    
    # Charger les données
    frames = []
    with open(jsonl_path, 'r') as f:
        for line in f:
            try:
                data = json.loads(line.strip())
                if data.get('ocr_ok') and 'raw' in data and 'values' in data:
                    frames.append(data)
            except:
                pass
    
    if len(frames) < 2:
        print("Besoin d'au moins 2 frames")
        sys.exit(1)
    
    print(f"Analysant {len(frames)} frames...\n")
    
    # Extraire les pages en bytes
    pages_data = {'21A0': [], '21A2': [], '21A5': [], '21CD': []}
    for frame in frames:
        for page_name in pages_data:
            raw_hex_ascii = frame['raw'].get(page_name, "")
            if raw_hex_ascii:
                hex_real = hex_ascii_to_hex(raw_hex_ascii)
                bytes_data = hex_to_bytes(hex_real)
                pages_data[page_name].append(bytes_data)
            else:
                pages_data[page_name].append(b"")
    
    # Analyser chaque champ
    field_names = [
        'desired_idle_speed_rpm', 'accelerator_pct', 'intake_c', 'battery_v',
        'fuel_temp_c', 'bar_pressure_kpa', 'bar_pressure_mmhg', 'abs_pressure_mbar',
        'air_flow_estimate_mgcp', 'air_flow_request_mgcp', 'speed_kmh',
        'rail_pressure_bar', 'rail_pressure_control_bar', 'desired_egr_position_pct',
        'gear_ratio', 'egr_position_pct', 'engine_temp_c', 'air_temp_c',
        'requested_in_pressure_mbar', 'engine_rpm'
    ]
    
    results = {}
    
    for field in field_names:
        # Extraire les valeurs pour ce champ
        values = [frame['values'].get(field) for frame in frames]
        valid_values = [(i, v) for i, v in enumerate(values) if v is not None]
        
        if len(valid_values) < 2:
            continue
        
        # Chercher dans chaque page
        best_match = None
        for page_name in ['21A0', '21A2', '21A5', '21CD']:
            pages = pages_data[page_name]
            if not pages or not pages[0]:
                continue
            
            # Essayer de trouver une correspondance
            matches = []
            for idx, val in valid_values:
                if idx < len(pages) and len(pages[idx]) >= 2:
                    match = find_value_in_bytes(val, pages[idx], tolerance=0.2)
                    if match:
                        matches.append((page_name, idx, match))
            
            if len(matches) >= max(2, len(valid_values) * 0.6):  # 60% de correspondances, min 2
                # Calculer l'offset et l'échelle moyens
                offsets = [m[2][0] for m in matches]
                scales = [m[2][1] for m in matches]
                
                # Vérifier que l'offset est cohérent (tolérance de 2 bytes)
                offset_counts = {}
                for o in offsets:
                    offset_counts[o] = offset_counts.get(o, 0) + 1
                
                # Prendre l'offset le plus fréquent
                most_common_offset = max(offset_counts.items(), key=lambda x: x[1])[0]
                offset_matches = [m for m in matches if m[2][0] == most_common_offset]
                
                if len(offset_matches) >= len(matches) * 0.7:  # 70% ont le même offset
                    # Vérifier l'échelle
                    scales_for_offset = [m[2][1] for m in offset_matches]
                    scale_counts = {}
                    for s in scales_for_offset:
                        # Arrondir l'échelle pour grouper
                        s_rounded = round(s, 2)
                        scale_counts[s_rounded] = scale_counts.get(s_rounded, 0) + 1
                    
                    most_common_scale = max(scale_counts.items(), key=lambda x: x[1])[0]
                    scale_matches = [m for m in offset_matches if abs(m[2][1] - most_common_scale) < 0.01]
                    
                    if len(scale_matches) >= len(offset_matches) * 0.7:
                        avg_error = sum(m[2][2] for m in scale_matches) / len(scale_matches)
                        if best_match is None or avg_error < best_match[3]:
                            best_match = (page_name, most_common_offset, most_common_scale, avg_error)
        
        # Filtrer selon le type de valeur
        if best_match and best_match[2] != 0.0:
            # Tolérance d'erreur adaptative selon la magnitude de la valeur
            max_error = 2.0
            if abs(valid_values[0][1]) > 1000.0:
                max_error = 50.0  # Pour les grandes valeurs (rpm, pressions élevées)
            elif abs(valid_values[0][1]) > 100.0:
                max_error = 10.0
            elif abs(valid_values[0][1]) > 10.0:
                max_error = 2.0
            else:
                max_error = 0.5
            
            if best_match[3] < max_error:
                results[field] = best_match
                print(f"{field:30s} -> {best_match[0]:5s} offset={best_match[1]:3d} scale={best_match[2]:6.2f} error={best_match[3]:.3f}")
    
    # Générer le code C++
    print("\n" + "="*70)
    print("// Code C++ pour decodeSzFromPages()")
    print("="*70)
    print("static void decodeSzFromPages(")
    print("  const uint8_t* a0, size_t a0Len,")
    print("  const uint8_t* a2, size_t a2Len,")
    print("  const uint8_t* a5, size_t a5Len,")
    print("  const uint8_t* cd, size_t cdLen,")
    print("  SzData& out")
    print(") {")
    
    for field, (page, offset, scale, _) in sorted(results.items()):
        page_var = {'21A0': 'a0', '21A2': 'a2', '21A5': 'a5', '21CD': 'cd'}[page]
        page_len = {'21A0': 'a0Len', '21A2': 'a2Len', '21A5': 'a5Len', '21CD': 'cdLen'}[page]
        
        if scale == 1.0:
            scale_op = ""
        elif scale == 0.1:
            scale_op = " / 10.0f"
        elif scale == 0.01:
            scale_op = " / 100.0f"
        elif scale == 10.0:
            scale_op = " * 10.0f"
        elif scale == 100.0:
            scale_op = " * 100.0f"
        elif scale == 0.5:
            scale_op = " / 2.0f"
        elif scale == 2.0:
            scale_op = " * 2.0f"
        else:
            scale_op = f" * {scale}f"
        
        print(f"  // {field}")
        print(f"  if ({page_len} > {offset + 1}) {{")
        print(f"    uint16_t raw_{field.replace('.', '_')} = ({page_var}[{offset}] << 8) | {page_var}[{offset + 1}];")
        print(f"    out.{field} = (float)raw_{field.replace('.', '_')}{scale_op};")
        print(f"  }}")
    
    print("}")

if __name__ == '__main__':
    main()
