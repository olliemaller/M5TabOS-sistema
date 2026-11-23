#!/bin/bash
# THE M5tabOS MENU - everything you do with the project, without memorising commands.
#
#   ferramentas/menu.sh     (or double-click M5tabOS.command in the Finder,
#                            or the "Menu" button in the VSCode status bar)
#
# Each option calls the usual script - the menu does nothing they do not
# do; it just reminds you which ones exist. An option whose script is not in this copy
# (the public one has no publicar.sh, for example) does not appear.
cd "$(dirname "$0")/.." || exit 1

PUBLICO="../M5TabOS-sistema"   # the GitHub copy (see docs/GITLAB.md)

pausa() { printf "\n(Enter to return to the menu) "; read -r _; }

publicar() {
    if [ ! -d "$PUBLICO/.git" ]; then
        echo "Could not find the public copy at $PUBLICO."; return
    fi
    echo ">>> 1/3 fetching what is already on GitHub"
    git -C "$PUBLICO" pull --ff-only || { echo "the pull failed - sort it out before publishing"; return; }
    echo ">>> 2/3 generating the copy (and building)"
    ferramentas/publicar.sh "$PUBLICO" --compilar || return
    echo ">>> 3/3 what is going to GitHub:"
    git -C "$PUBLICO" show --stat --format="%h %s" | head -40
    printf "\nPush to GitHub now? Type 'sim' (yes) to push: "
    read -r R
    if [ "$R" = "sim" ]; then
        git -C "$PUBLICO" push && echo ">>> published"
    else
        echo "Not pushed. The commit stayed in the local copy; push it later with:"
        echo "  git -C $PUBLICO push"
    fi
}

while true; do
    clear 2>/dev/null
    VERSAO="$(head -1 version.txt 2>/dev/null)"
    RAMO="$(git branch --show-current 2>/dev/null)"
    echo "M5tabOS $VERSAO   (branch: ${RAMO:-?})"
    echo
    OPC=()
    item() { OPC+=("$1"); printf "  %d) %s\n" "${#OPC[@]}" "$2"; }
    item gravar   "Flash the Tab5 (choose the variant)"
    item monitor  "Serial monitor only (no flashing)"
    [ -x ferramentas/compilar_tudo.sh ] && item tudo     "Build all variants (what CI does)"
    [ -x testes/checar_be.sh ]          && item berry    "Check the example Berry apps"
    [ -x ferramentas/m5burner.sh ]      && item m5burner "Generate the M5Burner .bin"
    [ -x ferramentas/lancar.sh ]        && item lancar   "Release a new version"
    [ -x ferramentas/publicar.sh ]      && item publicar "Publish the base to GitHub"
    echo "  0) Quit"
    printf "\nOption: "
    read -r N
    [ "$N" = "0" ] && exit 0
    [[ "$N" =~ ^[0-9]+$ ]] && [ "$N" -ge 1 ] && [ "$N" -le "${#OPC[@]}" ] || continue
    echo
    case "${OPC[$((N-1))]}" in
        gravar)   ./gravar.sh ;;
        monitor)  ( source ./ambiente.sh > /dev/null && idf.py -p "$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)" monitor ) ;;
        tudo)     ferramentas/compilar_tudo.sh; pausa ;;
        berry)    testes/checar_be.sh; pausa ;;
        m5burner)
            printf "Variant (Enter = m5burner; or berry): "; read -r V
            printf "P4 chip (Enter = v1; or v3): "; read -r C
            ferramentas/m5burner.sh ${V:-m5burner} ${C:-v1}; pausa ;;
        lancar)
            echo "patch = fixes only   minor = something new   (rules in docs/VERSIONAMENTO.md)"
            printf "Type (patch/minor): "; read -r T
            [ -n "$T" ] && ferramentas/lancar.sh "$T"
            echo; echo "To push to GitLab: git push origin main --tags"; pausa ;;
        publicar) publicar; pausa ;;
    esac
done
