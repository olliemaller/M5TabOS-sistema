#!/bin/bash
# BUILDS ALL VARIANTS - the golden rule of docs/VARIANTES.md: a commit is only
# ready when they all build. There is only one core; a change to it that
# breaks a variant breaks the system.
#   ferramentas/compilar_tudo.sh          the variants, P4 v1
#   ferramentas/compilar_tudo.sh v3       also the P4 v3 ones
# Also checks the example Berry apps (testes/checar_be.sh).
cd "$(dirname "$0")/.." || exit 1
source ./ambiente.sh > /dev/null
CHIPS="v1"
[ "$1" = "v3" ] && CHIPS="v1 v3"
FALHAS=""
for chip in $CHIPS; do
  for v in $(ls variantes | sed 's/\.cmake$//'); do
    PASTA=build
    [ "$v" != "completa" ] && PASTA="${PASTA}_$v"
    [ "$chip" = "v3" ] && PASTA="${PASTA}_v3"
    printf ">>> %-9s P4 %s  (%s/) ... " "$v" "$chip" "$PASTA"
    if [ "$PASTA" = "build" ]; then
      SAIDA=$(idf.py build 2>&1)
    else
      SAIDA=$(idf.py -B "$PASTA" -DM5_CHIP="$chip" -DM5_VARIANTE="$v" build 2>&1)
    fi
    if [ $? -eq 0 ]; then
      echo "ok  $(echo "$SAIDA" | grep -o '[0-9]*%) free' | tail -1)"
    else
      echo "FAILED"
      echo "$SAIDA" | grep -E "error:|Error" | head -10
      FALHAS="$FALHAS $v/$chip"
    fi
  done
done
if [ -x testes/checar_be.sh ] && [ -d berry_exemplos ]; then
  echo ">>> example Berry apps"
  testes/checar_be.sh || FALHAS="$FALHAS berry_exemplos"
fi
# The virtual keyboard (kernel/teclado_virtual + the editor) has a test bench on the
# Mac: the field types, long press, double space and Enter in Notes.
if [ -x testes/rodar_teclado.sh ]; then
  printf ">>> virtual keyboard (test bench) ... "
  if SAIDA=$(testes/rodar_teclado.sh 2>&1); then echo "ok"; else echo "FAILED"; echo "$SAIDA" | grep -E "FALHA|error" | head -10; FALHAS="$FALHAS teclado"; fi
fi
# The GIF decoder (services/tab5_gif_nucleo) too: real and hand-made GIFs
# checked pixel by pixel, and damaged files under ASan/UBSan.
if [ -x testes/rodar_gif.sh ]; then
  printf ">>> GIF decoder (test bench) ... "
  if SAIDA=$(testes/rodar_gif.sh 2>&1); then echo "ok"; else echo "FAILED"; echo "$SAIDA" | grep -E "FALHA|error" | head -10; FALHAS="$FALHAS gif"; fi
fi
if [ -n "$FALHAS" ]; then echo "FAILED:$FALHAS"; exit 1; fi
echo "everything built"
