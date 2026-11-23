#!/usr/bin/env python3
# Sends commands to the Tab5 console over serial and returns what it answered.
# Made for measuring without idf.py monitor (and for Claude agents to use).
#
#   ferramentas/serial_cmd.py musica
#   ferramentas/serial_cmd.py "musica" "@10" "musica"       <- @10 = listen for 10 s
#   ferramentas/serial_cmd.py --filtro "secou|musica" --log /tmp/x.txt @60
#   ferramentas/serial_cmd.py --reset @15                     <- restarts and captures the boot
#
# Each argument is a console command line, or a pause "@<seconds>"
# (listening to the log the whole time). After each command it waits for the answer
# to settle (--assentar, 1.5 s with nothing new; at most --max s).
#
#   --log ARQ      writes EVERYTHING that arrived (stdout may be filtered)
#   --filtro RE    on stdout only the matching lines (--log stays complete)
#   --reset        restarts the Tab5 first (as esptool does on USB-Serial-JTAG)
#
# CLOSE THE MONITOR FIRST: the port belongs to a single process.
# Commands that WRITE to NVS (ble ligar, espnow canal, energia sono...) stay
# written - restore the user's value after the test.

import sys, os, glob, time, re, argparse

try:
    import serial
except ImportError:
    for py in (os.path.join(os.environ.get("IDF_PYTHON_ENV_PATH", ""), "bin", "python"),
               os.path.expanduser("~/.m5tabos/idf-5.5-venv/bin/python")):
        if os.path.exists(py) and os.environ.get("_SERIAL_REEXEC") != "1":
            os.environ["_SERIAL_REEXEC"] = "1"
            os.execv(py, [py, os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit("pyserial is missing (IDF venv: ~/.m5tabos/idf-5.5-venv).")

ap = argparse.ArgumentParser()
ap.add_argument("passos", nargs="*")
ap.add_argument("--porta")
ap.add_argument("--log")
ap.add_argument("--filtro")
ap.add_argument("--reset", action="store_true")
ap.add_argument("--assentar", type=float, default=1.5)
ap.add_argument("--max", type=float, default=20.0)
a = ap.parse_args()

portas = [a.porta] if a.porta else sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
if not portas:
    sys.exit("No Tab5 USB port. Is it switched on and connected?")
try:
    s = serial.Serial(portas[0], 115200, timeout=0.2)
except Exception as e:
    sys.exit(f"Could not open {portas[0]}: {e} (is the monitor open?)")

filtro = re.compile(a.filtro) if a.filtro else None
log = open(a.log, "w", encoding="utf-8") if a.log else None
resto = ""

def ler(segundos, assentar=None):
    """Reads for up to 'segundos'; with 'assentar', stops early if it stays quiet that long."""
    global resto
    fim = time.time() + segundos
    quieto = time.time()
    while time.time() < fim:
        b = s.read(4096)
        if b:
            quieto = time.time()
            txt = resto + b.decode("utf-8", "replace").replace("\r", "")
            linhas = txt.split("\n")
            resto = linhas.pop()
            for l in linhas:
                if log: log.write(l + "\n")
                if not filtro or filtro.search(l): print(l, flush=True)
        elif assentar and time.time() - quieto > assentar:
            break

if a.reset:
    s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
    print(">>> restarted", flush=True)
else:
    ler(0.5, 0.3)   # whatever was already in transit
for p in a.passos:
    if p.startswith("@"):
        print(f">>> listening for {p[1:]} s", flush=True)
        ler(float(p[1:]))
        continue
    print(f">>> {p}", flush=True)
    s.write((p + "\r").encode())
    ler(a.max, a.assentar)
ler(0.3, 0.2)
if resto:
    if log: log.write(resto + "\n")
    if not filtro or filtro.search(resto): print(resto)
if log: log.close()
s.close()
