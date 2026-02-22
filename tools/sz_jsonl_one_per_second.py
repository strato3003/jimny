#!/usr/bin/env python3
"""
À partir de medias/sz_sync_ocr.jsonl (une ligne par frame), produit un jsonl
avec **une ligne par seconde** (log_hhmmss), en gardant une seule frame
représentative par seconde. Ainsi raw et OCR correspondent au même instant,
ce qui évite de comparer les mêmes trames à plusieurs OCR différents et
réduit le biais sur la MAE (décodé vs OCR).

Usage:
  python3 tools/sz_jsonl_one_per_second.py medias/sz_sync_ocr.jsonl -o medias/sz_sync_ocr_1s.jsonl
  python3 tools/sz_decode_from_ocr_jsonl.py medias/sz_sync_ocr_1s.jsonl --update-decode --write-mapping
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List


def main() -> None:
    ap = argparse.ArgumentParser(description="Réduit le jsonl à une ligne par seconde (une frame représentative)")
    ap.add_argument("jsonl", nargs="?", default="medias/sz_sync_ocr.jsonl", help="Chemin sz_sync_ocr.jsonl")
    ap.add_argument("-o", "--out", default="medias/sz_sync_ocr_1s.jsonl", help="Sortie jsonl (une ligne par seconde)")
    ap.add_argument("--first", action="store_true", help="Garder la première frame de chaque seconde (défaut: première)")
    ap.add_argument("--middle", action="store_true", help="Garder la frame du milieu de chaque seconde")
    args = ap.parse_args()

    path = Path(args.jsonl)
    if not path.exists():
        print(f"Fichier introuvable: {path}", file=sys.stderr)
        sys.exit(1)

    rows: List[Dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue

    # Grouper par log_hhmmss
    by_sec: Dict[str, List[Dict[str, Any]]] = {}
    for r in rows:
        sec = r.get("log_hhmmss")
        if not sec:
            continue
        by_sec.setdefault(sec, []).append(r)

    # Une ligne par seconde: prendre la première ou la frame du milieu
    out_rows: List[Dict[str, Any]] = []
    for hhmmss in sorted(by_sec.keys()):
        group = by_sec[hhmmss]
        if args.middle:
            idx = len(group) // 2
        else:
            idx = 0
        out_rows.append(group[idx])

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as w:
        for r in out_rows:
            w.write(json.dumps(r, ensure_ascii=False) + "\n")

    print(f"# {len(rows)} lignes → {len(out_rows)} lignes (une par seconde)")
    print(f"Écrit: {out_path}")


if __name__ == "__main__":
    main()
