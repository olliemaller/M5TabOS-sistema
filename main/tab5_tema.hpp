// SYSTEM-WIDE THEME - the colours, corners, spacing and font sizes
// of the WHOLE interface, in a single place.
//
// -------------------------------------------------------------------------
// WHY THIS EXISTS
// -------------------------------------------------------------------------
// Until now each app wrote its own colour by hand (0x18E3, TFT_LIGHTGREY,
// color565(64,64,78)...): over 1,300 lines scattered around. Changing the look of the
// system meant visiting each one. With the theme, the app asks for the colour's ROLE
// ("the background", "the dimmed text", "the accent") and not for the number; changing the
// palette (or gaining a light mode one day) means touching only this file.
//
// The visual mock-up of the redesign uses exactly these values (the link
// is in the 2026-11-15 entry of docs/PASSAGEM.md).
//
// For now NOTHING in the system uses this file: the switch happens bit by
// bit, screen by screen, once the mock-up is approved.
//
// -------------------------------------------------------------------------
// HOW TO USE
// -------------------------------------------------------------------------
//   #include "tab5_tema.hpp"
//   d.fillScreen(TEMA_FUNDO);
//   d.fillRoundRect(x, y, l, a, TEMA_RAIO_M, TEMA_SUPERFICIE);
//   d.setTextColor(TEMA_TEXTO_2);
//
// In Berry, the same colours live in the 'tema' module (tema.FUNDO, tema.TEXTO...).
//
// -------------------------------------------------------------------------
// ABOUT THE COLOURS (RGB565)
// -------------------------------------------------------------------------
// The Tab5 screen works with 16 bits per pixel: 5 bits of red, 6 of
// green and 5 of blue. A "pure" grey written in 24 bits tends to come out
// greenish after rounding (green has one extra bit). That is why
// the dark shades below were chosen ALREADY in 565, checking the final colour;
// the comment on each one shows how it actually appears on screen.
#pragma once

#include <stdint.h>

// Builds a 565 colour from the three already-reduced channels (r 0..31, g 0..63,
// b 0..31). Used only for the hand-picked dark shades.
constexpr uint16_t tema_565(uint8_t r5, uint8_t g6, uint8_t b5)
{
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// Builds a 565 colour from 0xRRGGBB (24 bits), rounding down.
constexpr uint16_t tema_hex(uint32_t rgb)
{
  return (uint16_t)((((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xFF) >> 3));
}

// -------------------------------------------------------------------------
// SURFACES - from the background forwards (each layer one shade lighter)
// -------------------------------------------------------------------------
#define TEMA_FUNDO            tema_565(1, 3, 3)      // #080C18 bluish near-black: the screen
#define TEMA_SUPERFICIE       tema_565(2, 6, 5)      // #101829 card, list row, bar
#define TEMA_SUPERFICIE_ALTA  tema_565(4, 10, 7)     // #202839 card on top of a card, key
#define TEMA_LINHA            tema_565(5, 13, 9)     // #29344A divider and thin border

// -------------------------------------------------------------------------
// TEXT - three levels of importance
// -------------------------------------------------------------------------
#define TEMA_TEXTO            tema_hex(0xF2F5FA)     // primary
#define TEMA_TEXTO_2          tema_hex(0x9AA5B8)     // secondary: caption, supporting value
#define TEMA_TEXTO_3          tema_hex(0x5D687A)     // disabled, hint

// -------------------------------------------------------------------------
// ACCENT - the system's ONLY strong colour (primary button, selection, on)
// -------------------------------------------------------------------------
#define TEMA_DESTAQUE         tema_hex(0x5B8CFF)     // blue
#define TEMA_DESTAQUE_SUAVE   tema_hex(0x1F2F55)     // selected item background
#define TEMA_SOBRE_DESTAQUE   tema_hex(0xFFFFFF)     // text on top of the accent

// -------------------------------------------------------------------------
// STATES - only to communicate something (never as decoration)
// -------------------------------------------------------------------------
#define TEMA_SUCESSO          tema_hex(0x34C77B)     // green: ok, connected, charging
#define TEMA_ALERTA           tema_hex(0xF5B83D)     // yellow: warning, low battery
#define TEMA_PERIGO           tema_hex(0xFF5C5C)     // red: error, delete
#define TEMA_INFO             tema_hex(0x39B8E6)     // cyan: neutral notice

// -------------------------------------------------------------------------
// APP COLOURS - the icon background on the home screen (app_t.cor)
// -------------------------------------------------------------------------
// Shades of equal luminance, so no icon "shouts" louder than another.
#define TEMA_APP_AZUL         tema_hex(0x4C7DFF)
#define TEMA_APP_CIANO        tema_hex(0x22B5D6)
#define TEMA_APP_VERDE        tema_hex(0x2FBF71)
#define TEMA_APP_LIMA         tema_hex(0x9BCB3C)
#define TEMA_APP_AMARELO      tema_hex(0xF2B33D)
#define TEMA_APP_LARANJA      tema_hex(0xF27B3D)
#define TEMA_APP_VERMELHO     tema_hex(0xEF5350)
#define TEMA_APP_ROSA         tema_hex(0xE8608F)
#define TEMA_APP_ROXO         tema_hex(0x9B6BF2)
#define TEMA_APP_GRAFITE      tema_565(12, 27, 16)   // #626D83 system apps

// -------------------------------------------------------------------------
// ROUNDED CORNERS (radius in px)
// -------------------------------------------------------------------------
#define TEMA_RAIO_P   8     // tag, small field
#define TEMA_RAIO_M   16    // button, list row, key
#define TEMA_RAIO_G   24    // card, panel
#define TEMA_RAIO_ICONE 28  // app icon (128 px square)

// -------------------------------------------------------------------------
// SPACING (margin and distance between elements, in px) - always a multiple of 4
// -------------------------------------------------------------------------
#define TEMA_ESPACO_1   4
#define TEMA_ESPACO_2   8
#define TEMA_ESPACO_3   12
#define TEMA_ESPACO_4   16
#define TEMA_ESPACO_5   24
#define TEMA_ESPACO_6   32
#define TEMA_MARGEM     TEMA_ESPACO_5   // distance from the screen edge to the content

// -------------------------------------------------------------------------
// FONT SIZES (line height in px, for tab5_fonte_suave/tab5_titulo)
// -------------------------------------------------------------------------
// All are NATIVE heights of the smoothed Roboto (ferramentas/gerar_fontes.py):
// they never scale.
#define TEMA_LETRA_LEGENDA     20   // small label, time in a list
#define TEMA_LETRA_CORPO       28   // ordinary text
#define TEMA_LETRA_SUBTITULO   32   // item name, card header
#define TEMA_LETRA_TITULO      40   // page title
#define TEMA_LETRA_GRANDE      56   // highlight number (clock, %)

// -------------------------------------------------------------------------
// TOUCH SIZES (px) - a finger needs a big target
// -------------------------------------------------------------------------
#define TEMA_TOQUE_MIN       72    // minimum height of a button or tappable row
#define TEMA_ICONE_APP       128   // app icon side on the home screen
