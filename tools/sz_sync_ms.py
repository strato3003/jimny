#!/usr/bin/env python3
"""
Synchro trames (log MIM à la milliseconde) ↔ frames vidéo.

Utilise un point d'ancrage : une frame (ex. 1) correspond à un instant log (ex. 17:36:44.038).
Pour chaque frame, on calcule l'instant log associé puis on attribue les dernières
réponses 21A0/21A2/21A5/21CD reçues à ou avant cet instant.

Entrées :
  - Log au format [HH:MM:SS.mmm] SEND: / RECV: (ex. recording/jimny_capture.log)
  - Vidéo (MP4) ou dossier de frames déjà extraites

Sortie : jsonl (une ligne par frame) avec raw par page et values à null (à remplir par sz_ocr).

Usage:
  python3 tools/sz_sync_ms.py --log recording/jimny_capture.log --video recording/2026-02-21_17-50-47.mp4 --fps 30 --anchor-frame 1 --anchor-log 17:52:51 --video-start-sec 125 --video-duration 17 --out recording/sz_sync_ms_window.jsonl
  python3 tools/sz_sync_ms.py --log recording/jimny_capture.log --frames recording/frames --fps 30 --anchor-frame 1 --anchor-log 17:52:51 --out recording/sz_sync_ms.jsonl
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Permettre l'import quand on lance depuis la racine du repo
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sz_parse_ms_log import hhmmss_ms_to_sec, parse_ms_log

VALUES_TEMPLATE = {
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


def build_events(log_path: Path) -> List[Tuple[float, str, str]]:
    """Liste (ts_sec, page, hex) triée par ts_sec."""
    events = list(parse_ms_log(log_path))
    events.sort(key=lambda x: (x[0], x[1]))
    return events


def latest_raw_per_page_at(events: List[Tuple[float, str, str]], at_sec: float) -> Dict[str, Optional[str]]:
    """Pour chaque page, dernière réponse avec ts_sec <= at_sec."""
    out: Dict[str, Optional[str]] = {"21A0": None, "21A2": None, "21A5": None, "21CD": None}
    for ts_sec, page, hex_payload in events:
        if ts_sec <= at_sec:
            out[page] = hex_payload
    return out


def extract_frames_from_video(
    video_path: Path,
    out_dir: Path,
    fps: float,
    start_sec: Optional[float] = None,
    duration_sec: Optional[float] = None,
) -> List[Path]:
    """Extrait les frames (optionnellement un segment [start_sec, start_sec+duration_sec])."""
    out_dir.mkdir(parents=True, exist_ok=True)
    out_pattern = str(out_dir / "frame_%05d.png")
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(video_path),
    ]
    if start_sec is not None and start_sec > 0:
        cmd.extend(["-ss", str(start_sec)])
    if duration_sec is not None and duration_sec > 0:
        cmd.extend(["-t", str(duration_sec)])
    cmd.extend(["-vf", f"fps={fps}", out_pattern])
    subprocess.run(cmd, check=True)
    return sorted(out_dir.glob("frame_*.png"))


def main() -> int:
    ap = argparse.ArgumentParser(description="Synchro log ms ↔ frames (vidéo ou dossier)")
    ap.add_argument("--log", default="recording/jimny_capture.log", help="Log MIM [HH:MM:SS.mmm] SEND/RECV")
    ap.add_argument("--video", help="Vidéo MP4 (extraction frames avec ffmpeg)")
    ap.add_argument("--frames", help="Dossier de frames existantes (frame_*.png)")
    ap.add_argument("--fps", type=float, default=30.0, help="FPS du screencast (ex. 30) pour ancrage et extraction")
    ap.add_argument("--anchor-frame", type=int, default=1, help="Index 1-based de la frame d'ancrage")
    ap.add_argument("--anchor-log", required=True, help="Instant log correspondant (ex. 17:52:51)")
    ap.add_argument("--start-log", help="Début fenêtre log (ex. 17:52:51) — ne garder que les frames dans [start-log, end-log]")
    ap.add_argument("--end-log", help="Fin fenêtre log (ex. 17:53:08)")
    ap.add_argument("--video-start-sec", type=float, default=None, help="Extraire la vidéo à partir de cette seconde (ex. 125 pour 17:52:51 si vidéo commence à 17:50:46)")
    ap.add_argument("--video-duration", type=float, default=None, help="Durée en secondes à extraire (ex. 17 pour 17:52:51→17:53:08)")
    ap.add_argument("--out", default="recording/sz_sync_ms.jsonl", help="Sortie jsonl")
    ap.add_argument("--limit", type=int, default=0, help="Limiter à N frames (0 = toutes)")
    args = ap.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f"Log introuvable: {log_path}", file=sys.stderr)
        return 1

    anchor_ts_sec = hhmmss_ms_to_sec(args.anchor_log)
    start_sec = hhmmss_ms_to_sec(args.start_log) if args.start_log else None
    end_sec = hhmmss_ms_to_sec(args.end_log) if args.end_log else None
    if (args.start_log or args.end_log) and (start_sec is None or end_sec is None):
        print("Indiquer --start-log et --end-log ensemble.", file=sys.stderr)
        return 1

    events = build_events(log_path)
    if not events:
        print("Aucune réponse 21A0/21A2/21A5/21CD dans le log.", file=sys.stderr)
        return 1

    if args.video:
        video_path = Path(args.video)
        if not video_path.exists():
            print(f"Vidéo introuvable: {video_path}", file=sys.stderr)
            return 1
        if args.video_start_sec is not None and args.video_duration is not None:
            frames_dir = video_path.parent / (video_path.stem + f"_frames_{int(args.video_start_sec)}_{int(args.video_duration)}")
        else:
            frames_dir = video_path.parent / (video_path.stem + "_frames")
        frame_paths = extract_frames_from_video(
            video_path, frames_dir, args.fps,
            start_sec=args.video_start_sec,
            duration_sec=args.video_duration,
        )
        print(f"Frames extraites: {len(frame_paths)} dans {frames_dir}", file=sys.stderr)
    elif args.frames:
        frames_dir = Path(args.frames)
        frame_paths = sorted(frames_dir.glob("frame_*.png"))
        if not frame_paths:
            print(f"Aucune frame_*.png dans {frames_dir}", file=sys.stderr)
            return 1
    else:
        print("Indiquer --video ou --frames", file=sys.stderr)
        return 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    written = 0
    with out_path.open("w", encoding="utf-8") as w:
        for i, fp in enumerate(frame_paths, start=1):
            if args.limit and i > args.limit:
                break
            # Instant log pour cette frame : anchor + (i - anchor_frame) / fps
            log_ts_sec = anchor_ts_sec + (i - args.anchor_frame) / args.fps
            if start_sec is not None and end_sec is not None:
                if log_ts_sec < start_sec or log_ts_sec > end_sec:
                    continue
            raw = latest_raw_per_page_at(events, log_ts_sec)
            rec = {
                "frame": str(fp.resolve()),
                "frame_idx": i,
                "t_offset_s": (i - args.anchor_frame) / args.fps,
                "log_ts_sec": round(log_ts_sec, 3),
                "raw": raw,
                "values": dict(VALUES_TEMPLATE),
            }
            w.write(json.dumps(rec, ensure_ascii=False) + "\n")
            written += 1

    print(f"OK: {written} lignes → {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
