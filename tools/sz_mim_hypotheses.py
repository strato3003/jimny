#!/usr/bin/env python3
"""
Exploite les captures MIM (trames.log + sz_sync_ocr.jsonl issu du screencast sz.mp4)
pour:
  - Extraire les valeurs affichées OCR et les min/max par champ
  - Tester des hypothèses de décodage (raw, raw/100, raw/1000, raw*0.25, etc.)
  - Proposer des formules cohérentes avec les plages attendues (ex: EGR 0-100%)

Usage:
  python3 tools/sz_mim_hypotheses.py medias/sz_sync_ocr.jsonl
"""

import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Champs SZ Viewer (ordre d'affichage)
FIELDS = [
    "desired_idle_speed_rpm",
    "accelerator_pct",
    "intake_c",
    "battery_v",
    "fuel_temp_c",
    "bar_pressure_kpa",
    "bar_pressure_mmhg",
    "abs_pressure_mbar",
    "air_flow_estimate_mgcp",
    "air_flow_request_mgcp",
    "speed_kmh",
    "rail_pressure_bar",
    "rail_pressure_control_bar",
    "desired_egr_position_pct",
    "gear_ratio",
    "egr_position_pct",
    "engine_temp_c",
    "air_temp_c",
    "requested_in_pressure_mbar",
    "engine_rpm",
]

# Hypothèses connues (page, offset dans payload, formules à tester)
# offset = octets après l'en-tête 61 XX (donc indice 0 = premier octet du payload)
HYPOTHESES = [
    ("egr_position_pct", "21A5", 2, [(1, "raw"), (0.25, "raw*0.25"), (0.001, "raw/1000")]),
    ("desired_egr_position_pct", "21A2", 2, [(1, "raw"), (0.01, "raw/100"), (0.001, "raw/1000")]),
    ("engine_rpm", "21A2", 12, [(8, "raw*8"), (8.0, "raw*8")]),
    ("desired_idle_speed_rpm", "21A0", 2, [(0.25, "raw*0.25")]),
    ("speed_kmh", "21A0", 4, [(1, "raw")]),
    ("engine_temp_c", "21A2", 18, [(0.01, "raw*0.01")]),
    ("bar_pressure_kpa", "21A2", 20, [(0.1, "raw/10")]),
    ("bar_pressure_mmhg", "21A2", 0, [(5, "raw*5")]),
]


def extract_page_bytes(raw_hex_ascii: Optional[str]) -> bytes:
    """Convertit le format hex ASCII du jsonl (ex: 36314130... = '61A0') en bytes."""
    if not raw_hex_ascii:
        return b""
    hex_str = ""
    i = 0
    while i < len(raw_hex_ascii):
        if i + 1 < len(raw_hex_ascii):
            try:
                ascii_byte = int(raw_hex_ascii[i : i + 2], 16)
                if (48 <= ascii_byte <= 57) or (65 <= ascii_byte <= 70) or (97 <= ascii_byte <= 102):
                    hex_str += chr(ascii_byte)
            except ValueError:
                pass
        i += 2
    try:
        return bytes.fromhex(hex_str)
    except Exception:
        return b""


def load_jsonl(path: str) -> List[Dict[str, Any]]:
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                if d.get("values") and (d.get("raw") or d.get("ocr_ok")):
                    out.append(d)
            except json.JSONDecodeError:
                continue
    return out


def stats_per_field(rows: List[Dict]) -> Dict[str, Dict[str, float]]:
    """Min/max/count des valeurs OCR par champ."""
    stats = {f: {"min": None, "max": None, "count": 0, "values": []} for f in FIELDS}
    for row in rows:
        v = row.get("values") or {}
        for f in FIELDS:
            x = v.get(f)
            if x is not None and isinstance(x, (int, float)):
                stats[f]["values"].append(x)
                if stats[f]["min"] is None or x < stats[f]["min"]:
                    stats[f]["min"] = x
                if stats[f]["max"] is None or x > stats[f]["max"]:
                    stats[f]["max"] = x
                stats[f]["count"] += 1
    return stats


def get_raw16(page_bytes: bytes, offset: int) -> Optional[int]:
    if offset + 1 < len(page_bytes):
        return (page_bytes[offset] << 8) | page_bytes[offset + 1]
    return None


def test_hypothesis(
    rows: List[Dict],
    field: str,
    page: str,
    offset: int,
    scale: float,
    scale_name: str,
) -> Tuple[float, int]:
    """Retourne (erreur moyenne, nombre de points) pour cette hypothèse."""
    err_sum = 0.0
    n = 0
    for row in rows:
        ocr_val = (row.get("values") or {}).get(field)
        if ocr_val is None:
            continue
        raw = row.get("raw") or {}
        page_hex = raw.get(page)
        if not page_hex:
            continue
        payload = extract_page_bytes(page_hex)
        raw16 = get_raw16(payload, offset)
        if raw16 is None:
            continue
        if scale >= 1:
            decoded = raw16 * scale
        else:
            decoded = raw16 * scale  # e.g. 0.001 = /1000
        err_sum += abs(decoded - ocr_val)
        n += 1
    if n == 0:
        return (float("inf"), 0)
    return (err_sum / n, n)


def main() -> None:
    jsonl_path = sys.argv[1] if len(sys.argv) > 1 else "medias/sz_sync_ocr.jsonl"
    if not Path(jsonl_path).exists():
        print(f"Fichier non trouvé: {jsonl_path}", file=sys.stderr)
        sys.exit(1)

    rows = load_jsonl(jsonl_path)
    print(f"# {len(rows)} trames chargées depuis {jsonl_path}\n")

    # 1) Min/Max par champ (valeurs affichées OCR)
    print("## Min/Max des valeurs affichées (OCR)\n")
    stats = stats_per_field(rows)
    for f in FIELDS:
        s = stats[f]
        if s["count"] == 0:
            print(f"  {f}: (aucune valeur)")
            continue
        print(f"  {f}: min={s['min']} max={s['max']} count={s['count']}")

    # 2) Meilleur offset pour EGR position (raw/1000)
    print("\n## EGR position: recherche du meilleur offset (formule raw/1000)\n")
    best_off = None
    best_err = float("inf")
    for page in ["21A5", "21A2", "21A0"]:
        for off in range(0, 14, 2):
            err, n = test_hypothesis(rows, "egr_position_pct", page, off, 0.001, "raw/1000")
            if n >= 10 and err < best_err:
                best_err = err
                best_off = (page, off)
    if best_off:
        print(f"  Meilleur: {best_off[0]} offset {best_off[1]} (err_moy={best_err:.2f})")
    print("  Référence: frame_00051.png affiche EGR position = 50.147% → valeur affichée = raw/1000.\n")

    # Desired EGR: OCR affiche souvent 64.02% mais lit 6402 → hypothèse raw/100 si raw > 100
    print("## Desired EGR: plage OCR 0–6403 → probablement XX.XX% (décimal mal lu)\n")
    desired_vals = [r.get("values", {}).get("desired_egr_position_pct") for r in rows]
    desired_vals = [v for v in desired_vals if v is not None]
    if desired_vals:
        print(f"  Si on borne à 0–100: min={min(min(desired_vals), 100):.1f} max={min(max(desired_vals), 100):.1f}")
        print("  Hypothèse: affichage 64.02% lu comme 6402 → utiliser raw/100 pour 21A2 bytes 4-5.\n")

    # 3) Tester les hypothèses définies
    print("\n## Erreur moyenne (décodé vs OCR) par hypothèse\n")
    for field, page, offset, formulas in HYPOTHESES:
        for scale, name in formulas:
            err, n = test_hypothesis(rows, field, page, offset, scale, name)
            if n > 0:
                print(f"  {field} ({page} off={offset}) {name}: err_moy={err:.2f} n={n}")

    # 4) Plages attendues pour validation
    print("\n## Plages attendues (pour valider les formules)\n")
    print("  - EGR / Desired EGR: 0–100 %")
    print("  - engine_rpm: 0–5000 tr/min")
    print("  - speed_kmh: 0–300 km/h")
    print("  - battery_v: 10–15 V")
    print("  - engine_temp_c / air_temp_c: -40–120 °C")


if __name__ == "__main__":
    main()
