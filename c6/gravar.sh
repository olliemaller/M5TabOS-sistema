#!/bin/bash
# Builds and flashes the C6 firmware through the serial programmer (e.g. M5Stack ESP32
# Downloader), with the Tab5 switched OFF - the programmer powers the C6.
# Usage:  c6/gravar.sh
cd "$(dirname "$0")" || exit 1
source ../ambiente.sh

# The programmer's port (CH9102 USB-serial bridge): /dev/cu.usbserial-* on macOS,
# /dev/ttyUSB* on Linux. Not to be confused with the Tab5's (/dev/cu.usbmodem*), which is
# the P4. PORTA=... c6/gravar.sh picks another one.
if [ -z "$PORTA" ]; then
    PORTA=$(ls /dev/cu.usbserial-* /dev/ttyUSB* 2>/dev/null | head -1)
fi
if [ -z "$PORTA" ]; then
    echo "No programmer found. Connected via USB-A? (with USB-C to USB-C it gets no power)"
    exit 1
fi
echo ">>> flashing the C6 on $PORTA"

idf.py -p "$PORTA" flash || exit 1
echo ">>> done. Disconnect the programmer, switch on the Tab5 and run 'farejo info' on the console."
