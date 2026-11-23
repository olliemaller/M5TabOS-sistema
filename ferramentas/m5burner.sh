#!/bin/bash
# BUILDS THE FIRMWARE FOR M5BURNER - a single .bin, flashed from 0x0.
#
#   ferramentas/m5burner.sh              the m5burner variant, P4 v1.x
#   ferramentas/m5burner.sh v3           the m5burner variant, P4 v3.x
#   ferramentas/m5burner.sh berry [v3]   another variant (see docs/VARIANTES.md)
#
# WHAT GOES IN is decided by the variant (variantes/m5burner.cmake: a copy
# of berry - system apps + Berry, with the Ola.be example). Merges bootloader,
# partition table and app into a single file, at the project root (outside git):
#   M5tabOS-<versao>-p4<chip>.bin          (m5burner variant)
#   M5tabOS-<versao>-<variante>-p4<chip>.bin
#
# Flashing this file ERASES the saved settings (the space between the parts is
# filled, NVS included). The C6 firmware (ESP-NOW, Bluetooth) does not go
# with it - M5Burner does not flash the C6.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

VARIANTE=m5burner
[ -f variantes/m5burner.cmake ] || VARIANTE=completa   # the public copy does not have m5burner
CHIP=v1
for a in "$@"; do
    case "$a" in
        v1|v3) CHIP="$a" ;;
        *) VARIANTE="$a" ;;
    esac
done
if [ ! -f "variantes/$VARIANTE.cmake" ]; then
    echo "usage: ferramentas/m5burner.sh [variant] [v1|v3] - variants: $(ls variantes | sed 's/\.cmake//' | tr '\n' ' ')"
    exit 1
fi
source ./ambiente.sh

# The variant's build folder (the same as compilar_tudo.sh and gravar.sh).
PASTA=build_$VARIANTE
[ "$VARIANTE" = "completa" ] && PASTA=build
[ "$CHIP" = "v3" ] && PASTA="${PASTA}_v3"
idf.py -B "$PASTA" -DM5_CHIP="$CHIP" -DM5_VARIANTE="$VARIANTE" build

VERSAO="$(head -1 version.txt | tr -d '[:space:]')"
NOME="M5tabOS-$VERSAO"
[ "$VARIANTE" != "m5burner" ] && NOME="$NOME-$VARIANTE"
SAIDA="$PWD/$NOME-p4$CHIP.bin"
( cd "$PASTA" && esptool.py --chip esp32p4 merge-bin -o "$SAIDA" @flash_args > /dev/null )
echo ">>> $SAIDA ($(du -h "$SAIDA" | cut -f1)) - in M5Burner, flash from 0x0"
