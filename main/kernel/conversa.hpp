// CONVERSA - the system's chat component.
//
// Message bubbles, compose bar, virtual keyboard (and the physical one on
// ExtPort1), scrolling and day divider - everything a messaging app needs,
// in one place. Born from the Mesh app (same measurements, colours and rules,
// measured on hardware throughout September), so that ESP-NOW would not be a
// copy of it and the next messaging app would not be a third.
//
// THE APP ONLY ANSWERS THREE QUESTIONS, by function:
//   - how many messages there are     (contar)
//   - what message i is (0 = the oldest)   (ler)
//   - send this text                  (enviar)
// The rest is the component's job. It keeps each bubble's measurements (measuring
// text is expensive) and only re-measures when the app says something changed (conversa_mudou).
//
// HOW TO USE (see app_espnow/app_espnow.cpp):
//   ao_abrir:        conversa_abrir(&s_c, &s_p, contar, ler, enviar, nullptr);
//   ao_fechar:       conversa_fechar(&s_c);
//   ao_aviso:        conversa_mudou(&s_c);        // message arrived, state changed
//   desenhar:        conversa_desenhar(&s_c, d, retangulo, completo, pode_escrever, motivo);
//   ao_tocar:        conversa_tocar(&s_c, x, y);
//   ao_tocar_nivel:  conversa_tocar_nivel(&s_c, x, y, pressionado);
//   ao_teclar:       conversa_teclar(&s_c, texto, codigo, mod);
//
// THE RECTANGLE goes down to the BOTTOM of the app area and takes its full
// width: the virtual keyboard opens over the lower part, and the compose bar
// rises with it. Above it the app puts whatever it likes (the ESP-NOW info
// bar, for example).
#pragma once

#include "app.h"
#include "lista.hpp"
#include "pintura.hpp"
#include "teclado_virtual.hpp"
#include "toque_longo.hpp"

#define CONVERSA_TEXTO        256
// Bytes of ONE drawn line. With accents (2 bytes) and emoji (4), 64 ran out
// before the screen width; 128 covers a full line even of emoji alone.
#define CONVERSA_LINHA_BYTES  128
#define CONVERSA_MAX_LINHAS   16

typedef enum {
  CONVERSA_ENVIO_NADA = 0,   // received (no mark)
  CONVERSA_ENVIO_PENDENTE,
  CONVERSA_ENVIO_OK,
  CONVERSA_ENVIO_FALHOU,
  CONVERSA_ENVIO_SEM_RESPOSTA,   // the timeout expired without the radio saying anything (Mesh)
  CONVERSA_ENVIO_OUVIDO,         // the network heard it, the destination did not confirm (grey tick)
} conversa_envio_t;

typedef struct {
  char     texto[CONVERSA_TEXTO];
  bool     minha;               // on the right, with the send mark
  uint32_t quando;              // UTC epoch; 0 = no date (opens no day divider)
  char     nome[16];            // sender label ("" = no label)
  uint16_t cor_nome;
  char     meta[24];            // small line underneath ("" = none)
  uint16_t cor_meta;
  uint8_t  envio;               // conversa_envio_t
} conversa_msg_t;

typedef int  (*conversa_contar_t)(void* ctx);
typedef bool (*conversa_ler_t)(int i, conversa_msg_t* out, void* ctx);
typedef bool (*conversa_enviar_t)(const char* texto, void* ctx);

struct conversa_balao_s;

typedef struct {
  conversa_contar_t contar;
  conversa_ler_t    ler;
  conversa_enviar_t enviar;
  void*             ctx;
  pintura_t*        p;    // the APP's paint: keyboard opening/closing requests a full pass
  // DRAFT CAP IN BYTES (0 = the whole buffer). The app sets it after
  // conversa_abrir when its protocol accepts less than CONVERSA_TEXTO -
  // the bar stops accepting letters, instead of the message going out truncated.
  int               limite;
  // SEND BUTTON COLOUR (0 = the system default). The app paints it with its
  // own identity - Mesh uses Meshtastic's brand green.
  uint16_t          cor_enviar;
  // TEXT SIZE of the bubbles (0 = 1.0, the value calibrated in Mesh). Message,
  // name and small line grow together; the compose bar stays as it is.
  // After conversa_abrir, via conversa_escala (re-measures everything).
  float             escala;

  // Internal state - the app does not touch it.
  lista_t   lista;
  struct conversa_balao_s* bal;   // PSRAM, 'cap' entries
  int       n, cap, larg;
  bool      medir;                // re-measure the bubbles on the next pass
  area_t    ret;                  // the rectangle of the last draw (screen)
  char      rascunho[CONVERSA_TEXTO];
  tecvirt_t kb;                   // the system's virtual keyboard (fingers, feedback, drawing)
  // COMPOSE BAR. 'cursor' is the offset in BYTES within the draft (always
  // on a UTF-8 character boundary). The line wrap keeps the start of each
  // line (bar_ini[n] = end of text) so a touch on the field can find the
  // position without re-measuring, and is only redone when the text or the
  // width changes - not on every pass.
  int       cursor;
  char      barra_txt[CONVERSA_TEXTO];   // the text from the last pass (for the "changed" check)
  int       barra_base, barra_modo, barra_larg, barra_cursor, barra_topo;
  int       barra_env;                   // the send button as it was painted (lit/dimmed)
  int16_t   bar_ini[CONVERSA_MAX_LINHAS + 2];
  int       bar_n, bar_topo, bar_nl;     // text lines, first visible, how many visible
  int       bar_x0, bar_y0, bar_passo;   // where the first visible line was drawn (screen)
  bool      bar_rascunho;                // what is in the bar is the draft (not the hint)
  lgfx::LovyanGFX* disp;                 // to measure text when the field is touched (UI task only)
  bool      pode_escrever;        // the value from the last draw (touch obeys what is on screen)
  area_t    tec;                  // where the keyboard opens (screen); width 0 = the rectangle itself
  toque_longo_t lp;               // hold on a bubble copies; on the field, pastes
} conversa_t;

void conversa_abrir(conversa_t* c, pintura_t* p, conversa_contar_t contar, conversa_ler_t ler,
                    conversa_enviar_t enviar, void* ctx);
void conversa_fechar(conversa_t* c);
void conversa_mudou(conversa_t* c);
// The text size of the bubbles (see conversa_t.escala).
void conversa_escala(conversa_t* c, float escala);

// 'pode_escrever' false: the bar becomes a notice ('motivo') and opens no keyboard.
void conversa_desenhar(conversa_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool completo, bool pode_escrever, const char* motivo);

// Coordinates in the app AREA - the ones ao_tocar/ao_tocar_nivel receive.
void conversa_tocar(conversa_t* c, int x, int y);
void conversa_tocar_nivel(conversa_t* c, int x, int y, bool pressionado);
void conversa_teclar(conversa_t* c, const char* texto, uint8_t codigo, uint8_t mod);
bool conversa_teclado_aberto(const conversa_t* c);

// THE KEYBOARD WIDER THAN THE CONVERSATION. In Mesh the conversation is only
// the right-hand column, but the keyboard needs the area's full width (a narrow
// key means typos). 'ka' in SCREEN coordinates, with the SAME bottom as the
// conversation rectangle. Call before conversa_desenhar.
void conversa_area_teclado(conversa_t* c, const area_t& ka);

// The conversation shown changed (another channel, another person): closes the
// keyboard, goes back to the end of the list and re-measures. The draft stays - it is the user's, not the conversation's.
void conversa_trocar(conversa_t* c);
void conversa_fechar_teclado(conversa_t* c);

// Wraps text into lines that fit in 'largura_px' (by word; by character only
// when a single word does not fit). Public for anyone drawing running text
// with the same rule as the bubbles (Mesh uses it).
int conversa_quebrar_linhas(lgfx::LovyanGFX& d, const char* txt, int largura_px,
                            char linhas[][CONVERSA_LINHA_BYTES], int max_linhas);
