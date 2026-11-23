#!/bin/bash
# Builds and flashes M5tabOS onto the Tab5, and opens the serial monitor.
# Usage: ./gravar.sh            asks which variant (Enter = the last one)
#        ./gravar.sh berry      flashes berry directly  (see docs/VARIANTES.md)
# Ctrl+] exits the monitor.
cd "$(dirname "$0")" || exit 1

# WITHOUT A VARIANT, A MENU (the VSCode "Gravar" button and ferramentas/menu.sh
# land here). Each variant describes itself in the "# menu:" line of its file -
# a new variant shows up by itself. Enter repeats the last one flashed (stored
# inside .git, outside the repository).
ULTIMA_ARQ=".git/m5_ultima_variante"
VARIANTE="$1"
if [ -z "$VARIANTE" ]; then
    ULTIMA="$(cat "$ULTIMA_ARQ" 2>/dev/null)"
    [ -f "variantes/$ULTIMA.cmake" ] || ULTIMA=completa
    if [ -t 0 ]; then
        echo "Which variant to flash?"
        NOMES=()
        for f in variantes/*.cmake; do
            n="$(basename "$f" .cmake)"
            NOMES+=("$n")
            printf "  %d) %-9s %s\n" "${#NOMES[@]}" "$n" "$(sed -n 's/^# menu: //p' "$f")"
        done
        printf "Number (Enter = %s): " "$ULTIMA"
        read -r ESC
        if [ -z "$ESC" ]; then
            VARIANTE="$ULTIMA"
        elif [[ "$ESC" =~ ^[0-9]+$ ]] && [ "$ESC" -ge 1 ] && [ "$ESC" -le "${#NOMES[@]}" ]; then
            VARIANTE="${NOMES[$((ESC-1))]}"
        else
            VARIANTE="$ESC"   # also accepts the name
        fi
    else
        VARIANTE="$ULTIMA"
    fi
fi
if [ ! -f "variantes/$VARIANTE.cmake" ]; then
    echo "Variant '$VARIANTE' does not exist. Available: $(ls variantes | sed 's/\.cmake//' | tr '\n' ' ')"
    exit 1
fi
source ./ambiente.sh

# The Tab5's port (the P4's native USB-Serial-JTAG): /dev/cu.usbmodem* on macOS,
# /dev/ttyACM* on Linux. PORTA=... ./gravar.sh picks another one.
if [ -z "$PORTA" ]; then
    PORTA=$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)
fi
if [ -z "$PORTA" ]; then
    echo "No Tab5 USB port found. Is the Tab5 switched on and connected?"
    exit 1
fi

# THE P4 REVISION decides the binary (see sdkconfig.defaults.chip_v1/_v3): a
# v1.x binary does not run on a v3.x, nor the other way round. Read from the
# device itself; CHIP=v1 or CHIP=v3 ./gravar.sh skips the read.
# --after no-reset: after reading, the chip STAYS in download mode. Without it
# it restarted, ran the old program for a moment, and only then did the
# flash go in - a pointless restart on every flash.
if [ -z "$CHIP" ]; then
    REV=$(esptool.py --chip esp32p4 -p "$PORTA" --after no-reset chip-id 2>/dev/null | grep -o "revision v[0-9]*" | head -1)
    case "$REV" in
        "revision v3"*) CHIP=v3 ;;
        "revision v"*)  CHIP=v1 ;;
        *) echo "Could not read the chip revision - using v1 (CHIP=v3 ./gravar.sh forces the other)"; CHIP=v1 ;;
    esac
fi
echo ">>> P4 $CHIP, variant $VARIANTE, flashing on $PORTA"
echo "$VARIANTE" > "$ULTIMA_ARQ" 2>/dev/null || true

# Each combination builds in ITS OWN folder (the sdkconfig lives inside it):
#   build/ (v1 completa)  build_v3/  build_berry/  build_berry_v3/
PASTA=build
[ "$VARIANTE" != "completa" ] && PASTA="${PASTA}_$VARIANTE"
[ "$CHIP" = "v3" ] && PASTA="${PASTA}_v3"

# NO NEW SOURCE, NO BUILD: 'idf.py flash' always runs the build first, and the
# "Compilado em" (built on) stamp (main/carimbo.cmake) changes every minute - every
# flash rebuilt and relinked the whole firmware with nothing having changed. If the
# folder's binary is newer than everything that goes into it, flash what is already
# there. COMPILAR=1 ./gravar.sh forces the build.
BIN="$PASTA/flash_args"
if [ -z "$COMPILAR" ] && [ -f "$BIN" ] && [ -f "$PASTA/M5tabOS.bin" ] && \
   [ -z "$(find main components compartilhado berry_exemplos variantes \
                CMakeLists.txt partitions.csv sdkconfig.defaults* \
                -newer "$PASTA/M5tabOS.bin" -type f -print -quit 2>/dev/null)" ]; then
    echo ">>> nothing changed since the last build in $PASTA/ - flashing without building"
    ( cd "$PASTA" && esptool.py --chip esp32p4 -p "$PORTA" -b 460800 \
          --before default_reset --after hard_reset write_flash "@flash_args" ) || exit 1
    idf.py -B "$PASTA" -p "$PORTA" monitor
    exit $?
fi

if [ "$PASTA" = "build" ]; then
    idf.py -p "$PORTA" flash monitor
else
    idf.py -B "$PASTA" -DM5_CHIP="$CHIP" -DM5_VARIANTE="$VARIANTE" -p "$PORTA" flash monitor
fi
