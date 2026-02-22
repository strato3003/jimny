#!/usr/bin/env python3
"""Trouve les octets 21A0 qui corrèlent avec le RPM. Ralenti = 800 tr/min."""
import json

def hex_to_bytes(h):
    h = h.strip().replace(" ", "")
    return bytes.fromhex(h)

def load_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows

def main():
    path = "jimny-2026-02-19-decheterrie-jard.json"
    rows = load_rows(path)
    if not rows:
        print("No data")
        return

    # Ralenti: début du log (14:14), moteur tourne ~800 rpm
    idle_samples = [r for r in rows if r.get("datetime", "").startswith("2026-02-19 14:14")][:20]
    # Fort régime: 14:45 (tu as dit ~3000 rpm)
    high_samples = [r for r in rows if "14:45" in r.get("datetime", "") or "14:46" in r.get("datetime", "")][:30]

    def get_a0_bytes(r):
        raw = r.get("raw", {}).get("21A0", "")
        if not raw:
            return None
        return hex_to_bytes(raw)

    # Idle: on veut un mot (2 bytes) qui donne ~800 avec une certaine échelle
    # High: le même mot doit donner ~2000-3000
    # Ratio attendu high/idle ≈ 3000/800 = 3.75 (ou 2000/800 = 2.5)
    print("=== Ralenti (14:14, attendu ~800 tr/min) ===")
    idle_bytes = [get_a0_bytes(r) for r in idle_samples if get_a0_bytes(r)]
    if not idle_bytes:
        print("Pas de 21A0 en ralenti")
        return
    b0 = idle_bytes[0]
    print(f"Longueur 21A0: {len(b0)} octets")
    print("Mots 16b (offset 2 = premier payload):")
    for i in range(2, min(30, len(b0) - 1), 2):
        w = (b0[i] << 8) | b0[i + 1]
        # échelle pour que 800 rpm = w * k  => k = 800/w
        if w > 0:
            scale_800 = 800.0 / w
            print(f"  offset {i:2d}-{i+1:2d}: 0x{b0[i]:02X}{b0[i+1]:02X} = {w:5d}  (×{scale_800:.4f} => 800 rpm)")
        else:
            print(f"  offset {i:2d}-{i+1:2d}: 0x{b0[i]:02X}{b0[i+1]:02X} = {w:5d}")

    print("\n=== Fort régime (14:45-14:46, attendu ~2000-3000 tr/min) ===")
    high_bytes = [get_a0_bytes(r) for r in high_samples if get_a0_bytes(r)]
    if not high_bytes:
        print("Pas de 21A0 à 14:45")
        return
    b1 = high_bytes[0]
    for i in range(2, min(30, len(b1) - 1), 2):
        w = (b1[i] << 8) | b1[i + 1]
        print(f"  offset {i:2d}-{i+1:2d}: 0x{b1[i]:02X}{b1[i+1]:02X} = {w:5d}")

    print("\n=== Recherche: quel mot (idle→high) a un ratio ~2.5 à 3.75 ? ===")
    for i in range(2, min(30, len(b0) - 1), 2):
        w_idle = (b0[i] << 8) | b0[i + 1]
        ws_high = [(b[i] << 8) | b[i + 1] for b in high_bytes if len(b) > i + 1]
        if not ws_high or w_idle == 0:
            continue
        avg_high = sum(ws_high) / len(ws_high)
        ratio = avg_high / w_idle
        # scale pour que idle=800: factor = 800/w_idle, alors high_rpm = avg_high * 800 / w_idle
        rpm_idle = 800.0
        scale = rpm_idle / w_idle
        rpm_high_est = avg_high * scale
        # On veut rpm_high_est entre 2000 et 3500
        ok = 1800 <= rpm_high_est <= 3500 and 600 <= rpm_idle <= 1000
        mark = " *** CANDIDAT RPM" if ok else ""
        print(f"  offset {i:2d}: idle word={w_idle:5d}  high avg={avg_high:.0f}  ratio={ratio:.2f}  => scale={scale:.4f}  rpm_idle=800  rpm_high_est={rpm_high_est:.0f}{mark}")

    # Décode actuel: bytes 14-15, scale 0.125
    print("\n=== Décode actuel (bytes 14-15 × 0.125) ===")
    for label, samples in [("idle", idle_samples), ("14:45", high_samples[:5])]:
        for r in samples[:3]:
            raw = r.get("raw", {}).get("21A0", "")
            if not raw:
                continue
            b = hex_to_bytes(raw)
            if len(b) > 15:
                w = (b[14] << 8) | b[15]
                decoded = w * 0.125
                print(f"  {label} {r.get('datetime','')[-8:]}  word(14-15)={w}  ×0.125 => {decoded:.0f} rpm")

def scan_page(rows, page_key, label_idle, label_high, idle_dt_prefix, high_dt_substr):
    """Scan a raw page (21A0, 21A2, 21A5) for a 16-bit word with ratio ~3.75."""
    idle_rows = [r for r in rows if r.get("datetime", "").startswith(idle_dt_prefix)][:15]
    high_rows = [r for r in rows if high_dt_substr in r.get("datetime", "")][:20]
    def get_bytes(r):
        raw = r.get("raw", {}).get(page_key, "")
        return hex_to_bytes(raw) if raw else None
    idle_b = [get_bytes(r) for r in idle_rows if get_bytes(r)]
    high_b = [get_bytes(r) for r in high_rows if get_bytes(r)]
    if not idle_b or not high_b:
        return
    print(f"\n--- Page {page_key} ---")
    for i in range(0, min(32, len(idle_b[0]) - 1), 2):
        w_idle = (idle_b[0][i] << 8) | idle_b[0][i + 1]
        ws_high = [(b[i] << 8) | b[i + 1] for b in high_b if len(b) > i + 1]
        if not ws_high or w_idle == 0:
            continue
        avg_high = sum(ws_high) / len(ws_high)
        ratio = avg_high / w_idle
        scale = 800.0 / w_idle
        rpm_high = avg_high * scale
        ok = 1.8 <= ratio <= 4.0 and 2000 <= rpm_high <= 3500
        mark = " *** CANDIDAT RPM" if ok else ""
        print(f"  offset {i:2d}: idle={w_idle:5d}  high_avg={avg_high:.0f}  ratio={ratio:.2f}  => rpm_high_est={rpm_high:.0f}{mark}")

if __name__ == "__main__":
    main()
    rows = load_rows("jimny-2026-02-19-decheterrie-jard.json")
    for key, idle_prefix, high_substr in [
        ("21A0", "2026-02-19 14:14", "14:45"),
        ("21A2", "2026-02-19 14:14", "14:45"),
        ("21A5", "2026-02-19 14:14", "14:45"),
    ]:
        scan_page(rows, key, "idle", "high", idle_prefix, high_substr)
