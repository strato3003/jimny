#!/usr/bin/env python3
"""
Parse un log MIM avec timestamps à la milliseconde ([HH:MM:SS.mmm] SEND: / RECV:).
Produit la liste des réponses complètes 21A0, 21A2, 21A5, 21CD avec leur timestamp.

Une réponse est complète quand les fragments RECV concaténés contiennent 0D0D3E (fin ELM).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Generator, List, Optional, Tuple

LOG_LINE_RE = re.compile(r"^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]\s+(SEND|RECV):\s*(.*)\s*$")
PAGE_PREFIXES = ("21A0", "21A2", "21A5", "21CD")
END_MARKER = "0D0D3E"


def hhmmss_ms_to_sec(s: str) -> float:
    """Convertit HH:MM:SS.mmm en secondes depuis minuit."""
    parts = s.strip().split(".")
    hms = parts[0]
    ms = int(parts[1]) if len(parts) > 1 else 0
    h, m, sec = hms.split(":")
    return int(h) * 3600 + int(m) * 60 + int(sec) + ms / 1000.0


def parse_ms_log(log_path: Path) -> Generator[Tuple[float, str, str], None, None]:
    """
    Pour chaque réponse complète (21A0/21A2/21A5/21CD), yield (timestamp_sec, page, hex_payload).
    """
    current_page: Optional[str] = None
    current_hex: List[str] = []
    last_ts_sec: float = 0.0

    with log_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LOG_LINE_RE.match(line.strip())
            if not m:
                continue
            ts_str, kind, payload = m.group(1), m.group(2), (m.group(3) or "").strip()
            ts_sec = hhmmss_ms_to_sec(ts_str)

            if kind == "SEND":
                cmd = payload.upper()
                page = None
                for p in PAGE_PREFIXES:
                    if cmd.startswith(p):
                        page = p
                        break
                if page:
                    current_page = page
                    current_hex = []
                else:
                    current_page = None
                    current_hex = []
            elif kind == "RECV" and current_page:
                # RECV: hex (en majuscules dans le log)
                hex_part = payload.replace(" ", "").upper()
                if not hex_part:
                    continue
                current_hex.append(hex_part)
                last_ts_sec = ts_sec
                full = "".join(current_hex)
                if END_MARKER in full:
                    # Réponse complète : tout jusqu'à et y compris 0D0D3E
                    idx = full.index(END_MARKER) + len(END_MARKER)
                    payload_hex = full[:idx]
                    yield (last_ts_sec, current_page, payload_hex)
                    current_page = None
                    current_hex = []


def main() -> int:
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("recording/jimny_capture.log")
    if not log_path.exists():
        print(f"Fichier introuvable: {log_path}", file=sys.stderr)
        return 1
    n = 0
    for ts_sec, page, hex_payload in parse_ms_log(log_path):
        n += 1
        print(f"{ts_sec:.3f}\t{page}\t{len(hex_payload)}")
    print(f"# {n} réponses complètes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
