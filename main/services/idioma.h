// INTERFACE LANGUAGE - chosen in Settings > Language (and in the welcome wizard).
//
// THE KEY IS THE PORTUGUESE TEXT ITSELF, as in gettext:
//
//     d.drawString(tr("Cancelar"), x, y);
//     snprintf(t, sizeof(t), tr("Remover \"%s\"?"), app->nome);
//
// In Portuguese tr() returns the same pointer, with no lookup. In the others it looks up the
// phrase; A PHRASE WITH NO TRANSLATION COMES OUT IN PORTUGUESE.
//
// EACH LANGUAGE IS A .po FILE (the gettext format - Poedit and any
// translation tool open it), never code:
//   - idiomas/en.po is EMBEDDED in the firmware;
//   - any .po in /sdcard/apps/Sistema/idiomas/ joins the list at the next
//     boot (an fr.po there = French, with nothing compiled). An en.po on the card
//     replaces the embedded one - fixing a translation does not need firmware either.
// The .po header gives the code ("Language: fr") and the name that appears in the
// list ("X-Language-Name: Français"). ferramentas/idiomas.py gathers all the
// tr("...") calls in the code into idiomas/M5tabOS.pot and updates the .po files.
//
// All languages are read ONCE, at boot, into PSRAM, and stay until the
// next boot: switching is instant, and a text returned by tr() never
// ceases to exist.
//
// What does NOT go through here: the serial console, the logs and the app_t's .nome (which
// is also a folder and a command) - the DRAWN name is tr(app->nome).

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define IDIOMAS_PASTA "Sistema/idiomas"   // inside /sdcard/apps (tab5_sd)

// Reads the languages (the embedded one and those on the card) and applies the chosen one. Once, at
// boot, after the card (tab5_sd_iniciar).
void idioma_iniciar(void);

// The list: 0 is always Portuguese.
int         idioma_n(void);
const char* idioma_codigo(int i);   // "pt", "en", "fr"...
const char* idioma_nome(int i);     // in the language itself: "Português", "English"
int         idioma_atual(void);
// Saves and applies at once. Whatever is on screen repaints: the bar notices via the version.
void        idioma_definir(int i);
// Changes on every switch. Whoever paints by difference compares with the one it drew.
uint32_t    idioma_versao(void);

// The text in the current language. 'pt' is the key; nullptr returns nullptr.
const char* tr(const char* pt);

// MARK WITHOUT TRANSLATING: for text kept in a table and translated only at
// drawing time (tr(TABELA[i])). It does nothing - it is what ferramentas/idiomas.py
// looks for, as well as tr("...").
#define N_(s) (s)

// "terça-feira, 21 de outubro" / "Tuesday, October 21" - the order also comes
// from the .po (the phrase "{dia}, {n} de {mes}").
void idioma_data_por_extenso(const struct tm* t, char* out, size_t n);
