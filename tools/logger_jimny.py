import os, pty, serial, select, sys, time
from datetime import datetime

# CONFIGURATION EXACTE
REAL_PORT = '/dev/cu.vLinkerMC-Android' 
LOG_FILE = "trames.log"

def get_ms_now():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

def main():
    print(f"\n[!] CW TRAINER v36.0 - CONNEXION CIBLE")
    print(f"[!] Cible : {REAL_PORT}")

    if not os.path.exists(REAL_PORT):
        print(f"[!] ERREUR : Le port n'existe pas. Vérifie que le vLinker est jumelé.")
        return

    master, slave = pty.openpty()
    pts_name = os.ttyname(slave)
    
    try:
        # On tente d'ouvrir le port avec un timeout de lecture
        ser = serial.Serial(REAL_PORT, 115200, timeout=0.1)
        print(f"[!] CONNECTÉ AU VLINKER !")
        print(f"[!] CONFIG SZ VIEWER SUR : {pts_name}")

        with open(LOG_FILE, "a") as f:
            f.write(f"\n--- SESSION : {datetime.now()} ---\n")
            while True:
                # On surveille le port virtuel et le port réel
                r, _, _ = select.select([master, ser], [], [], 0.1)
                for src in r:
                    if src == master:
                        data = os.read(master, 1024)
                        ser.write(data)
                        f.write(f"[{get_ms_now()}] SEND -> {data.hex()}\n")
                    elif src == ser:
                        data = ser.read_all()
                        if not data: continue
                        os.write(master, data)
                        f.write(f"[{get_ms_now()}] RECV <- {data.hex()}\n")
                        f.flush()
                        sys.stdout.write(".")
                        sys.stdout.flush()
                        
    except Exception as e:
        print(f"\n[!] Erreur de connexion : {e}")
        print("[!] Conseil : Déconnecte/Reconnecte le vLinker dans le menu Bluetooth du Mac.")

if __name__ == "__main__":
    main()
