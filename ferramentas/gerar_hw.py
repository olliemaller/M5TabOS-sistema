#!/usr/bin/env python3
# Generates main/tab5_mesh_hw_dados.cpp from Meshtastic's official HardwareModel
# enum (meshtastic/protobufs, mesh.proto). Runs once, offline after
# that - the firmware never fetches anything at run time.
#
# WHY THIS EXISTS: the previous table was typed by hand, with some 12
# models that seemed likely. On a real mesh (the one that prompted this,
# ) "model 110", "model 88", "model 81" showed up - real nodes
# the guessed table did not cover. The official source has 149 models; generating
# the whole table from it eliminates the guesswork.
#
#   ./gerar_hw.py
#
# See main/tab5_mesh.cpp (mesh_hw_nome) for where the table is used.

import re
import subprocess
import os

URL = "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto"
AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # the project ROOT - the script lives in ferramentas/
SAIDA = os.path.join(AQUI, "main", "tab5_mesh_hw_dados.cpp")


def titulo(nome_enum: str) -> str:
    # HELTEC_WIRELESS_TRACKER -> "Heltec Wireless Tracker". No dictionary of
    # exceptions on purpose: it is derived, not guessed - the same rule that
    # prompted rewriting this file.
    partes = nome_enum.split("_")
    saida = []
    for p in partes:
        if p.isdigit() or re.match(r'^[0-9]', p):
            saida.append(p)               # "T1000", "900" stay as they are
        else:
            saida.append(p.capitalize())
    return " ".join(saida)


def main():
    r = subprocess.run(["curl", "-sf", URL], capture_output=True, text=True, check=True)
    txt = r.stdout

    m = re.search(r'enum HardwareModel \{(.*?)\n\}', txt, re.S)
    if not m:
        raise SystemExit("could not find the HardwareModel enum in mesh.proto - has the format changed?")

    entradas = re.findall(r'\n\s*([A-Z0-9_]+)\s*=\s*(\d+)\s*;', m.group(1))

    tabela = {}   # code -> pretty name
    for nome_enum, codigo_s in entradas:
        codigo = int(codigo_s)
        if nome_enum == "UNSET":
            continue   # 0 is "no info" - handled separately, does not become a name
        if nome_enum == "PRIVATE_HW":
            tabela[codigo] = "privado"
            continue
        tabela[codigo] = titulo(nome_enum)

    linhas = [
        "// GERADO por gerar_hw.py a partir do HardwareModel oficial do",
        "// Meshtastic (meshtastic/protobufs, mesh.proto). Nao editar a mao -",
        "// rode o script de novo se a lista oficial crescer.",
        '#include "tab5_mesh.hpp"',
        "",
        "// 256 ponteiros, a maioria nulos - endereca direto pelo byte do codigo,",
        "// sem busca. O nulo E o dado: mesh_hw_nome() o devolve para quem chama",
        "// decidir como mostrar um numero que a tabela oficial ainda nao tem.",
        "const char* const TAB5_MESH_HW_NOMES[256] = {",
    ]
    for codigo in range(256):
        nome = tabela.get(codigo)
        if nome is None:
            linhas.append(f"  [{codigo}] = nullptr,")
        else:
            linhas.append(f'  [{codigo}] = "{nome}",')
    linhas.append("};")

    with open(SAIDA, "w") as f:
        f.write("\n".join(linhas) + "\n")

    print(f"wrote {SAIDA}: {len(tabela)} known models out of {len(entradas)} in the enum")


if __name__ == "__main__":
    main()
