#!/usr/bin/env python3
"""Vérifie le décode RPM 21A2 octets 12-13 × (800/105) sur le log."""
import json

def main():
    path = "jimny-2026-02-19-decheterrie-jard.json"
    scale = 800.0 / 105.0
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    print("Nouveau décode RPM (21A2 bytes 12-13 × 800/105):")
    print("  Ralenti 14:14 (attendu ~800)")
    n_idle = n_45 = 0
    for line in lines[:25]:
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        raw_a2 = r.get("raw", {}).get("21A2", "")
        if not raw_a2 or len(raw_a2) < 28:
            continue
        b = bytes.fromhex(raw_a2.replace(" ", ""))
        w = (b[12] << 8) | b[13]
        rpm = w * scale
        if 0 <= rpm <= 5000:
            n_idle += 1
            if n_idle <= 5:
                print(f"    {r.get('datetime','')[-8:]}  raw={w}  => {rpm:.0f} tr/min")
    print("  14:45 (attendu ~2000-3000)")
    for line in lines:
        if "14:45" not in line and "14:46" not in line:
            continue
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        raw_a2 = r.get("raw", {}).get("21A2", "")
        if not raw_a2 or len(raw_a2) < 28:
            continue
        b = bytes.fromhex(raw_a2.replace(" ", ""))
        w = (b[12] << 8) | b[13]
        rpm = w * scale
        if 0 <= rpm <= 5000:
            n_45 += 1
            if n_45 <= 8:
                print(f"    {r.get('datetime','')[-8:]}  raw={w}  => {rpm:.0f} tr/min")
    print(f"\n  Total ralenti (14:14) avec rpm valide: {n_idle}")
    print(f"  Total 14:45-14:46 avec rpm valide: {n_45}")

if __name__ == "__main__":
    main()
