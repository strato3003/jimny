import serial
import os
import pty
import select
import time
from datetime import datetime  # Ajout de l'import datetime

# --- CONFIGURATION ---
REAL_VLINKER_PORT = '/dev/rfcomm0'
BAUDRATE = 115200
LOG_FILE = "jimny_capture.log"

def get_ms_timestamp():
    """Retourne le timestamp formaté HH:MM:SS.mmm"""
    now = datetime.now()
    return now.strftime("%H:%M:%S") + f".{now.microsecond // 1000:03d}"

def start_sniffer():
    master, slave = pty.openpty()
    s_name = os.ttyname(slave)
    
    print(f"[*] Port virtuel créé : {s_name}")
    print(f"[*] ACTION : Configure SZ Viewer pour se connecter sur : {s_name}")
    print(f"[*] Les logs seront enregistrés dans : {LOG_FILE}")

    try:
        ser_real = serial.Serial(REAL_VLINKER_PORT, BAUDRATE, timeout=0.1)
        print(f"[*] Connecté au vLinker sur {REAL_VLINKER_PORT}")

        with open(LOG_FILE, "a") as log:
            # Log de l'en-tête avec millisecondes
            log.write(f"\n--- Nouvelle Capture : {get_ms_timestamp()} ---\n")
            
            while True:
                r, w, e = select.select([ser_real, master], [], [])
                
                for fd in r:
                    if fd == ser_real:
                        # Données venant du véhicule -> vers SZ Viewer
                        data = ser_real.read(ser_real.in_waiting or 1)
                        os.write(master, data)
                        if data:
                            ts = get_ms_timestamp() # Utilisation de la nouvelle fonction
                            log.write(f"[{ts}] RECV: {data.hex().upper()}\n")
                            log.flush()
                            
                    elif fd == master:
                        # Données venant de SZ Viewer -> vers le véhicule
                        data = os.read(master, 1024)
                        ser_real.write(data)
                        if data:
                            ts = get_ms_timestamp() # Utilisation de la nouvelle fonction
                            # Note: On garde .hex() ici aussi, car les commandes ELM327 sont souvent de l'ASCII
                            log.write(f"[{ts}] SEND: {data.decode(errors='ignore').strip()}\n")
                            log.flush()

    except serial.SerialException as e:
        print(f"[!] Erreur port série : {e}")
    except KeyboardInterrupt:
        print("\n[*] Arrêt du sniffer.")

if __name__ == "__main__":
    start_sniffer()
