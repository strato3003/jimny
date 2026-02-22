#!/usr/bin/env python3
"""
Itère jusqu'à ce que l'erreur (décodé vs OCR) soit minimale:
  1) Optimise le décodeur (régression linéaire scale+offset) et met à jour sz_decode.h + mapping.
  2) Compare raw décodé vs OCR.
  3) Si max_mae < seuil, on s'arrête. Sinon on exclut le candidat (page, offset) du champ le pire et on réessaie.

Usage:
  python3 tools/sz_iterate_decode_vs_ocr.py medias/sz_sync_ocr.jsonl
  python3 tools/sz_iterate_decode_vs_ocr.py medias/sz_sync_ocr.jsonl --max-iter 5 --mae-threshold 0.01
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def main() -> None:
    jsonl = sys.argv[1] if len(sys.argv) > 1 else "medias/sz_sync_ocr.jsonl"
    max_iter = 10
    mae_threshold = 0.001
    for i, a in enumerate(sys.argv):
        if a == "--max-iter" and i + 1 < len(sys.argv):
            max_iter = int(sys.argv[i + 1])
        elif a == "--mae-threshold" and i + 1 < len(sys.argv):
            mae_threshold = float(sys.argv[i + 1])

    repo = Path(__file__).resolve().parent.parent
    tools = Path(__file__).resolve().parent
    jsonl_path = (repo / jsonl) if not Path(jsonl).is_absolute() else Path(jsonl)
    if not jsonl_path.exists():
        jsonl_path = Path(jsonl)
    if not jsonl_path.exists():
        print(f"Fichier introuvable: {jsonl_path}", file=sys.stderr)
        sys.exit(1)

    exclude_str: str | None = None
    for it in range(max_iter):
        cmd_opt = [
            sys.executable,
            str(tools / "sz_decode_from_ocr_jsonl.py"),
            str(jsonl_path),
            "--update-decode",
            "--write-mapping",
        ]
        if exclude_str:
            cmd_opt.extend(["--exclude", exclude_str])
        r = subprocess.run(cmd_opt, cwd=str(repo), capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr or r.stdout, file=sys.stderr)
            sys.exit(1)

        r2 = subprocess.run(
            [sys.executable, str(tools / "sz_compare_decode_vs_ocr.py"), str(jsonl_path), "--machine"],
            cwd=str(repo),
            capture_output=True,
            text=True,
        )
        if r2.returncode != 0:
            print(r2.stderr or r2.stdout, file=sys.stderr)
            sys.exit(1)

        max_mae = 0.0
        worst_field: str | None = None
        mae_by_field: dict[str, float] = {}
        for line in r2.stdout.strip().splitlines():
            if line.startswith("mae\t"):
                _, field, mae_str = line.split("\t", 2)
                mae = float(mae_str)
                mae_by_field[field] = mae
                if mae > max_mae:
                    max_mae = mae
                    worst_field = field
            elif line.startswith("max_mae\t"):
                max_mae = float(line.split("\t", 1)[1])

        print(f"  Itération {it + 1}: max_mae={max_mae:.4f}" + (f" (pire: {worst_field})" if worst_field else ""))

        if max_mae < mae_threshold:
            print(f"\n# Erreur sous le seuil (max_mae={max_mae:.4f} < {mae_threshold}). Arrêt.")
            break

        if not worst_field or it + 1 >= max_iter:
            print(f"\n# Nombre d'itérations max atteint ou plus d'amélioration. max_mae={max_mae:.4f}")
            break

        mapping_path = tools / "sz_decode_mapping.json"
        if not mapping_path.exists():
            break
        mapping = json.loads(mapping_path.read_text(encoding="utf-8"))
        m = mapping.get(worst_field)
        if not m:
            break
        page = m.get("page", "")
        offset = m.get("offset", 0)
        exclude_str = f"{worst_field}:{page}:{offset}"
        print(f"  Exclure candidat pour {worst_field}: {page} offset {offset}")

    print("\n# Comparaison détaillée (décodé vs OCR):")
    subprocess.run(
        [sys.executable, str(tools / "sz_compare_decode_vs_ocr.py"), str(jsonl_path)],
        cwd=str(repo),
    )


if __name__ == "__main__":
    main()
