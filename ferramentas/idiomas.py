#!/usr/bin/env python3
"""IDIOMAS (languages) - M5tabOS's xgettext/msgmerge, with no dependencies at all.

  ferramentas/idiomas.py          gathers the texts and updates idiomas/*.po
  ferramentas/idiomas.py novo fr "Français"
                                  creates idiomas/fr.po with all texts empty

What goes in: every tr("...") and N_("...") in main/ (adjacent literals are
joined, as in C) and the .nome of each app_t (the drawn name is tr(app->nome)).

idiomas/M5tabOS.pot is the template. In each .po: existing translations stay;
new text goes in with an empty msgstr; text that left the code goes to the end
as a "#~" comment (the translation is not lost if it comes back).

en.po is embedded in the firmware; any other .po is copied to
/sdcard/apps/Sistema/idiomas/ on the SD card (see main/services/idioma.h).
"""
import os
import re
import sys

RAIZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
MAIN = os.path.join(RAIZ, "main")
PASTA = os.path.join(RAIZ, "idiomas")
POT = os.path.join(PASTA, "M5tabOS.pot")

LIT = r'"(?:[^"\\\n]|\\.)*"'
CHAMADA = re.compile(r'\b(?:tr|N_)\(\s*((?:' + LIT + r'\s*)+)\)')
NOME_APP = re.compile(r'\.nome\s*=\s*(' + LIT + r')')


def sem_comentarios(c):
    """Strips // and /* */ outside strings (so examples in comments do not count)."""
    out, i, n = [], 0, len(c)
    while i < n:
        if c[i] == '"':
            j = i + 1
            while j < n and c[j] != '"':
                j += 2 if c[j] == '\\' else 1
            out.append(c[i:j + 1]); i = j + 1
        elif c.startswith('//', i):
            j = c.find('\n', i); i = n if j < 0 else j
        elif c.startswith('/*', i):
            j = c.find('*/', i + 2); i = n if j < 0 else j + 2
        else:
            out.append(c[i]); i += 1
    return ''.join(out)


def juntar(literais):
    return ''.join(m[1:-1] for m in re.findall(LIT, literais))


def extrair():
    textos = {}   # text -> [files], in the order they appear
    for pasta, _, arqs in sorted(os.walk(MAIN)):
        for a in sorted(arqs):
            if not a.endswith(('.cpp', '.hpp', '.h')) or a in ('fontes_suaves.cpp', 'tab5_emoji_dados.cpp'):
                continue
            cam = os.path.join(pasta, a)
            rel = os.path.relpath(cam, RAIZ)
            c = sem_comentarios(open(cam, encoding='utf-8', errors='ignore').read())
            achados = [juntar(m.group(1)) for m in CHAMADA.finditer(c)]
            if os.path.basename(pasta).startswith('app_'):
                achados += [m.group(1)[1:-1] for m in NOME_APP.finditer(c)]
            for t in achados:
                if t and rel not in textos.setdefault(t, []):
                    textos[t].append(rel)
    return textos


def ler_po(cam):
    """{msgid: msgstr} and the header, from a .po (only what this script writes and Poedit saves)."""
    pares, cab, atual, campo = {}, '', None, None
    obsoletos = {}
    for linha in open(cam, encoding='utf-8'):
        linha = linha.rstrip('\n')
        velho = linha.startswith('#~ ')
        if velho:
            linha = linha[3:]
        elif linha.startswith('#'):
            continue
        m = re.match(r'(msgid|msgstr) (' + LIT + ')$', linha)
        if m:
            if m.group(1) == 'msgid':
                atual = {'id': m.group(2)[1:-1], 'str': '', 'velho': velho}
                campo = 'id'
            else:
                campo = 'str'
                atual['str'] = m.group(2)[1:-1]
                if atual['id'] == '':
                    cab = atual['str']
                else:
                    (obsoletos if velho else pares)[atual['id']] = atual['str']
            continue
        if re.match(LIT + '$', linha) and atual is not None:
            atual[campo] += linha[1:-1]
            if campo == 'str':
                if atual['id'] == '':
                    cab = atual['str']
                else:
                    (obsoletos if atual['velho'] else pares)[atual['id']] = atual['str']
    for k, v in obsoletos.items():
        pares.setdefault(k, v)
    return cab, pares


def escrever_po(cam, cab, textos, pares, refs=False):
    """'refs': the "#: file" lines (only in the .pot - the .po is embedded and is smaller without them)."""
    out = ['msgid ""', 'msgstr ""']
    out += ['"' + c + '\\n"' for c in cab.split('\\n') if c]
    out.append('')
    for t, arqs in textos.items():
        if refs:
            out.append('#: ' + ' '.join(arqs))
        out += ['msgid "' + t + '"', 'msgstr "' + pares.get(t, '') + '"', '']
    for t, v in pares.items():
        if t not in textos and v:
            out += ['#~ msgid "' + t + '"', '#~ msgstr "' + v + '"', '']
    open(cam, 'w', encoding='utf-8').write('\n'.join(out))


def main():
    os.makedirs(PASTA, exist_ok=True)
    textos = extrair()
    escrever_po(POT, 'Language: \\nX-Language-Name: \\nContent-Type: text/plain; charset=UTF-8\\n', textos, {}, refs=True)
    if len(sys.argv) >= 4 and sys.argv[1] == 'novo':
        cam = os.path.join(PASTA, sys.argv[2] + '.po')
        if not os.path.exists(cam):
            cab = 'Language: %s\\nX-Language-Name: %s\\nContent-Type: text/plain; charset=UTF-8\\n' % (sys.argv[2], sys.argv[3])
            escrever_po(cam, cab, textos, {})
    print('%d texts in the code' % len(textos))
    for a in sorted(os.listdir(PASTA)):
        if not a.endswith('.po'):
            continue
        cam = os.path.join(PASTA, a)
        cab, pares = ler_po(cam)
        escrever_po(cam, cab, textos, pares)
        feitos = sum(1 for t in textos if pares.get(t))
        print('  %-8s %4d/%d translated' % (a, feitos, len(textos)))


if __name__ == '__main__':
    main()
