#!/usr/bin/env python3
"""Résumé des champs décodés du log SZ (NDJSON) pour vérifier cohérence avec la conduite."""
import json
import sys
from collections import defaultdict

def num(x):
    return x if x is not None else float('nan')

def main():
    path = "jimny-2026-02-19-decheterrie-jard.json"
    if len(sys.argv) > 1:
        path = sys.argv[1]

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

    if not rows:
        print("Aucune ligne JSON valide.")
        return

    # Champs décodés à résumer
    rpms = []
    speeds = []
    accel = []
    rail = []
    battery = []
    engine_temp = []
    air_temp = []
    timestamps = []

    for r in rows:
        dt = r.get("datetime", "")
        timestamps.append(dt)
        rpms.append(num(r.get("engine_rpm")))
        speeds.append(num(r.get("speed_kmh")))
        accel.append(num(r.get("accelerator_pct")))
        rail.append(num(r.get("rail_pressure_bar")))
        battery.append(num(r.get("battery_v")))
        engine_temp.append(num(r.get("engine_temp_c")))
        air_temp.append(num(r.get("air_temp_c")))

    def stats(name, vals):
        v = [x for x in vals if x == x and x is not None]
        if not v:
            return "n/a"
        return f"min={min(v):.1f}  max={max(v):.1f}  moy≈{sum(v)/len(v):.1f}  n={len(v)}"

    print("=" * 72)
    print("RÉSUMÉ DÉCODAGE — jimny-2026-02-19-decheterrie-jard.json")
    print("=" * 72)
    print(f"Période: {timestamps[0]} → {timestamps[-1]}")
    print(f"Nombre de trames: {len(rows)}")
    print()
    print("Valeurs décodées (toutes trames):")
    print("  engine_rpm (tr/min)  :", stats("rpm", rpms))
    print("  speed_kmh            :", stats("speed", speeds))
    print("  accelerator_pct      :", stats("accel", accel))
    print("  rail_pressure_bar    :", stats("rail", rail))
    print("  battery_v           :", stats("battery", battery))
    print("  engine_temp_c       :", stats("engine_temp", engine_temp))
    print("  air_temp_c          :", stats("air_temp", air_temp))
    print()

    # Échantillon toutes les ~30 s (environ 1 trame / 1.5 s → 1 point / 20 trames)
    step = max(1, len(rows) // 80)
    print("Échantillon (toutes les ~20 trames) — datetime | engine_rpm | speed_kmh | rail_bar | accel%")
    print("-" * 72)
    for i in range(0, len(rows), step):
        r = rows[i]
        dt = r.get("datetime", "")[-8:]  # HH:MM:SS
        rpm = r.get("engine_rpm")
        spd = r.get("speed_kmh")
        rp = r.get("rail_pressure_bar")
        ac = r.get("accelerator_pct")
        rpm_s = f"{rpm:.0f}" if rpm is not None else "—"
        spd_s = f"{spd:.0f}" if spd is not None else "—"
        rp_s = f"{rp:.1f}" if rp is not None else "—"
        ac_s = f"{ac:.0f}" if ac is not None else "—"
        print(f"  {dt}  {rpm_s:>6}  {spd_s:>5}  {rp_s:>7}  {ac_s:>5}")
    print()

    # Focus 14:43–14:48 (période où tu as poussé à 3000 tr/min)
    print("Focus 14:43 → 14:48 (période ~3000 tr/min annoncée)")
    print("-" * 72)
    for i, r in enumerate(rows):
        dt = r.get("datetime", "")
        if "14:43" not in dt and "14:44" not in dt and "14:45" not in dt and "14:46" not in dt and "14:47" not in dt and "14:48" not in dt:
            continue
        rpm = r.get("engine_rpm")
        spd = r.get("speed_kmh")
        ac = r.get("accelerator_pct")
        rpm_s = f"{rpm:.0f}" if rpm is not None else "—"
        spd_s = f"{spd:.0f}" if spd is not None else "—"
        ac_s = f"{ac:.0f}" if ac is not None else "—"
        print(f"  {dt}  rpm={rpm_s:>6}  speed={spd_s:>5} km/h  accel%={ac_s}")
    print()

    # Répartition RPM (par 500 tr/min)
    rpm_ok = [x for x in rpms if x == x and 0 <= x <= 5000]
    if rpm_ok:
        buckets = defaultdict(int)
        for x in rpm_ok:
            b = int(x // 500) * 500
            buckets[b] += 1
        print("Répartition engine_rpm (par 500 tr/min):")
        for b in sorted(buckets.keys()):
            print(f"  {b:4d}-{b+499:4d} tr/min : {buckets[b]} trames")
    print("=" * 72)

if __name__ == "__main__":
    main()
