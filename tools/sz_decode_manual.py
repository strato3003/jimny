#!/usr/bin/env python3
"""
Analyse manuelle pour identifier les champs manquants en comparant
les valeurs OCR avec les bytes bruts de manière plus exhaustive.
"""

import json
import sys

def hex_ascii_to_hex(hex_ascii):
    if not hex_ascii: return ""
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

def find_all_matches(target_value, bytes_data, max_offset=60):
    """Trouve tous les offsets possibles pour une valeur."""
    matches = []
    if not bytes_data or len(bytes_data) < 2:
        return matches
    
    scales = [1.0, 0.1, 0.01, 10.0, 100.0, 0.5, 2.0, 0.25, 4.0, 0.05, 20.0]
    
    for offset in range(0, min(max_offset, len(bytes_data) - 1), 2):
        # Big-endian 16-bit
        raw_val = (bytes_data[offset] << 8) | bytes_data[offset + 1]
        
        for scale in scales:
            decoded = raw_val * scale
            error = abs(decoded - target_value)
            # Tolérance adaptative
            if abs(target_value) < 1.0:
                tol = 0.2
            elif abs(target_value) < 10.0:
                tol = 0.5
            elif abs(target_value) < 100.0:
                tol = 2.0
            elif abs(target_value) < 1000.0:
                tol = 20.0
            else:
                tol = 100.0
            
            if error < tol:
                matches.append((offset, scale, error, decoded))
    
    return sorted(matches, key=lambda x: x[2])[:5]  # Top 5

def main():
    jsonl_path = sys.argv[1] if len(sys.argv) > 1 else 'medias/sz_sync_ocr.jsonl'
    
    frames = []
    with open(jsonl_path, 'r') as f:
        for line in f:
            try:
                data = json.loads(line.strip())
                if data.get('ocr_ok') and 'raw' in data and 'values' in data:
                    frames.append(data)
            except:
                pass
    
    if len(frames) < 3:
        print("Besoin d'au moins 3 frames")
        return
    
    # Champs à analyser en priorité
    priority_fields = [
        'engine_rpm', 'battery_v', 'engine_temp_c', 'air_temp_c',
        'intake_c', 'rail_pressure_bar', 'rail_pressure_control_bar',
        'egr_position_pct', 'desired_egr_position_pct', 'abs_pressure_mbar',
        'air_flow_estimate_mgcp', 'air_flow_request_mgcp'
    ]
    
    print("Analyse manuelle des champs prioritaires:\n")
    
    for field in priority_fields:
        # Prendre 3 frames avec des valeurs différentes
        values_with_idx = [(i, f['values'].get(field)) for i, f in enumerate(frames) 
                           if f['values'].get(field) is not None]
        
        if len(values_with_idx) < 2:
            continue
        
        # Prendre les 3 premières avec valeurs différentes
        selected = []
        seen_vals = set()
        for idx, val in values_with_idx:
            if val not in seen_vals or len(selected) < 3:
                selected.append((idx, val))
                seen_vals.add(val)
            if len(selected) >= 3:
                break
        
        if len(selected) < 2:
            continue
        
        print(f"\n{field} (valeurs: {[v for _, v in selected]})")
        
        # Analyser dans chaque page
        for page_name in ['21A0', '21A2', '21A5', '21CD']:
            all_matches = []
            for idx, val in selected:
                if idx < len(frames):
                    raw_hex = frames[idx]['raw'].get(page_name, "")
                    if raw_hex:
                        hex_real = hex_ascii_to_hex(raw_hex)
                        try:
                            bytes_data = bytes.fromhex(hex_real)
                            matches = find_all_matches(val, bytes_data)
                            for m in matches:
                                all_matches.append((page_name, idx, m))
                        except:
                            pass
            
            # Grouper par offset et scale
            offset_scale_counts = {}
            for page, idx, (offset, scale, error, _) in all_matches:
                key = (page, offset, round(scale, 2))
                if key not in offset_scale_counts:
                    offset_scale_counts[key] = []
                offset_scale_counts[key].append((idx, error))
            
            # Trouver les offsets cohérents (apparaissent dans plusieurs frames)
            for (page, offset, scale), matches_list in offset_scale_counts.items():
                if len(matches_list) >= 2:  # Au moins 2 frames
                    avg_error = sum(e for _, e in matches_list) / len(matches_list)
                    print(f"  {page} offset={offset:2d} scale={scale:6.2f} error={avg_error:6.3f} (frames: {len(matches_list)})")

if __name__ == '__main__':
    main()
