#!/usr/bin/env python3
"""
OCR des frames SZ Viewer pour remplir automatiquement `values` (20 champs)
dans un jsonl de synchro (ex: `medias/sz_sync.jsonl`).

Dépendances:
 - ffmpeg (pour crop + amélioration contraste)
 - tesseract (OCR), langues: eng (installé sur macOS homebrew)

Sortie:
 - un nouveau fichier jsonl (par défaut `medias/sz_sync_ocr.jsonl`)

Note:
 - UI SZ Viewer est en anglais, donc OCR en `eng` suffit.
 - On garde une approche robuste: parsing par regex sur les labels connus.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, Optional, Tuple


LABEL_MAP = {
    "Desired idle speed": "desired_idle_speed_rpm",
    "Accelerator": "accelerator_pct",
    "Intake": "intake_c",
    "Battery": "battery_v",
    "Fuel temp.": "fuel_temp_c",
    "Bar.pressure": ("bar_pressure_kpa", "bar_pressure_mmhg"),  # 2 occurrences
    "Abs.pressure": "abs_pressure_mbar",
    "Air flow estimate": "air_flow_estimate_mgcp",
    "Air flow request": "air_flow_request_mgcp",
    "Speed": "speed_kmh",
    "Rail pressure": "rail_pressure_bar",
    "Rail pressure control": "rail_pressure_control_bar",
    "Desired EGR position": "desired_egr_position_pct",
    "Gear ratio (speed/rpm)": "gear_ratio",
    "EGR position": "egr_position_pct",
    "Engine": ("engine_temp_c", "engine_rpm"),  # 2 occurrences (temp, rpm)
    "Air temp.": "air_temp_c",
    "Requested in. pressure": "requested_in_pressure_mbar",
}


NUM_RE = re.compile(r"([-+]?\d+(?:\.\d+)?)")


def run(cmd: list[str]) -> str:
    return subprocess.check_output(cmd, text=True)


def ocr_frame(
    frame_path: Path,
    *,
    crop_w: int = 1600,
    crop_h: int = 600,
    crop_x: int = 0,
    crop_y: int = 280,
    contrast: float = 2.2,
    brightness: float = 0.05,
) -> str:
    """
    Retourne le texte OCR pour la zone du tableau SZ Viewer (intégralité des 2 colonnes, 20 lignes).
    """
    with tempfile.TemporaryDirectory(prefix="szocr_") as td:
        out_png = Path(td) / "crop.png"
        vf = f"crop={crop_w}:{crop_h}:{crop_x}:{crop_y},format=gray,eq=contrast={contrast}:brightness={brightness}"
        subprocess.check_call(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(frame_path),
                "-vf",
                vf,
                str(out_png),
            ]
        )
        txt = run(["tesseract", str(out_png), "stdout", "-l", "eng", "--psm", "6"])
        return txt


def parse_value_with_unit(s: str) -> Optional[Tuple[float, str]]:
    """
    Extrait (nombre, unité) d'une sous-chaîne comme:
      "813 rpm", "34.4°C", "100.6 kPa", "998 mbar"
    """
    m = NUM_RE.search(s)
    if not m:
        return None
    v = float(m.group(1))
    unit = s[m.end() :].strip()
    return v, unit


def extract_values(ocr_text: str) -> Dict[str, float]:
    """
    Retourne un dict field->value (float) pour les champs reconnus.
    Gestion des labels présents deux fois (Bar.pressure, Engine).
    """
    # Normalisation légère (garde les lignes, mais on parse par regex global)
    t = ocr_text.replace("\u00b0", "°")

    found: Dict[str, float] = {}
    bar_seen = 0
    engine_seen = 0

    # Stratégie robuste:
    # - on cherche explicitement "Label:" dans tout le texte, indépendamment
    #   des espaces/retours à la ligne (Tesseract peut "coller" les 2 colonnes).
    for label, mapped in LABEL_MAP.items():
        # capture ce qui suit "Label:" jusqu'à fin de ligne
        # (même si la ligne contient 2 colonnes, on récupère au moins le début)
        pattern = re.compile(rf"{re.escape(label)}:\s*([^\n]+)")
        matches = list(pattern.finditer(t))
        if not matches:
            continue

        # Certains labels apparaissent plusieurs fois (Bar.pressure, Engine)
        for m in matches:
            rest = m.group(1).strip()
            pv = parse_value_with_unit(rest)
            if not pv:
                continue
            val, unit = pv

            if isinstance(mapped, tuple):
                if label == "Bar.pressure":
                    if "kPa" in unit and "bar_pressure_kpa" not in found:
                        found[mapped[0]] = val
                        bar_seen += 1
                    elif "mmHg" in unit and "bar_pressure_mmhg" not in found:
                        found[mapped[1]] = val
                        bar_seen += 1
                elif label == "Engine":
                    # UI: "Engine" apparaît 2 fois (colonne droite), ligne 7 = temp, ligne 10 = rpm
                    # 1ère occurrence → engine_temp_c, 2e → engine_rpm
                    if engine_seen == 0:
                        found[mapped[0]] = val
                        engine_seen += 1
                    elif engine_seen == 1:
                        found[mapped[1]] = val
                        engine_seen += 1
                else:
                    # fallback: 1ère cible
                    if mapped[0] not in found:
                        found[mapped[0]] = val
            else:
                found[mapped] = val

    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="medias/sz_sync.jsonl", help="Input jsonl")
    ap.add_argument("--out", dest="out", default="medias/sz_sync_ocr.jsonl", help="Output jsonl")
    ap.add_argument("--limit", type=int, default=0, help="Limiter à N frames (0 = toutes)")
    args = ap.parse_args()

    inp = Path(args.inp)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    n = 0
    with inp.open("r", encoding="utf-8") as r, out.open("w", encoding="utf-8") as w:
        for line in r:
            if not line.strip():
                continue
            rec = json.loads(line)
            frame = Path(rec["frame"])

            try:
                txt = ocr_frame(frame)
                vals = extract_values(txt)
                rec.setdefault("values", {})
                for k, v in vals.items():
                    rec["values"][k] = v
                rec["ocr_ok"] = True
            except Exception as e:
                rec["ocr_ok"] = False
                rec["ocr_error"] = str(e)

            w.write(json.dumps(rec, ensure_ascii=False) + "\n")
            n += 1
            if args.limit and n >= args.limit:
                break

    print(f"OK: wrote {out} ({n} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

