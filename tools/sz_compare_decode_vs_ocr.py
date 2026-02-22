#!/usr/bin/env python3
"""
Relit les trames raw de medias/sz_sync_ocr.jsonl, applique le décodeur
(identique à esp32/sz-mqtt/sz_decode.h) et compare avec les valeurs OCR du même jsonl.

Usage:
  python3 tools/sz_compare_decode_vs_ocr.py medias/sz_sync_ocr.jsonl
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

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


def extract_page_bytes(raw_hex_ascii: Optional[str]) -> bytes:
    """Hex ASCII (jsonl) → bytes, comme dans sz_decode_analyze / sz_decode_from_ocr_jsonl."""
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


def u16(b: bytes, off: int) -> Optional[int]:
    if b is None or off + 1 >= len(b):
        return None
    return (b[off] << 8) | b[off + 1]


def decode_from_mapping(
    pages: Dict[str, bytes],
    mapping: Dict[str, Dict[str, Any]],
) -> Dict[str, Optional[float]]:
    """Décode à partir du mapping JSON (page, offset, mult, div, add)."""
    out: Dict[str, Optional[float]] = {f: None for f in FIELDS}
    for field in FIELDS:
        m = mapping.get(field)
        if not m:
            continue
        page = m.get("page")
        offset = m.get("offset", 0)
        mult = m.get("mult", 1)
        div = m.get("div", 1) or 1
        add = m.get("add", 0) or 0
        b = pages.get(page)
        raw = u16(b, offset)
        if raw is not None:
            out[field] = raw * mult / div + add
    return out


def decode_from_pages(a0: bytes, a2: bytes, a5: bytes, cd: bytes) -> Dict[str, Optional[float]]:
    """Décodeur miroir de sz_decode.h (même page, offset, formule)."""
    out: Dict[str, Optional[float]] = {f: None for f in FIELDS}
    if u16(a0, 4) is not None:
        out["desired_idle_speed_rpm"] = u16(a0, 4) / 4.0
    if len(a0) > 25:
        out["accelerator_pct"] = float(u16(a0, 24))
    if len(a2) > 43:
        out["intake_c"] = float(u16(a2, 42))
    if len(a0) > 15:
        out["battery_v"] = u16(a0, 14) / 1000.0
    if len(a2) > 11:
        out["fuel_temp_c"] = u16(a2, 10) / 10.0
    if len(a2) > 23:
        out["bar_pressure_kpa"] = u16(a2, 22) / 10.0
    if len(a0) > 13:
        out["bar_pressure_mmhg"] = u16(a0, 12) / 4.0
    if len(a0) > 19:
        out["abs_pressure_mbar"] = float(u16(a0, 18))
    if len(a0) > 21:
        out["air_flow_estimate_mgcp"] = u16(a0, 20) / 10.0
    if len(a2) > 29:
        out["air_flow_request_mgcp"] = u16(a2, 28) / 4.0
    if len(a2) > 41:
        out["speed_kmh"] = float(u16(a2, 40))
    if len(a2) > 33:
        out["rail_pressure_bar"] = u16(a2, 32) / 10.0
    if len(a2) > 9:
        out["rail_pressure_control_bar"] = u16(a2, 8) / 2.0
    if len(cd) > 5:
        out["desired_egr_position_pct"] = u16(cd, 4) / 100.0
    if len(a5) > 25:
        out["gear_ratio"] = float(u16(a5, 24) * 5)
    if len(a5) > 7:
        out["egr_position_pct"] = u16(a5, 6) / 1000.0
    if len(a2) > 25:
        out["engine_temp_c"] = u16(a2, 24) / 100.0
    if len(a0) > 9:
        out["air_temp_c"] = u16(a0, 8) / 4.0
    if len(a0) > 17:
        out["requested_in_pressure_mbar"] = float(u16(a0, 16))
    if len(a2) > 13:
        out["engine_rpm"] = float(u16(a2, 12) * 8)
    return out


def load_jsonl(path: str) -> List[Dict[str, Any]]:
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def run_comparison(
    rows: List[Dict[str, Any]],
    mapping: Optional[Dict[str, Dict[str, Any]]] = None,
    use_last_known: bool = True,
) -> Dict[str, float]:
    """Retourne les MAE par champ (décodé vs OCR). Si use_last_known, les valeurs manquantes sont remplacées par la dernière connue."""
    err_sum: Dict[str, float] = {f: 0.0 for f in FIELDS}
    count: Dict[str, int] = {f: 0 for f in FIELDS}
    last_known: Dict[str, float] = {}
    for row in rows:
        raw = row.get("raw") or {}
        pages = {
            "21A0": extract_page_bytes(raw.get("21A0")),
            "21A2": extract_page_bytes(raw.get("21A2")),
            "21A5": extract_page_bytes(raw.get("21A5")),
            "21CD": extract_page_bytes(raw.get("21CD")),
        }
        if mapping:
            decoded = decode_from_mapping(pages, mapping)
        else:
            a0, a2, a5, cd = pages["21A0"], pages["21A2"], pages["21A5"], pages["21CD"]
            decoded = decode_from_pages(a0, a2, a5, cd)
        ocr = row.get("values") or {}
        for f in FIELDS:
            dec_v = decoded.get(f)
            if use_last_known and dec_v is None and f in last_known:
                dec_v = last_known[f]
            if dec_v is not None and use_last_known:
                last_known[f] = dec_v
            ocr_v = ocr.get(f)
            if dec_v is None or ocr_v is None:
                continue
            if f in ("desired_egr_position_pct", "egr_position_pct", "accelerator_pct") and ocr_v > 100 and ocr_v < 100000:
                ocr_v = ocr_v / 100.0
            err_sum[f] += abs(dec_v - ocr_v)
            count[f] += 1
    mae_by_field: Dict[str, float] = {}
    for f in FIELDS:
        n = count[f]
        mae_by_field[f] = (err_sum[f] / n) if n else 0.0
    return mae_by_field


def main() -> None:
    jsonl_path = sys.argv[1] if len(sys.argv) > 1 else "medias/sz_sync_ocr.jsonl"
    machine = "--machine" in sys.argv or "-m" in sys.argv
    path = Path(jsonl_path)
    if not path.exists():
        print(f"Fichier introuvable: {path}", file=sys.stderr)
        sys.exit(1)

    mapping_path = Path(__file__).resolve().parent / "sz_decode_mapping.json"
    mapping: Optional[Dict[str, Dict[str, Any]]] = None
    if mapping_path.exists():
        try:
            mapping = json.loads(mapping_path.read_text(encoding="utf-8"))
        except Exception:
            pass

    rows = load_jsonl(str(path))
    decoder_src = "mapping (sz_decode_mapping.json)" if mapping else "sz_decode.h (codé en dur)"
    if not machine:
        print(f"# {len(rows)} trames — décodeur {decoder_src}, comparé à l'OCR\n")

    mae_by_field = run_comparison(rows, mapping)

    if not machine:
        print("## Erreur moyenne |décodé − OCR| par champ\n")
    for f in FIELDS:
        mae = mae_by_field[f]
        if machine:
            print(f"mae\t{f}\t{mae:.6f}")
        else:
            n = sum(1 for row in rows if (row.get("values") or {}).get(f) is not None)
            if n == 0:
                print(f"  {f}: (pas de paires décodé/OCR)")
            else:
                print(f"  {f}: mae={mae:.3f}  n={n}")
    max_mae = max(mae_by_field.values()) if mae_by_field else 0.0
    if machine:
        print(f"max_mae\t{max_mae:.6f}")

    if not machine:
        def _norm_ocr(f: str, o: Any) -> Optional[float]:
            if o is None:
                return None
            if f in ("desired_egr_position_pct", "egr_position_pct", "accelerator_pct") and isinstance(o, (int, float)) and 100 < o < 100000:
                return o / 100.0
            return float(o) if isinstance(o, (int, float)) else None
        print("\n## Aperçu frame 1 (décodé vs OCR)\n")
        if rows:
            raw = rows[0].get("raw") or {}
            pages = {p: extract_page_bytes(raw.get(p)) for p in ("21A0", "21A2", "21A5", "21CD")}
            dec = decode_from_mapping(pages, mapping) if mapping else decode_from_pages(pages["21A0"], pages["21A2"], pages["21A5"], pages["21CD"])
            ocr = rows[0].get("values") or {}
            for f in FIELDS:
                d, o = dec.get(f), _norm_ocr(f, ocr.get(f))
                print(f"  {f}: décodé={d}  ocr={o}")
        if len(rows) >= 51:
            print("\n## Aperçu frame 51 (décodé vs OCR)\n")
            raw = rows[50].get("raw") or {}
            pages = {p: extract_page_bytes(raw.get(p)) for p in ("21A0", "21A2", "21A5", "21CD")}
            dec = decode_from_mapping(pages, mapping) if mapping else decode_from_pages(pages["21A0"], pages["21A2"], pages["21A5"], pages["21CD"])
            ocr = rows[50].get("values") or {}
            for f in FIELDS:
                d, o = dec.get(f), _norm_ocr(f, ocr.get(f))
                print(f"  {f}: décodé={d}  ocr={o}")


if __name__ == "__main__":
    main()
