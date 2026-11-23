#!/usr/bin/env python3
# Generates main/tab5_emoji_dados.cpp with EVERY emoji made of a SINGLE code point -
# 1391 in Unicode version 17.0, 1357 with a drawing in Twemoji. Art from Twemoji (twemoji.twitter.com,
# CC-BY 4.0). Runs once, offline after that - the firmware never downloads anything
# at run time, only the already converted bytes this script leaves
# committed to the repository.
#
#   pip3 install --user Pillow
#   ./gerar_emoji.py
#
# "A SINGLE CODE POINT": the variation selector (U+FE0F) does NOT count - the display
# library itself already discards it before any emoji reaches the callback
# (see LGFXBase.cpp). What is left out are flags (pairs of regional
# indicators), skin tones and ZWJ sequences (family, professions) - each
# one is SEVERAL code points stuck together into a single image, and the
# LovyanGFX hook fires a callback PER CODE POINT. It can be done, but it is another
# project: merging N callback calls into a composite image.
#
# PALETTE FORMAT: each emoji stores its colours (at most 256, and
# none exceeds 237) and a 1-byte index per pixel - 0.95MB instead of
# 1.49MB of raw RGB565, with the same image pixel for pixel. Index 0 is always
# the transparent background.
#
# See main/tab5_emoji.hpp for the format of the generated table and main/tab5_emoji.cpp
# for the callback that consumes it (BINARY search - the table must stay
# SORTED by code point, and this script already guarantees that).

import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

TAM = 24
SENTINELA = 0xF81F  # RGB565 for pure magenta - never appears in a real emoji
FONTE_URL = "https://unicode.org/Public/emoji/latest/emoji-test.txt"
IMG_BASE = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72"

AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # the project ROOT - the script lives in ferramentas/
CACHE = os.path.join(AQUI, ".emoji_cache")
SAIDA = os.path.join(AQUI, "main", "tab5_emoji_dados.cpp")

PADRAO_LINHA = re.compile(
    r'^([0-9A-Fa-f ]+);\s*(\S+)\s*#\s*(\S+)\s*E[\d.]+\s*(.*)$'
)


def buscar_lista_oficial():
    """Returns [(code_point, english_name), ...] sorted by code point - only
    those with ONE code point, fully-qualified (the "complete" and canonical form of
    each emoji; minimally-qualified/unqualified are variants of the same one)."""
    r = subprocess.run(["curl", "-sf", FONTE_URL], capture_output=True, text=True, check=True)
    entradas = {}
    for linha in r.stdout.splitlines():
        if not linha or linha.startswith('#'):
            continue
        m = PADRAO_LINHA.match(linha)
        if not m:
            continue
        codes_s, status, _emoji_char, nome = m.groups()
        if status != 'fully-qualified':
            continue
        codigos = [c for c in codes_s.strip().split(' ') if c.upper() != 'FE0F']
        if len(codigos) != 1:
            continue   # flag, skin tone or ZWJ sequence - out of scope
        cp = int(codigos[0], 16)
        entradas[cp] = nome.strip()
    return sorted(entradas.items())


def baixar_um(cp):
    # curl, not urllib: the Python on this Mac does not have the root certificate
    # configured (a classic python.org installer error), and curl uses the
    # system certificate, which already works.
    nome_arquivo = f"{cp:x}"
    caminho = os.path.join(CACHE, f"{nome_arquivo}.png")
    if os.path.exists(caminho):
        return cp, caminho, None
    url = f"{IMG_BASE}/{nome_arquivo}.png"
    r = subprocess.run(["curl", "-sf", "-o", caminho, url])
    if r.returncode != 0:
        if os.path.exists(caminho):
            os.remove(caminho)   # curl may leave an empty/partial file on error
        return cp, None, f"HTTP failed for U+{cp:04X}"
    return cp, caminho, None


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def trocar_bytes(v):
    # LovyanGFX reads a uint16_t image as RGB565 with SWAPPED bytes
    # (swap565_t) when setSwapBytes is not enabled - which is the system's case.
    # Writing it already swapped makes the colour arrive right without touching the screen state.
    return ((v & 0xFF) << 8) | (v >> 8)


def converter(caminho):
    from PIL import Image
    img = Image.open(caminho).convert("RGBA").resize((TAM, TAM), Image.LANCZOS)
    px = img.load()
    valores = []
    for y in range(TAM):
        for x in range(TAM):
            r, g, b, a = px[x, y]
            if a < 128:
                v = SENTINELA
            else:
                v = rgb565(r, g, b)
                if v == SENTINELA:
                    v ^= 0x0001   # never collide with the real sentinel
            valores.append(trocar_bytes(v))
    return valores


def paletizar(valores):
    """RGB565 pixels (already swapped) -> (palette, indices). The sentinel becomes
    index 0 - the drawing's 'transparent' -, the other colours go in
    in the order they appear."""
    sentinela = trocar_bytes(SENTINELA)
    paleta = [sentinela]
    pos = {sentinela: 0}
    indices = []
    for v in valores:
        if v not in pos:
            pos[v] = len(paleta)
            paleta.append(v)
        indices.append(pos[v])
    if len(paleta) > 256:
        raise ValueError(f"{len(paleta)} colours - does not fit in a 1-byte index")
    return paleta, indices


def escrever(itens):
    """itens = [(code_point, name, RGB565 pixels already swapped), ...] in code
    point order. Writes SAIDA."""
    linhas = [
        "// GERADO por gerar_emoji.py a partir da lista oficial de emoji do",
        "// Unicode (emoji-test.txt) e da arte do Twemoji (CC-BY 4.0,",
        "// twemoji.twitter.com). Nao editar a mao - rode o script de novo.",
        "//",
        "// ORDENADA POR CODIGO DE PROPOSITO: tab5_emoji.cpp busca aqui por",
        "// busca BINARIA, nao varredura - com mais de mil entradas isso importa.",
        "//",
        "// Cada emoji: uma paleta RGB565 (bytes trocados, indice 0 = transparente)",
        "// e um indice de 1 byte por pixel.",
        '#include "tab5_emoji.hpp"',
        "",
    ]
    tabela = []
    total = 0
    for cp, nome, valores in itens:
        paleta, indices = paletizar(valores)
        arr = f"s_emoji_{cp:x}"
        pal = f"s_paleta_{cp:x}"
        linhas.append(f"// U+{cp:04X} - {nome}")
        linhas.append(f"static const uint16_t {pal}[{len(paleta)}] = {{")
        for k in range(0, len(paleta), 16):
            linhas.append("  " + ",".join(f"0x{v:04X}" for v in paleta[k:k + 16]) + ",")
        linhas.append("};")
        linhas.append(f"static const uint8_t {arr}[{TAM}*{TAM}] = {{")
        for k in range(0, len(indices), 24):
            linhas.append("  " + ",".join(str(v) for v in indices[k:k + 24]) + ",")
        linhas.append("};")
        tabela.append((cp, arr, pal))
        total += len(indices) + 2 * len(paleta)

    linhas.append("")
    linhas.append(f"const int TAB5_EMOJI_LADO = {TAM};")
    linhas.append("")
    linhas.append("// ORDENADA por codigo - nao embaralhar. Ver o aviso no topo.")
    linhas.append("const tab5_emoji_t TAB5_EMOJI_TABELA[] = {")
    for cp, arr, pal in tabela:
        linhas.append(f"  {{ 0x{cp:X}, {arr}, {pal} }},")
    linhas.append("};")
    linhas.append(f"const int TAB5_EMOJI_N = {len(tabela)};")

    with open(SAIDA, "w") as f:
        f.write("\n".join(linhas) + "\n")

    print(f"wrote {SAIDA}: {len(tabela)} emoji, {TAM}x{TAM} with palette, "
          f"{total} bytes ({total/1024/1024:.2f} MB)")


def main():
    os.makedirs(CACHE, exist_ok=True)

    print("fetching the official emoji list (unicode.org)...")
    lista = buscar_lista_oficial()
    print(f"{len(lista)} single-code-point emoji")

    print("downloading (20 at a time, cache in .emoji_cache/)...")
    caminhos = {}
    falhas = []
    with ThreadPoolExecutor(max_workers=20) as ex:
        futuros = [ex.submit(baixar_um, cp) for cp, _ in lista]
        feitos = 0
        for fut in as_completed(futuros):
            cp, caminho, erro = fut.result()
            feitos += 1
            if erro:
                falhas.append((cp, erro))
            else:
                caminhos[cp] = caminho
            if feitos % 200 == 0:
                print(f"  {feitos}/{len(lista)}...")

    if falhas:
        print(f"WARNING: {len(falhas)} with no image in Twemoji (skipped):")
        for cp, erro in falhas[:20]:
            print(f"  U+{cp:04X}: {erro}")
        if len(falhas) > 20:
            print(f"  ... and {len(falhas) - 20} more")

    itens = []
    for i, (cp, nome) in enumerate(lista):
        caminho = caminhos.get(cp)
        if not caminho:
            continue
        itens.append((cp, nome, converter(caminho)))
        if (i + 1) % 200 == 0:
            print(f"  converted {i + 1}/{len(lista)}...")

    escrever(itens)


if __name__ == "__main__":
    main()
