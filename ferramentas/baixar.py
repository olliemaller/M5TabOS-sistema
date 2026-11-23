#!/usr/bin/env python3
# Copies a file from the Tab5's SD card to the computer, over the same serial port as the monitor.
#
#   ./baixar.py /sdcard/apps/Sistema/energia.csv
#   ./baixar.py                  <- with no argument, downloads energia.csv
#
# On the other side, the one answering is the console's "despejar" (dump) command (console.cpp).
# It silences ESP_LOG while dumping and wraps the content in >>>INICIO and >>>FIM,
# which is what lets us know where it starts, where it ends and whether it all arrived.
#
# CLOSE THE MONITOR FIRST. The port belongs to a single process: with idf.py monitor
# open, this script cannot open the port - or worse, the two each read
# half.

import sys, glob, time, os

# pyserial is not in the system python - it lives in the IDF venv. Instead of
# requiring "source ambiente.sh" first (an easy step to forget), the file itself re-runs with the right
# interpreter on the first import failure. Runs directly, by clicking or from the terminal.
try:
    import serial
except ImportError:
    # The ESP-IDF environment's python (IDF_PYTHON_ENV_PATH, set by
    # export.sh / ambiente.sh).
    IDF_PY = os.path.join(os.environ.get("IDF_PYTHON_ENV_PATH", ""), "bin", "python")
    if os.path.exists(IDF_PY) and os.environ.get("_BAIXAR_REEXEC") != "1":
        os.environ["_BAIXAR_REEXEC"] = "1"
        os.execv(IDF_PY, [IDF_PY, os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit("pyserial is missing. Run:  source ambiente.sh && ./baixar.py")

ORIGEM  = sys.argv[1] if len(sys.argv) > 1 else "/sdcard/apps/Sistema/energia.csv"
# Binaries (photo, firmware) come in base64 ('despejar -b'): plain text breaks
# at the first null byte.
BINARIO = ORIGEM.lower().endswith((".bmp", ".png", ".jpg", ".bin", ".mp3", ".wav"))
import re
B64 = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")
DESTINO = ORIGEM.split("/")[-1]

# TWO TIMEOUTS, and the difference matters.
#
# Before the dump starts, the only reason for silence is something wrong -
# port taken by the monitor, firmware without the command, card missing. Waiting two
# minutes for that only delays the bad news.
#
# Once it has started, a long silence is normal: the Tab5 pauses every 64
# lines so as not to hog the CPU, and the card sometimes stalls for a moment.
ESPERA_INICIO = 8       # until the first response
ESPERA_FLUXO  = 60      # between one line and the next, once dumping

portas = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
if not portas:
    sys.exit("No Tab5 USB port. Is it switched on and connected?")
porta = portas[0]

print(f">>> {porta}: requesting {ORIGEM}")
s = serial.Serial(porta, 115200, timeout=1)
# OPENING THE PORT RESETS THE TAB5 (USB-Serial-JTAG). Wait for the boot to settle -
# 2 s with nothing arriving, at most 20 s - before asking: a request in the middle of
# boot puts boot log in the middle of the file.
fim_boot = time.time() + 20
quieto = time.time()
while time.time() < fim_boot and time.time() - quieto < 2:
    if s.read(4096):
        quieto = time.time()
s.reset_input_buffer()
s.write(f"despejar {'-b ' if BINARIO else ''}{ORIGEM}\n".encode())

linhas   = []
dentro   = False
esperado = None
contadas = None
mudo_ate = time.time() + ESPERA_INICIO

while time.time() < mudo_ate:
    bruto = s.readline()
    if not bruto:
        continue
    mudo_ate = time.time() + (ESPERA_FLUXO if dentro else ESPERA_INICIO)
    linha = bruto.decode("utf-8", "replace").rstrip("\r\n")

    # Firmware without the command. Without this test the script kept waiting for the
    # >>>INICIO that never comes, and the symptom was "it hung" instead of "flash the
    # new firmware" - the same information, delivered too late to be useful.
    if "comando desconhecido" in linha:
        sys.exit("This Tab5 does not have the 'despejar' command yet.\n"
                 "Flash the new firmware:  ./gravar.sh")

    if linha.startswith(">>>INICIO"):
        dentro = True
        partes = linha.split()
        if len(partes) >= 3:
            esperado = int(partes[2])
            print(f"    {esperado} bytes on the way...")
        continue

    if linha.startswith(">>>FIM"):
        contadas = linha.split()[1]
        break

    if linha.startswith("erro:"):
        sys.exit(f"The Tab5 replied: {linha}")

    if dentro:
        # Log from another task that escaped the silencing (e.g. a driver with
        # its own level): a whole log line, not part of the file.
        # Binary: only base64 is part of the file. Log from another task that escaped the
        # silencing, a printf from another service, an empty serial line: out.
        if BINARIO and (not linha or not B64.match(linha)):
            continue
        linhas.append(linha)
        if len(linhas) % 200 == 0:
            print(f"    {len(linhas)} lines", end="\r", flush=True)

s.close()

if contadas is None:
    if not dentro:
        sys.exit("The Tab5 did not respond.\n"
                 "  - is the serial monitor open in another window? "
                 "the port belongs to a single process\n"
                 "  - is the SD card in the device?")
    sys.exit(f"\nThe dump stopped midway ({len(linhas)} lines). "
             "Run it again - a half file is no good for fitting a curve.")

if BINARIO:
    import base64
    # Line by line: each one is 57 whole bytes (76 characters), so
    # a line damaged on the serial link is found and counted, instead of becoming a
    # "padding" error at the end.
    ruins = [i for i, l in enumerate(linhas[:-1]) if len(l) != 76]
    if ruins:
        sys.exit(f"!!! {len(ruins)} line(s) damaged on the serial link (e.g. line {ruins[0]}: "
                 f"{linhas[ruins[0]][:40]!r}) - run it again.")
    dados = b"".join(base64.b64decode(l) for l in linhas)
    open(DESTINO, "wb").write(dados)
    print(f"\n>>> {DESTINO}: {len(dados)} bytes")
    if esperado is not None and len(dados) != esperado:
        # Saved anyway: in a photo, 0.2% missing is a skewed band, and
        # it is still fine to look at. For firmware, NO - run it again.
        sys.exit(f"!!! the Tab5 said {esperado} bytes and {len(dados)} arrived (saved anyway) - "
                 "fine for a photo; for anything else, run it again.")
    sys.exit(0)

corpo = "\n".join(linhas) + "\n"
open(DESTINO, "w").write(corpo)

# CHECKS THE SIZE, and not just the line count. A byte lost on the serial link does not
# erase a whole line - it corrupts a number in the middle, and a curve fitted
# on top of a corrupted number lies without giving any sign.
recebido = len(corpo.encode())
print(f"\n>>> {DESTINO}: {len(linhas)} lines, {recebido} bytes")
if esperado is not None and recebido != esperado:
    print(f"!!! the Tab5 said {esperado} bytes and {recebido} arrived.")
    print("    Probably a lost byte - run it again before using this file.")
