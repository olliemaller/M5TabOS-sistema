# Generates M5tabOS's SMOOTHED fonts.
#
# Why: the system drew all text with ONE bitmap font (FreeSans 12pt,
# 29 px, 1 bit per pixel) stretched or shrunk by fractions (0.88x, 0.63x...).
# Fractional bitmap scaling duplicates some pixel rows and drops others:
# letters come out jagged and uneven - the typing bar (0.88x) was the worst
# case. Here each size is born NATIVE, drawn by FreeType (via Pillow)
# with 8-bit antialiasing, in the VLW format that LovyanGFX draws by blending
# the letter's edge with the background.
#
# Font: Roboto (Apache 2.0 licence). Regular for the body, Medium for
# bold. With Portuguese accents (the old bitmap font did not have them).
#
# Run: python3 ferramentas/gerar_fontes.py   (generates main/fontes/fontes_suaves.cpp)
import os
from PIL import ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # the project ROOT - the script lives in ferramentas/
SAIDA = os.path.join(AQUI, "main", "fontes", "fontes_suaves.cpp")

FAMILIAS = [
    ("corpo",   os.path.join(AQUI, "ferramentas", "Roboto-Regular.ttf")),
    ("negrito", os.path.join(AQUI, "ferramentas", "Roboto-Medium.ttf")),
]

# LINE HEIGHTS in pixels (what d.fontHeight() returns). The system asks for
# a size in the "old style" N (height = N*8 px, inherited from Font0); the table
# covers 12 to 40 px in steps of 2 and the large ones in steps of 4 - the request lands on the
# nearest, at most 1 px (2 px for the large ones) from what was asked.
ALTURAS = list(range(12, 42, 2)) + [44, 48, 52, 56]

# Printable ASCII + what Portuguese uses + a few useful symbols.
CARACTERES = [chr(c) for c in range(0x20, 0x7F)]
CARACTERES += list("¡ª«°±²³µ·º»¿ÀÁÂÃÄÇÈÉÊËÍÎÏÑÒÓÔÕÖÚÛÜàáâãäçèéêëíîïñòóôõöùúûü×÷")
CARACTERES += list("–—‘’“”•…←↑→↓")


def metricas_ascii(fonte):
    # How LovyanGFX computes the line height: largest ascent and largest descent
    # among the ASCII glyphs (except space) - capital accents are left out
    # of its sum, and here too.
    sobe = desce = 0
    for c in range(0x21, 0x7F):
        esq, topo, dir_, base = fonte.getbbox(chr(c), anchor="ls")
        sobe = max(sobe, -topo)
        desce = max(desce, base)
    return sobe, desce


def fonte_para_altura(caminho, alvo):
    # Looks for the pixel size whose line (ascent+descent) gives the target.
    melhor = None
    for dezimos in range(60, 800):
        tam = dezimos / 10.0
        f = ImageFont.truetype(caminho, tam)
        s, d = metricas_ascii(f)
        if s + d >= alvo:
            melhor = f
            break
    return melhor


def vlw(fonte):
    glifos = []
    for ch in sorted(set(CARACTERES), key=ord):
        if ch != " " and fonte.getbbox(ch, anchor="ls")[2] == 0 and ord(ch) > 0x7E:
            continue
        avanco = int(round(fonte.getlength(ch)))
        if ch == " ":
            glifos.append((ord(ch), 0, 0, avanco, 0, 0, b""))
            continue
        mascara, (ox, oy) = fonte.getmask2(ch, mode="L", anchor="ls")
        w, h = mascara.size
        if w == 0 or h == 0:
            glifos.append((ord(ch), 0, 0, avanco, 0, 0, b""))
            continue
        dados = bytes(mascara)
        # top of the bitmap above the baseline (positive) and left offset
        glifos.append((ord(ch), h, w, avanco, -oy, ox, dados))

    sobe, desce = metricas_ascii(fonte)
    out = bytearray()

    def i32(v):
        out.extend((v & 0xFFFFFFFF).to_bytes(4, "big"))

    i32(len(glifos))       # number of glyphs
    i32(11)                # encoder version (LovyanGFX ignores it)
    i32(sobe + desce)      # "size" - becomes yAdvance, recalculated from the glyphs
    i32(0)
    i32(sobe)              # ascent
    i32(desce)             # descent
    for (u, h, w, av, dy, dx, _) in glifos:
        for v in (u, h, w, av, dy, dx, 0):
            i32(v)
    for g in glifos:
        out.extend(g[6])
    return bytes(out), len(glifos), sobe + desce


def main():
    os.makedirs(os.path.dirname(SAIDA), exist_ok=True)
    linhas = [
        "// GERADO por gerar_fontes.py - nao edite a mao.",
        "// Roboto (Apache 2.0), antialiasing de 8 bits, formato VLW (LovyanGFX).",
        "#include \"fontes_suaves.hpp\"",
        "",
    ]
    tabela = {nome: [] for nome, _ in FAMILIAS}
    total = 0
    for nome, caminho in FAMILIAS:
        for alvo in ALTURAS:
            f = fonte_para_altura(caminho, alvo)
            dados, n, altura = vlw(f)
            total += len(dados)
            simbolo = f"s_{nome}_{alvo}"
            linhas.append(f"static const uint8_t {simbolo}[{len(dados)}] = {{")
            for k in range(0, len(dados), 32):
                linhas.append("  " + ",".join(str(b) for b in dados[k:k + 32]) + ",")
            linhas.append("};")
            tabela[nome].append((alvo, simbolo, len(dados), altura))
            print(f"{nome:8s} target {alvo:3d}px -> line {altura:3d}px, {n} glyphs, {len(dados)/1024:6.1f} KB")

    for nome, itens in tabela.items():
        linhas.append(f"const fonte_suave_t FONTES_{nome.upper()}[] = {{")
        for alvo, simbolo, tam, altura in itens:
            linhas.append(f"  {{ {altura}, {simbolo}, {tam} }},")
        linhas.append("};")
        linhas.append(f"const int FONTES_{nome.upper()}_N = {len(itens)};")
    with open(SAIDA, "w") as arq:
        arq.write("\n".join(linhas) + "\n")
    print(f"total {total/1024:.0f} KB -> {SAIDA}")


main()
