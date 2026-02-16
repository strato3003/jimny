#!/usr/bin/env python3
"""
Génère un fichier C (sz_data.h) à partir de sz_sync_ocr.jsonl
pour embarquer les données directement dans le sketch ESP32.

Usage:
    python3 tools/sz_embed.py medias/sz_sync_ocr.jsonl
"""

import json
import sys
import os

def escape_c_string(s):
    """Échappe une chaîne pour l'utiliser dans un littéral C."""
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 sz_embed.py <input.jsonl> [output.h]")
        sys.exit(1)
    
    input_file = sys.argv[1]
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        # Par défaut: génère dans le dossier du sketch
        output_file = os.path.join(os.path.dirname(os.path.dirname(input_file)), 
                                   'esp32', 'sz-replay-mqtt', 'sz_data.h')
    
    lines = []
    with open(input_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line:
                lines.append(line)
    
    print(f"Lecture de {len(lines)} lignes depuis {input_file}")
    
    # Génère le fichier C
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("// Fichier généré automatiquement par tools/sz_embed.py\n")
        f.write("// Ne pas modifier manuellement !\n\n")
        f.write(f"#ifndef SZ_DATA_H\n#define SZ_DATA_H\n\n")
        f.write(f"#define SZ_DATA_LINE_COUNT {len(lines)}\n\n")
        f.write("// Tableau des lignes JSONL (stocké en flash, pas en RAM)\n")
        f.write("// Note: ESP32 stocke automatiquement les const char* en flash\n")
        f.write("static const char* const sz_data_lines[SZ_DATA_LINE_COUNT] = {\n")
        
        for i, line in enumerate(lines):
            escaped = escape_c_string(line)
            comma = "," if i < len(lines) - 1 else ""
            f.write(f'  "{escaped}"{comma}\n')
        
        f.write("};\n\n")
        f.write("#endif // SZ_DATA_H\n")
    
    print(f"✓ Généré: {output_file}")
    print(f"  {len(lines)} lignes, ~{sum(len(l) for l in lines)} caractères")

if __name__ == '__main__':
    main()
