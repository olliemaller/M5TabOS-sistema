#!/bin/bash
# Double-clicking this in the Finder opens the M5tabOS menu in Terminal
# (flash, build, M5Burner, release a version, publish). See ferramentas/menu.sh.
cd "$(dirname "$0")" || exit 1
exec ferramentas/menu.sh
