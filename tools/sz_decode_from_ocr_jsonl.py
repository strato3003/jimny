#!/usr/bin/env python3
"""
Dérive le décodeur SZ (page, offset, formule) UNIQUEMENT à partir de
medias/sz_sync_ocr.jsonl (trames brutes + valeurs OCR des frames du screencast sz.mp4).

Pour chaque champ des 20 du SZ Viewer, on cherche la combinaison (page, offset 16-bit, formule)
qui minimise l'erreur moyenne |décodé - OCR|. Les formules testées: *0.01, *0.1, *0.25, *0.5,
*1, *5, *8, /10, /100, /1000. On ignore les suffixes 0D0D3E en fin de trame.

Usage:
  python3 tools/sz_decode_from_ocr_jsonl.py medias/sz_sync_ocr.jsonl
  python3 tools/sz_decode_from_ocr_jsonl.py medias/sz_sync_ocr.jsonl --update-decode
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple, Union

# Candidat: (page, offset, mult, div, add, label, mae, n) — add=0 pour formules scale-only
Candidate = Tuple[str, int, float, float, float, str, float, int]

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

PAGES = ["21A0", "21A2", "21A5", "21CD"]

# Priorités issues de la doc / usage: on préfixe ces candidats pour les champs sensibles
# (page, offset, mult, div, label) — utilisé si le champ a peu de variance dans l’OCR (ex: rpm à 0)
PRIORITY_CANDIDATES: Dict[str, Tuple[str, int, float, float, str]] = {
    "engine_rpm": ("21A2", 12, 8, 1, "raw*8"),   # ralenti ~105 raw → 840 tr/min
    "egr_position_pct": ("21A5", 6, 1, 1000, "raw/1000"),  # 50.147% = 50147/1000 (frame_00051)
}

# (scale_multiplier, scale_divisor, label) — applied as value = raw16 * mult / div
FORMULAS: List[Tuple[float, float, str]] = [
    (1, 1, "raw"),
    (1, 10, "raw/10"),
    (1, 100, "raw/100"),
    (1, 1000, "raw/1000"),
    (1, 4, "raw*0.25"),
    (1, 2, "raw*0.5"),
    (1, 100, "raw*0.01"),
    (1, 10, "raw*0.1"),
    (5, 1, "raw*5"),
    (8, 1, "raw*8"),
    (25, 10, "raw*2.5"),
]


def extract_page_bytes(raw_hex_ascii: Optional[str]) -> bytes:
    """Convertit le format hex ASCII du jsonl (ex: 36314130... = '61A0') en bytes réels."""
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
                if d.get("values") and d.get("raw"):
                    out.append(d)
            except json.JSONDecodeError:
                continue
    return out


def get_raw16(payload: bytes, offset: int) -> Optional[int]:
    """Octets offset et offset+1 en big-endian (trame complète: indice 0 = 61, 1 = A0, 2 = premier data)."""
    if offset + 1 < len(payload):
        return (payload[offset] << 8) | payload[offset + 1]
    return None


def get_ocr_series(rows: List[Dict], field: str) -> List[float]:
    """Série des valeurs OCR pour un champ, avec rétention dernière valeur connue."""
    out: List[float] = []
    last: Optional[float] = None
    for row in rows:
        ocr_val = (row.get("values") or {}).get(field)
        if ocr_val is not None:
            t = target_ocr_value(field, ocr_val)
            if t is not None:
                last = t
        if last is not None:
            out.append(last)
        else:
            out.append(float("nan"))
    return out


def get_raw_series(rows: List[Dict], page: str, offset: int) -> List[float]:
    """Série des raw u16 pour (page, offset), avec rétention dernière valeur connue."""
    out: List[float] = []
    last: Optional[float] = None
    for row in rows:
        raw_hex = (row.get("raw") or {}).get(page)
        raw16 = get_raw16(extract_page_bytes(raw_hex) if raw_hex else b"", offset)
        if raw16 is not None:
            last = float(raw16)
        if last is not None:
            out.append(last)
        else:
            out.append(float("nan"))
    return out


def normalized_correlation(ocr_series: List[float], raw_series: List[float]) -> Tuple[float, int]:
    """
    Corrélation de Pearson entre les deux séries normalisées [0,1] (min/max/amplitude).
    Indépendante du facteur d'échelle et du décalage: rapproche les bonnes sections du flux.
    Retourne (correlation, n) avec n = nombre de paires valides. Si amplitude nulle, retourne (0, n).
    """
    n = min(len(ocr_series), len(raw_series))
    if n == 0:
        return (0.0, 0)
    pairs = [(ocr_series[i], raw_series[i]) for i in range(n) if not (ocr_series[i] != ocr_series[i] or raw_series[i] != raw_series[i])]
    if len(pairs) < 5:
        return (0.0, len(pairs))
    ocr_vals = [p[0] for p in pairs]
    raw_vals = [p[1] for p in pairs]
    min_o, max_o = min(ocr_vals), max(ocr_vals)
    min_r, max_r = min(raw_vals), max(raw_vals)
    amp_o = max_o - min_o
    amp_r = max_r - min_r
    if amp_o < 1e-15 and amp_r < 1e-15:
        return (1.0, len(pairs))
    if amp_o < 1e-15 or amp_r < 1e-15:
        return (0.0, len(pairs))
    no = [(x - min_o) / amp_o for x in ocr_vals]
    nr = [(x - min_r) / amp_r for x in raw_vals]
    mean_no = sum(no) / len(no)
    mean_nr = sum(nr) / len(nr)
    var_o = sum((x - mean_no) ** 2 for x in no) / len(no)
    var_r = sum((x - mean_nr) ** 2 for x in nr) / len(nr)
    if var_o < 1e-20 or var_r < 1e-20:
        return (0.0, len(pairs))
    cov = sum((no[i] - mean_no) * (nr[i] - mean_nr) for i in range(len(no))) / len(no)
    r = cov / (var_o * var_r) ** 0.5
    return (max(-1.0, min(1.0, r)), len(pairs))


def shape_candidates_for_field(
    rows: List[Dict],
    field: str,
    *,
    max_offset: int = 60,
    top_n: int = 100,
) -> List[Tuple[str, int, float, int]]:
    """Pour un champ, retourne les (page, offset, correlation, n) triés par corrélation décroissante."""
    ocr_series = get_ocr_series(rows, field)
    if all(x != x for x in ocr_series):
        return []
    candidates: List[Tuple[str, int, float, int]] = []
    for page in PAGES:
        max_len = 0
        for row in rows:
            payload = extract_page_bytes((row.get("raw") or {}).get(page))
            max_len = max(max_len, len(payload))
        for offset in range(0, min(max_offset, max_len - 1), 2):
            raw_series = get_raw_series(rows, page, offset)
            corr, n = normalized_correlation(ocr_series, raw_series)
            if n >= 5:
                candidates.append((page, offset, corr, n))
    candidates.sort(key=lambda x: (-x[2], -x[3]))
    return candidates[:top_n]


def target_ocr_value(field: str, ocr_val: float) -> float:
    """Normalise la valeur OCR pour comparaison (ex: desired_egr 6402 → 64.02)."""
    if ocr_val is None:
        return None
    if field in ("desired_egr_position_pct", "egr_position_pct", "accelerator_pct"):
        if ocr_val > 100 and ocr_val < 100000:
            return ocr_val / 100.0  # OCR a lu "64.02" comme 6402
    return float(ocr_val)


def mae_for(
    rows: List[Dict],
    field: str,
    page: str,
    offset: int,
    mult: float,
    div: float,
    add: float = 0.0,
    use_last_known: bool = True,
) -> Tuple[float, int]:
    """Erreur moyenne absolue (avec dernière valeur connue si use_last_known)."""
    err_sum = 0.0
    n = 0
    last_known: Optional[float] = None
    for row in rows:
        ocr_val = (row.get("values") or {}).get(field)
        if ocr_val is None:
            continue
        target = target_ocr_value(field, ocr_val)
        if target is None:
            continue
        raw_hex = (row.get("raw") or {}).get(page)
        payload = extract_page_bytes(raw_hex) if raw_hex else b""
        raw16 = get_raw16(payload, offset)
        decoded: Optional[float] = None
        if raw16 is not None:
            decoded = raw16 * mult / div + add
            if use_last_known:
                last_known = decoded
        elif use_last_known and last_known is not None:
            decoded = last_known
        if decoded is not None:
            err_sum += abs(decoded - target)
            n += 1
    if n == 0:
        return (float("inf"), 0)
    return (err_sum / n, n)


def linear_fit_for(
    rows: List[Dict],
    field: str,
    page: str,
    offset: int,
    use_last_known: bool = True,
) -> Optional[Tuple[float, float, float, int]]:
    """Régression linéaire raw -> OCR, puis MAE avec dernière valeur connue."""
    raws: List[float] = []
    targets: List[float] = []
    for row in rows:
        ocr_val = (row.get("values") or {}).get(field)
        if ocr_val is None:
            continue
        target = target_ocr_value(field, ocr_val)
        if target is None:
            continue
        raw_hex = (row.get("raw") or {}).get(page)
        if not raw_hex:
            continue
        payload = extract_page_bytes(raw_hex)
        raw16 = get_raw16(payload, offset)
        if raw16 is None:
            continue
        raws.append(float(raw16))
        targets.append(target)
    n_fit = len(raws)
    if n_fit < 5:
        return None
    mean_r = sum(raws) / n_fit
    mean_t = sum(targets) / n_fit
    var_r = sum((r - mean_r) ** 2 for r in raws) / n_fit
    if var_r < 1e-15:
        return None
    cov = sum((raws[i] - mean_r) * (targets[i] - mean_t) for i in range(n_fit)) / n_fit
    scale = cov / var_r
    add = mean_t - scale * mean_r
    # MAE en parcourant toutes les lignes avec rétention dernière valeur connue
    err_sum = 0.0
    n_mae = 0
    last_known: Optional[float] = None
    for row in rows:
        ocr_val = (row.get("values") or {}).get(field)
        if ocr_val is None:
            continue
        target = target_ocr_value(field, ocr_val)
        if target is None:
            continue
        raw_hex = (row.get("raw") or {}).get(page)
        payload = extract_page_bytes(raw_hex) if raw_hex else b""
        raw16 = get_raw16(payload, offset)
        decoded: Optional[float] = None
        if raw16 is not None:
            decoded = float(raw16) * scale + add
            if use_last_known:
                last_known = decoded
        elif use_last_known and last_known is not None:
            decoded = last_known
        if decoded is not None:
            err_sum += abs(decoded - target)
            n_mae += 1
    if n_mae == 0:
        return None
    mae = err_sum / n_mae
    return (scale, add, mae, n_mae)


def find_all_candidates(
    rows: List[Dict],
    field: str,
    *,
    max_offset: int = 60,
    top_n: int = 5,
    use_linear_fit: bool = True,
) -> List[Candidate]:
    """Retourne les top_n meilleurs (page, offset, mult, div, add, label, mae, n) triés par MAE."""
    candidates: List[Candidate] = []
    for page in PAGES:
        max_len = 0
        for row in rows:
            payload = extract_page_bytes((row.get("raw") or {}).get(page))
            max_len = max(max_len, len(payload))
        for offset in range(0, min(max_offset, max_len - 1), 2):
            for mult, div, label in FORMULAS:
                mae, n = mae_for(rows, field, page, offset, mult, div, 0.0)
                if n < 5:
                    continue
                candidates.append((page, offset, mult, div, 0.0, label, mae, n))
            if use_linear_fit:
                fit = linear_fit_for(rows, field, page, offset)
                if fit:
                    scale, add, mae, n = fit
                    candidates.append((page, offset, scale, 1.0, add, "linear", mae, n))
    candidates.sort(key=lambda x: (x[6], -x[7]))  # mae asc, then n desc
    return candidates[: top_n]


def find_best_for_field(
    rows: List[Dict],
    field: str,
    *,
    max_offset: int = 60,
    use_linear_fit: bool = True,
) -> Optional[Candidate]:
    """Retourne le meilleur candidat (8-tuple) ou None."""
    cands = find_all_candidates(rows, field, max_offset=max_offset, top_n=1, use_linear_fit=use_linear_fit)
    return cands[0] if cands else None


def assign_no_conflicts(
    rows: List[Dict],
    use_linear_fit: bool = True,
    exclude: Optional[Tuple[str, str, int]] = None,
) -> Dict[str, Candidate]:
    """Attribue à chaque champ un (page, offset) unique. exclude=(field, page, offset) pour forcer un autre choix."""
    used: Dict[Tuple[str, int], str] = {}
    result: Dict[str, Optional[Candidate]] = {}
    by_field: Dict[str, List[Candidate]] = {}
    excl_page, excl_offset = (exclude[1], exclude[2]) if exclude and len(exclude) == 3 else (None, None)
    excl_field = exclude[0] if exclude else None
    for field in FIELDS:
        cands = find_all_candidates(rows, field, top_n=20, use_linear_fit=use_linear_fit)
        if field == excl_field and excl_page is not None:
            cands = [c for c in cands if (c[0], c[1]) != (excl_page, excl_offset)]
        if field in PRIORITY_CANDIDATES and (excl_field != field or (excl_page, excl_offset) != (PRIORITY_CANDIDATES[field][0], PRIORITY_CANDIDATES[field][1])):
            page, offset, mult, div, label = PRIORITY_CANDIDATES[field]
            mae, n = mae_for(rows, field, page, offset, mult, div, 0.0)
            prio: Candidate = (page, offset, mult, div, 0.0, label, mae, n)
            by_field[field] = [prio] + [c for c in cands if (c[0], c[1]) != (page, offset)]
        else:
            by_field[field] = cands
    field_order = sorted(
        FIELDS,
        key=lambda f: (by_field[f][0][6], -by_field[f][0][7]) if by_field[f] else (float("inf"), 0),
    )
    for field in field_order:
        cands = by_field.get(field) or []
        best = None
        for c in cands:
            page, offset = c[0], c[1]
            if (page, offset) not in used:
                used[(page, offset)] = field
                best = c
                break
        result[field] = best
    return {k: v for k, v in result.items() if v is not None}


def assign_by_shape(
    rows: List[Dict],
    use_linear_fit: bool = True,
    exclude: Optional[Tuple[str, str, int]] = None,
) -> Dict[str, Candidate]:
    """
    Attribue (page, offset) par forme du signal: min/max/amplitude, corrélation des séries normalisées,
    indépendante du facteur multiplicateur. Puis régression linéaire pour scale+offset et MAE.
    """
    excl_page, excl_offset = (exclude[1], exclude[2]) if exclude and len(exclude) == 3 else (None, None)
    excl_field = exclude[0] if exclude else None
    by_field: Dict[str, List[Tuple[str, int, float, int]]] = {}
    for field in FIELDS:
        shape_cands = shape_candidates_for_field(rows, field, top_n=80)
        if field == excl_field and excl_page is not None:
            shape_cands = [c for c in shape_cands if (c[0], c[1]) != (excl_page, excl_offset)]
        by_field[field] = shape_cands
    field_order = sorted(
        FIELDS,
        key=lambda f: (by_field[f][0][2], by_field[f][0][3]) if by_field[f] else (-2.0, 0),
        reverse=True,
    )
    used: Dict[Tuple[str, int], str] = {}
    result: Dict[str, Candidate] = {}
    for field in field_order:
        cands = by_field.get(field) or []
        best: Optional[Candidate] = None
        for page, offset, corr, n in cands:
            if (page, offset) in used:
                continue
            if use_linear_fit:
                fit = linear_fit_for(rows, field, page, offset)
                if fit:
                    scale, add, mae, n_mae = fit
                    best = (page, offset, scale, 1.0, add, "linear", mae, n_mae)
            else:
                mae, n_mae = mae_for(rows, field, page, offset, 1, 1, 0.0)
                best = (page, offset, 1.0, 1.0, 0.0, "raw", mae, n_mae)
            if best:
                used[(page, offset)] = field
                result[field] = best
                break
    return result


def assign_hybrid(
    rows: List[Dict],
    use_linear_fit: bool = True,
    mae_zero_threshold: float = 0.0,
    exclude: Optional[Tuple[str, str, int]] = None,
) -> Dict[str, Candidate]:
    """
    Conserve les champs avec MAE <= mae_zero_threshold (1ère passe formula + linear).
    Pour le reste, attribue par forme (corrélation normalisée) + régression linéaire sur slots encore libres.
    """
    results1 = assign_no_conflicts(rows, use_linear_fit=use_linear_fit, exclude=exclude)
    frozen: Dict[str, Candidate] = {
        f: c for f, c in results1.items()
        if c and c[6] <= mae_zero_threshold
    }
    used: Dict[Tuple[str, int], str] = {(c[0], c[1]): f for f, c in frozen.items()}
    rest_fields = [f for f in FIELDS if f not in frozen]
    if not rest_fields:
        return results1
    by_shape: Dict[str, List[Tuple[str, int, float, int]]] = {}
    for field in rest_fields:
        shape_cands = shape_candidates_for_field(rows, field, top_n=80)
        if exclude and exclude[0] == field:
            shape_cands = [c for c in shape_cands if (c[0], c[1]) != (exclude[1], exclude[2])]
        by_shape[field] = shape_cands
    field_order = sorted(
        rest_fields,
        key=lambda f: (by_shape[f][0][2], by_shape[f][0][3]) if by_shape.get(f) else (-2.0, 0),
        reverse=True,
    )
    result: Dict[str, Candidate] = dict(frozen)
    for field in field_order:
        cands = by_shape.get(field) or []
        best: Optional[Candidate] = None
        for page, offset, corr, n in cands:
            if (page, offset) in used:
                continue
            if use_linear_fit:
                fit = linear_fit_for(rows, field, page, offset)
                if fit:
                    scale, add, mae, n_mae = fit
                    best = (page, offset, scale, 1.0, add, "linear", mae, n_mae)
            else:
                mae, n_mae = mae_for(rows, field, page, offset, 1, 1, 0.0)
                best = (page, offset, 1.0, 1.0, 0.0, "raw", mae, n_mae)
            if best:
                used[(page, offset)] = field
                result[field] = best
                break
        if field not in result:
            cands_f = find_all_candidates(rows, field, top_n=30, use_linear_fit=use_linear_fit)
            for c in cands_f:
                if (c[0], c[1]) in used:
                    continue
                used[(c[0], c[1])] = field
                result[field] = c
                break
    return result


def main() -> None:
    ap = argparse.ArgumentParser(description="Dérive le décodeur SZ depuis sz_sync_ocr.jsonl")
    ap.add_argument("jsonl", nargs="?", default="medias/sz_sync_ocr.jsonl", help="Chemin sz_sync_ocr.jsonl")
    ap.add_argument("--update-decode", action="store_true", help="Écrire le décodeur dans sz_decode.h")
    ap.add_argument("--no-linear-fit", action="store_true", help="Désactiver la régression linéaire (scale+offset)")
    ap.add_argument("--write-mapping", action="store_true", help="Écrire sz_decode_mapping.json pour le script compare")
    ap.add_argument("--exclude", metavar="field:page:offset", help="Exclure ce candidat pour field (ex: engine_rpm:21A2:12)")
    ap.add_argument("--limit", type=int, default=0, help="Utiliser seulement les N premières trames (0 = toutes)")
    ap.add_argument("--by-shape", action="store_true", help="Tout apparier par forme (corrélation normalisée)")
    ap.add_argument("--no-freeze", action="store_true", help="Ne pas geler les champs MAE=0 (tout ré-optimiser par formules)")
    args = ap.parse_args()

    path = Path(args.jsonl)
    if not path.exists():
        print(f"Fichier introuvable: {path}", file=sys.stderr)
        sys.exit(1)

    use_linear = not args.no_linear_fit
    exclude: Optional[Tuple[str, str, int]] = None
    if args.exclude:
        parts = args.exclude.split(":")
        if len(parts) == 3:
            exclude = (parts[0].strip(), parts[1].strip().upper(), int(parts[2], 10))
    rows = load_jsonl(str(path))
    if args.limit > 0:
        rows = rows[: args.limit]
        print(f"# {len(rows)} trames (limit={args.limit}) depuis {path} (linear_fit={use_linear})\n")
    else:
        print(f"# {len(rows)} trames chargées depuis {path} (linear_fit={use_linear})\n")

    if getattr(args, "by_shape", False):
        results = assign_by_shape(rows, use_linear_fit=use_linear, exclude=exclude)
        print("# Attribution par forme (min/max/amplitude, corrélation séries normalisées)\n")
    elif not getattr(args, "no_freeze", False):
        results = assign_hybrid(rows, use_linear_fit=use_linear, exclude=exclude)
        print("# Champs MAE=0 conservés ; reste optimisé par forme puis formules\n")
    else:
        results = assign_no_conflicts(rows, use_linear_fit=use_linear, exclude=exclude)
    for field in FIELDS:
        r = results.get(field)
        if r:
            page, offset, mult, div, add, label, mae, n = r
            add_s = f" + {add}f" if add != 0 else ""
            print(f"  {field}: page={page} offset={offset} {label}{add_s}  mae={mae:.3f} n={n}")
        else:
            print(f"  {field}: (aucun fit)")

    if args.update_decode:
        decode_path = Path(__file__).resolve().parent.parent / "esp32" / "sz-mqtt" / "sz_decode.h"
        print(f"\n# Génération du décodeur pour {decode_path}")
        # On génère le bloc decodeSzFromPages à partir de results
        # et on l'écrit dans un fichier .gen pour que tu puisses le coller ou on fait un patch
        gen = []
        gen.append("// Généré par tools/sz_decode_from_ocr_jsonl.py à partir de sz_sync_ocr.jsonl")
        gen.append("// Coller le contenu de decodeSzFromPages (remplacer l’existant) ou appliquer manuellement.")
        gen.append("")
        for field in FIELDS:
            r = results.get(field)
            if not r:
                continue
            page, offset, mult, div, add, label, mae, n = r
            var = "a0" if page == "21A0" else "a2" if page == "21A2" else "a5" if page == "21A5" else "cd"
            len_var = var + "Len"
            if add != 0:
                expr = f"(float)raw * {mult}f + {add}f"
            elif mult == 1 and div == 1:
                expr = "(float)raw"
            elif div == 1:
                expr = f"(float)raw * {mult}f"
            elif mult == 1:
                expr = f"(float)raw / {div}f"
            else:
                expr = f"(float)raw * {mult}f / {div}f"
            gen.append(f"  // {field} (mae={mae:.2f} n={n})")
            gen.append(f"  if ({len_var} > {offset + 1}) {{")
            gen.append(f"    uint16_t raw = ({var}[{offset}] << 8) | {var}[{offset + 1}];")
            gen.append(f"    out.{field} = {expr};")
            gen.append(f"  }}")
            gen.append("")
        out_gen = Path(__file__).resolve().parent.parent / "esp32" / "sz-mqtt" / "sz_decode_generated.txt"
        out_gen.write_text("\n".join(gen), encoding="utf-8")
        print(f"  Snippet: {out_gen}")
        rewrite_sz_decode_h(decode_path, gen)
    if args.write_mapping:
        repo = Path(__file__).resolve().parent.parent
        mapping: Dict[str, Dict[str, Any]] = {}
        for field in FIELDS:
            r = results.get(field)
            if not r:
                continue
            page, offset, mult, div, add, label, mae, n = r
            mapping[field] = {"page": page, "offset": offset, "mult": mult, "div": div, "add": add, "label": label}
        map_path = repo / "tools" / "sz_decode_mapping.json"
        map_path.write_text(json.dumps(mapping, indent=2), encoding="utf-8")
        print(f"  Mapping: {map_path}")


def rewrite_sz_decode_h(decode_path: Path, body_lines: List[str]) -> None:
    """Remplace le corps de decodeSzFromPages dans sz_decode.h par body_lines."""
    text = decode_path.read_text(encoding="utf-8")
    start_marker = "static inline void decodeSzFromPages("
    end_marker = "}"
    if start_marker not in text or "decodeSzFromPages" not in text:
        print("  (sz_decode.h: marqueur non trouvé, pas de remplacement)")
        return
    # Trouver le début du bloc { après la signature
    idx = text.find(start_marker)
    idx_brace = text.find("{", idx)
    if idx_brace == -1:
        return
    # Trouver la fin de la fonction (accolade fermante correspondante)
    depth = 1
    idx_end = idx_brace + 1
    while idx_end < len(text) and depth > 0:
        if text[idx_end] == "{":
            depth += 1
        elif text[idx_end] == "}":
            depth -= 1
        idx_end += 1
    new_body = "\n".join(body_lines)
    # Ne pas copier l'accolade fermante (éviter double }) : on ajoute nous-mêmes
    new_text = text[: idx_brace + 1] + "\n" + new_body + "\n  }\n" + text[idx_end:]
    decode_path.write_text(new_text, encoding="utf-8")
    print(f"  Mis à jour: {decode_path}")


if __name__ == "__main__":
    main()
