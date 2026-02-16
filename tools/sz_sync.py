#!/usr/bin/env python3
"""
Génère un fichier de synchro entre:
 - `medias/trames.log` (dialogue SZ Viewer ↔ vLinker/ELM)
 - `medias/frames/frame_XXXXX.png` (frames extraites du screencast)

Objectif:
 - produire un `jsonl` prêt à annoter/à décoder, avec pour chaque frame:
   - time offset estimé (frame index / fps)
   - seconde "log" alignée (HH:MM:SS) à partir d'un point d'ancrage fourni
   - dernières réponses brutes (hex) 21A0/21A2/21A5/21CD vues à cette seconde
   - un objet `values` (les 20 champs SZ Viewer) initialisé à null

Pourquoi un point d'ancrage?
 - le screencast affiche un timestamp humain (ex: Feb 15 17:36:53),
   mais on ne fait pas d'OCR ici (dépendances).
 - On demande donc à l'utilisateur la correspondance:
     frame_00001.png == 17:36:53
   ensuite on propage sur les frames suivantes.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


LOG_LINE_RE = re.compile(r"^\[(\d{2}:\d{2}:\d{2})\]\s+(send|RECV):\s+(.*)\s*$")


def hhmmss_to_sec(h: str) -> int:
    hh, mm, ss = h.split(":")
    return int(hh) * 3600 + int(mm) * 60 + int(ss)


def sec_to_hhmmss(s: int) -> str:
    s = s % (24 * 3600)
    hh = s // 3600
    mm = (s % 3600) // 60
    ss = s % 60
    return f"{hh:02d}:{mm:02d}:{ss:02d}"


@dataclass
class FrameMapping:
    frame_path: Path
    frame_idx_1based: int
    t_offset_s: float
    log_hhmmss: str


def list_frames(frames_dir: Path) -> List[Path]:
    return sorted(frames_dir.glob("frame_*.png"))


def parse_trames(trames_path: Path) -> Dict[str, Dict[str, str]]:
    """
    Retourne: par seconde (HH:MM:SS) → dict des dernières réponses pour:
      - "21A0", "21A2", "21A5", "21CD"

    Hypothèses:
    - les commandes envoyées sont du type "21A0 1"
    - les réponses ELM arrivent en plusieurs RECV (fragments hex ASCII)
      et se terminent par ">" dans l'ELM; dans le log on voit déjà les
      fragments concaténables.
    - ici on concatène naïvement tous les RECV jusqu'au prochain "send:".
    """
    per_sec: Dict[str, Dict[str, str]] = {}
    current_sec: Optional[str] = None
    current_cmd: Optional[str] = None
    current_recv_parts: List[str] = []

    def flush():
        nonlocal current_cmd, current_recv_parts, current_sec
        if not current_sec or not current_cmd:
            current_cmd = None
            current_recv_parts = []
            return
        cmd_key = current_cmd.upper().strip()
        if cmd_key.startswith("21A0"):
            key = "21A0"
        elif cmd_key.startswith("21A2"):
            key = "21A2"
        elif cmd_key.startswith("21A5"):
            key = "21A5"
        elif cmd_key.startswith("21CD"):
            key = "21CD"
        else:
            current_cmd = None
            current_recv_parts = []
            return

        payload = "".join(p.strip() for p in current_recv_parts if p.strip())
        # Le log contient de l'hex ASCII du transport (ex: "36314130..." = "61A0...")
        # On garde tel quel ici (on décodera plus tard si besoin).
        per_sec.setdefault(current_sec, {})[key] = payload
        current_cmd = None
        current_recv_parts = []

    with trames_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LOG_LINE_RE.match(line)
            if not m:
                continue
            ts, kind, payload = m.group(1), m.group(2), m.group(3)
            current_sec = ts

            if kind == "send":
                # nouvelle commande => flush de la précédente
                flush()
                current_cmd = payload.strip()
            else:
                # RECV: on accumule si on est dans une commande d'intérêt
                if current_cmd and current_cmd.upper().startswith(("21A0", "21A2", "21A5", "21CD")):
                    current_recv_parts.append(payload.strip())

    flush()
    return per_sec


def build_frame_mapping(
    frames: List[Path],
    fps: float,
    anchor_frame_idx_1based: int,
    anchor_log_hhmmss: str,
) -> List[FrameMapping]:
    anchor_sec = hhmmss_to_sec(anchor_log_hhmmss)
    out: List[FrameMapping] = []
    for i, p in enumerate(frames, start=1):
        t_offset_s = (i - 1) / fps
        # différence en secondes entières depuis l'ancre
        delta_s = int(round((i - anchor_frame_idx_1based) / fps))
        log_hhmmss = sec_to_hhmmss(anchor_sec + delta_s)
        out.append(FrameMapping(frame_path=p, frame_idx_1based=i, t_offset_s=t_offset_s, log_hhmmss=log_hhmmss))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trames", default="medias/trames.log", help="Chemin vers trames.log")
    ap.add_argument("--frames", default="medias/frames", help="Dossier frames/frame_*.png")
    ap.add_argument("--out", default="medias/sz_sync.jsonl", help="Sortie jsonl")
    ap.add_argument("--fps", type=float, default=2.0, help="FPS utilisé pour extraire les frames (ex: 2)")
    ap.add_argument("--anchor-frame", type=int, default=1, help="Index 1-based de la frame qui correspond à l'heure d'ancrage")
    ap.add_argument("--anchor-hhmmss", required=True, help="Heure log (HH:MM:SS) correspondant à anchor-frame (ex: 17:36:53)")
    args = ap.parse_args()

    trames_path = Path(args.trames)
    frames_dir = Path(args.frames)
    out_path = Path(args.out)

    frames = list_frames(frames_dir)
    if not frames:
        raise SystemExit(f"Aucune frame trouvée dans {frames_dir}")

    per_sec = parse_trames(trames_path)
    mapping = build_frame_mapping(frames, args.fps, args.anchor_frame, args.anchor_hhmmss)

    # 20 champs attendus (null par défaut)
    values_template = {
        "desired_idle_speed_rpm": None,
        "accelerator_pct": None,
        "intake_c": None,
        "battery_v": None,
        "fuel_temp_c": None,
        "bar_pressure_kpa": None,
        "bar_pressure_mmhg": None,
        "abs_pressure_mbar": None,
        "air_flow_estimate_mgcp": None,
        "air_flow_request_mgcp": None,
        "speed_kmh": None,
        "rail_pressure_bar": None,
        "rail_pressure_control_bar": None,
        "desired_egr_position_pct": None,
        "gear_ratio": None,
        "egr_position_pct": None,
        "engine_temp_c": None,
        "air_temp_c": None,
        "requested_in_pressure_mbar": None,
        "engine_rpm": None,
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as w:
        for fm in mapping:
            raw = per_sec.get(fm.log_hhmmss, {})
            rec = {
                "frame": str(fm.frame_path.as_posix()),
                "frame_idx": fm.frame_idx_1based,
                "t_offset_s": fm.t_offset_s,
                "log_hhmmss": fm.log_hhmmss,
                "raw": {
                    "21A0": raw.get("21A0"),
                    "21A2": raw.get("21A2"),
                    "21A5": raw.get("21A5"),
                    "21CD": raw.get("21CD"),
                },
                "values": dict(values_template),
            }
            w.write(json.dumps(rec, ensure_ascii=False) + "\n")

    print(f"OK: wrote {out_path} ({len(mapping)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

