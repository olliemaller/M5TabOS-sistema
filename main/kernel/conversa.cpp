#include "conversa.hpp"
#include "teclado_virtual.hpp"
#include "tab5_ui.hpp"
#include "tab5_tela.hpp"
#include "tab5_teclado.hpp"
#include "tab5_hora.hpp"
#include "tab5_emoji.hpp"
#include "transferencia.h"
#include "tab5_unicode.hpp"
#include "idioma.h"
#include "tab5_tema.hpp"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// MEASUREMENTS - the Mesh app's, calibrated on hardware throughout September
// (see the notes on TAM_MSG/TAM_ESCRITA/LINHA_STEP in app_mesh/app_mesh.cpp).
// The STEPs are fixed pixel values that follow the font size: change one,
// change the other ("glyph height (size*8) + ~4px of breathing room").
// ---------------------------------------------------------------------------
static const float TAM_MSG     = 3.8f;
static const float TAM_NOME    = 2.415f;
static const float TAM_META    = 1.955f;
static const float TAM_ESCRITA = 3.2f;
static const int   LINHA_STEP  = 34;    // follows TAM_MSG
static const int   DIVISOR_A   = 52;    // follows TAM_DIVISOR (42 px pill)
static const float TAM_DIVISOR = 2.4f;  // "HOJE" (TODAY): 1.7 was too small (user request)
static const int   VAO_BALAO   = 8;
// THE TIME UNDER THE BUBBLE ("14:32"), on the speaker's side. Only when the
// message has a real date - without one (clock not set) there is nothing to
// say, and the strip does not even exist.
static const float TAM_HORA    = 2.3f;  // 1.7 was too small (user request)
static const int   HORA_A      = 28;    // follows TAM_HORA (glyph ~19 px + breathing room)
static const int   ESCRITA_A   = 80;
static const int   ENV_L       = 110;   // send button + margin
static const int   MARCA_L     = 26;    // send mark, to the left of the bubble
static const int   CAP_MAX     = 256;

struct conversa_balao_s {
  uint32_t assin;               // text+name+meta+side+width of the last measurement (see medir)
  int16_t  bl, ba, altura, nome_a, meta_a;
  uint16_t cor_nome, cor_meta;
  bool     minha, inicio_dia;
  uint8_t  n_linhas, envio;
  uint32_t quando;
  char     nome[16];
  char     meta[24];
  char     linhas[CONVERSA_MAX_LINHAS][CONVERSA_LINHA_BYTES];
};
typedef struct conversa_balao_s balao_t;

// Background of the conversation area: the app area's own (the system knows
// which it is), so the bubbles match what is beneath them even in an app still on black.
static uint16_t fundo(lgfx::LovyanGFX&)               { return sistema_cor_fundo(); }
// My bubble in a soft highlight; others' on a surface one step above the background.
static uint16_t cor_balao(lgfx::LovyanGFX&, bool m)   { return m ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA; }

// ---------------------------------------------------------------------------
// LINE WRAPPING
// ---------------------------------------------------------------------------
int conversa_quebrar_linhas(lgfx::LovyanGFX& d, const char* txt, int largura_px,
                            char linhas[][CONVERSA_LINHA_BYTES], int max_linhas)
{
  int n = 0;
  char atual[CONVERSA_LINHA_BYTES] = "";
  const char* p = txt;
  while (*p && n < max_linhas) {
    // NEW LINE in the text (Shift+Enter, the new-line key): close the line
    // here, even if empty.
    if (*p == '\n') {
      snprintf(linhas[n++], CONVERSA_LINHA_BYTES, "%s", atual);
      atual[0] = 0;
      ++p;
      continue;
    }
    const char* fim = p;
    while (*fim && *fim != ' ' && *fim != '\n') { ++fim; }
    size_t tam = (size_t)(fim - p);
    // Word longer than a line's buffer: only what fits goes in (without
    // splitting a UTF-8 character), and the rest comes back round the loop as
    // "the next word" - nothing is discarded.
    const bool cortada = tam >= CONVERSA_LINHA_BYTES;
    if (cortada) {
      tam = CONVERSA_LINHA_BYTES - 1;
      while (tam > 0 && ((unsigned char)p[tam] & 0xC0) == 0x80) { --tam; }
    }
    char palavra[CONVERSA_LINHA_BYTES];
    memcpy(palavra, p, tam);
    palavra[tam] = 0;

    // THE LIMIT IS THE WIDTH, NEVER THE BUFFER. Previously the test built
    // "current + word" in a buffer one line long: when the BYTES ran out
    // before the pixels (accents and emoji take 2-4 bytes), snprintf truncated,
    // the truncated text "fitted", and the end of the word vanished - the line
    // ended mid-screen and the rest appeared nowhere. Now whatever does not
    // fit in the buffer simply moves to the line below.
    const size_t usado = strlen(atual);
    const bool cabe_bytes = usado + (usado ? 1 : 0) + tam < CONVERSA_LINHA_BYTES;
    char teste[CONVERSA_LINHA_BYTES];
    if (cabe_bytes) {
      if (usado) { snprintf(teste, sizeof(teste), "%s %s", atual, palavra); }
      else       { snprintf(teste, sizeof(teste), "%s", palavra); }
    }

    if (!cortada && cabe_bytes && d.textWidth(teste) <= largura_px) {
      memcpy(atual, teste, strlen(teste) + 1);
      p = fim;
      while (*p == ' ') { ++p; }
    } else if (!atual[0]) {
      // Empty line and it still did not fit: cut by character (never in the
      // middle of a UTF-8 sequence) and advance ONLY what went in - the rest of the word carries on.
      memcpy(atual, palavra, tam + 1);
      tab5_cortar_ao_caber(d, atual, largura_px);
      size_t consumido = strlen(atual);
      if (consumido == 0) {   // not even 1 character fitted: advance 1 anyway (never stalls)
        const unsigned char c0 = (unsigned char)palavra[0];
        consumido = ((c0 & 0xE0) == 0xC0) ? 2 : ((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xF8) == 0xF0) ? 4 : 1;
      }
      p += consumido;
    } else {
      snprintf(linhas[n++], CONVERSA_LINHA_BYTES, "%s", atual);
      atual[0] = 0;
    }
  }
  if (atual[0] && n < max_linhas) { snprintf(linhas[n++], CONVERSA_LINHA_BYTES, "%s", atual); }
  return n;
}

// ---------------------------------------------------------------------------
// DAY DIVIDER - "HOJE", "ONTEM", "DD/MM" (today, yesterday; local time zone)
// ---------------------------------------------------------------------------
static int dia_local(uint32_t epoch)
{
  struct tm t;
  tab5_hora_local(epoch, &t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

static void formatar_dia(uint32_t epoch, char* out, size_t tam)
{
  if (const uint32_t agora = tab5_hora_epoch()) {
    if (dia_local(epoch) == dia_local(agora))          { snprintf(out, tam, tr("HOJE"));  return; }
    if (dia_local(epoch) == dia_local(agora - 86400u)) { snprintf(out, tam, tr("ONTEM")); return; }
  }
  struct tm t;
  tab5_hora_local(epoch, &t);
  snprintf(out, tam, "%02d/%02d", t.tm_mday, t.tm_mon + 1);
}

static void desenhar_divisor(lgfx::LovyanGFX& d, int x0, int larg, int y, uint32_t epoch)
{
  char txt[16];
  formatar_dia(epoch, txt, sizeof(txt));
  tab5_texto(d, TAM_DIVISOR);
  const int pl = (int)d.textWidth(txt) + 32;
  const int pa = DIVISOR_A - 10;
  const uint16_t pilula = TEMA_SUPERFICIE_ALTA;
  d.fillRect(x0, y, larg, DIVISOR_A, fundo(d));
  d.fillRoundRect(x0 + (larg - pl) / 2, y + 5, pl, pa, pa / 2, pilula);
  d.setTextDatum(middle_center);
  d.setTextColor(TEMA_TEXTO_2, pilula);
  d.drawString(txt, x0 + larg / 2, y + DIVISOR_A / 2);
  d.setTextDatum(top_left);
}

// Send mark, beside the bubble: three dots (sending), tick (sent), X (failed).
static void marca_envio(lgfx::LovyanGFX& d, int cx, int cy, uint8_t envio)
{
  d.fillRect(cx - MARCA_L / 2, cy - 12, MARCA_L, 24, fundo(d));
  if (envio == CONVERSA_ENVIO_PENDENTE) {
    for (int k = -1; k <= 1; ++k) { d.fillCircle(cx + k * 7, cy, 2, TEMA_TEXTO_3); }
  } else if (envio == CONVERSA_ENVIO_OK) {
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TEMA_SUCESSO);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TEMA_SUCESSO);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TEMA_SUCESSO);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TEMA_SUCESSO);
  } else if (envio == CONVERSA_ENVIO_OUVIDO) {
    // The same tick, in grey: it went out and the network heard it, but the
    // one who should confirm (the recipient) has not confirmed yet.
    d.drawLine(cx - 8, cy, cx - 3, cy + 6, TEMA_TEXTO_3);
    d.drawLine(cx - 8, cy + 1, cx - 3, cy + 7, TEMA_TEXTO_3);
    d.drawLine(cx - 3, cy + 6, cx + 8, cy - 7, TEMA_TEXTO_3);
    d.drawLine(cx - 3, cy + 7, cx + 8, cy - 6, TEMA_TEXTO_3);
  } else if (envio == CONVERSA_ENVIO_SEM_RESPOSTA) {
    // Grey, circle and "?": not a failure, just not knowing.
    d.drawCircle(cx, cy, 9, TEMA_TEXTO_3);
    d.drawCircle(cx, cy, 8, TEMA_TEXTO_3);
    const auto e = d.getTextStyle();
    d.setFont(tab5_fonte_suave(true, 16));
    d.setTextDatum(middle_center);
    d.setTextColor(TEMA_TEXTO_3);
    d.drawString("?", cx, cy + 1);
    d.setTextStyle(e);
    tab5_fonte_corpo(d);
  } else if (envio == CONVERSA_ENVIO_FALHOU) {
    d.drawLine(cx - 6, cy - 6, cx + 6, cy + 6, TEMA_PERIGO);
    d.drawLine(cx - 6, cy + 6, cx + 6, cy - 6, TEMA_PERIGO);
    d.drawLine(cx - 5, cy - 6, cx + 7, cy + 6, TEMA_PERIGO);
    d.drawLine(cx - 5, cy + 6, cx + 7, cy - 6, TEMA_PERIGO);
  }
}

// ---------------------------------------------------------------------------
// MEASURE - once per change, never per frame
// ---------------------------------------------------------------------------
// Only what changed is re-measured. An incoming message or a delivery
// confirmation (which only swaps the mark) used to re-measure ALL bubbles - up
// to 256, wrapping word by word - on the UI task: if it landed mid-typing,
// that frame was late and the key stuttered. Each bubble keeps the signature
// of what defines it (text, name, meta, side, width); if equal, only the cheap
// fields (mark, date) are updated. A shifted index (history that scrolled)
// gives a different signature and is re-measured - never wrongly reused.
//
// Returns true when the STRUCTURE changed (count or height of some bubble):
// then the whole list re-lays out. Otherwise, only the bubbles that changed
// (typically the send mark) are noted for repainting.
// The TEXT SIZE of the bubbles (conversa_t.escala; 0 = 1). The line step
// follows the font, as the MEASUREMENTS rule above requires.
static float esc(const conversa_t* c)        { return c->escala > 0 ? c->escala : 1.0f; }
static float tam_msg(const conversa_t* c)    { return TAM_MSG * esc(c); }
static float tam_nome(const conversa_t* c)   { return TAM_NOME * esc(c); }
static float tam_meta(const conversa_t* c)   { return TAM_META * esc(c); }
static int   linha_step(const conversa_t* c) { return (int)(LINHA_STEP * esc(c) + 0.5f); }

static int hora_a(const balao_t* b) { return tab5_hora_valida(b->quando) ? HORA_A : 0; }

static uint32_t assinatura(const conversa_msg_t* m, int larg)
{
  uint32_t h = 2166136261u;   // FNV-1a
  auto mais = [&](const char* s) { for (; *s; ++s) { h = (h ^ (uint8_t)*s) * 16777619u; } h = (h ^ 0xFF) * 16777619u; };
  mais(m->texto); mais(m->nome); mais(m->meta);
  h = (h ^ (uint32_t)m->minha) * 16777619u;
  h = (h ^ (uint32_t)larg) * 16777619u;
  return h | 1u;   // 0 = never measured
}

static bool medir(conversa_t* c, lgfx::LovyanGFX& d, int larg)
{
  c->larg = larg;
  c->medir = false;
  const int n_antes = c->n;
  int n = c->contar ? c->contar(c->ctx) : 0;
  if (n > CAP_MAX) { n = CAP_MAX; }
  if (n > c->cap) {
    const int nova = ((n + 31) / 32) * 32;
    void* p = heap_caps_realloc(c->bal, sizeof(balao_t) * nova, MALLOC_CAP_SPIRAM);
    if (!p) { c->n = 0; return true; }
    c->bal = (balao_t*)p;
    c->cap = nova;
  }
  c->n = n;

  const auto estilo = d.getTextStyle();
  tab5_texto(d, tam_nome(c));
  const int alt_nome = d.fontHeight();
  tab5_texto(d, tam_meta(c));
  const int alt_meta = d.fontHeight();
  const int respiro = linha_step(c) / 4;

  bool estrutura = (n != n_antes);
  int dia_ant = 0;
  static conversa_msg_t m;   // 300+ bytes: kept off the UI task's stack
  for (int i = 0; i < n; ++i) {
    balao_t* b = &c->bal[i];
    memset(&m, 0, sizeof(m));
    if (!c->ler(i, &m, c->ctx)) { m.texto[0] = 0; }
    const int16_t alt_antes = (i < n_antes) ? b->altura : -1;
    if (i < n_antes && (b->envio != m.envio || b->quando != m.quando
                    || b->cor_nome != m.cor_nome || b->cor_meta != m.cor_meta)) { lista_item_mudou(&c->lista, i); }
    b->envio = m.envio;
    b->quando = m.quando;
    b->cor_nome = m.cor_nome;
    b->cor_meta = m.cor_meta;
    const uint32_t as = assinatura(&m, larg ^ ((int)(esc(c) * 100) << 16));   // the scale is part of the measurement too
    if (i < n_antes && b->assin == as) {
      // Same content: measurements stand. Only the day divider, which depends on the neighbour.
      b->inicio_dia = false;
      if (tab5_hora_valida(b->quando)) {
        const int dia = dia_local(b->quando);
        b->inicio_dia = (dia != dia_ant);
        dia_ant = dia;
      }
      b->altura = (int16_t)((b->inicio_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
      if (b->altura != alt_antes) { estrutura = true; }
      continue;
    }
    if (i < n_antes) { lista_item_mudou(&c->lista, i); }
    b->assin = as;
    b->minha = m.minha;
    snprintf(b->nome, sizeof(b->nome), "%s", m.nome);
    snprintf(b->meta, sizeof(b->meta), "%s", m.meta);

    // Width cap: 38% for mine (beside the mark), 60% for received ones.
    const int pct = (int)(larg * (b->minha ? 0.38f : 0.60f));
    const int teto = (pct < larg - 28 - MARCA_L) ? pct : (larg - 28 - MARCA_L);

    char txt[CONVERSA_TEXTO];
    tab5_emoji_preparar(m.texto, txt, sizeof(txt));
    tab5_texto(d, tam_msg(c));
    b->n_linhas = (uint8_t)conversa_quebrar_linhas(d, txt, teto - 24, b->linhas, CONVERSA_MAX_LINHAS);

    int bl = 24;
    for (int k = 0; k < b->n_linhas; ++k) {
      const int lw = (int)d.textWidth(b->linhas[k]) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->nome[0]) {
      tab5_texto(d, tam_nome(c));
      const int lw = (int)d.textWidth(b->nome) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (b->meta[0]) {
      tab5_texto(d, tam_meta(c));
      const int lw = (int)d.textWidth(b->meta) + 24;
      if (lw > bl) { bl = lw; }
    }
    if (bl > teto) { bl = teto; }

    b->nome_a = b->nome[0] ? (int16_t)(alt_nome + respiro) : 0;
    b->meta_a = b->meta[0] ? (int16_t)(alt_meta + respiro) : 0;
    b->bl = (int16_t)bl;
    b->ba = (int16_t)(24 + b->n_linhas * linha_step(c) + b->nome_a + b->meta_a);

    // Divider before the first message of each day. No date (clock not
    // set when it arrived) opens no day - otherwise it would become 31/12/1969.
    b->inicio_dia = false;
    if (tab5_hora_valida(b->quando)) {
      const int dia = dia_local(b->quando);
      b->inicio_dia = (dia != dia_ant);
      dia_ant = dia;
    }
    b->altura = (int16_t)((b->inicio_dia ? DIVISOR_A : 0) + b->ba + hora_a(b) + VAO_BALAO);
    if (b->altura != alt_antes) { estrutura = true; }
  }
  d.setTextStyle(estilo);
  return estrutura;
}

static int balao_altura(int i, void* ctx)
{
  const conversa_t* c = (const conversa_t*)ctx;
  return (c->bal && i < c->n) ? c->bal[i].altura : 0;
}

static void balao_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int larg, void* ctx)
{
  const conversa_t* c = (const conversa_t*)ctx;
  if (!c->bal || i >= c->n) { return; }
  const balao_t* b = &c->bal[i];
  const auto estilo = d.getTextStyle();

  if (b->inicio_dia) { desenhar_divisor(d, x0, larg, y, b->quando); y += DIVISOR_A; }

  const int bx = b->minha ? (x0 + larg - b->bl - 16) : (x0 + 16);
  const uint16_t cor = cor_balao(d, b->minha);
  // A tall bubble gets the large corner; a short one (one line) the medium, so it does not become a pill.
  const int raio = (b->ba >= 4 * TEMA_RAIO_G) ? TEMA_RAIO_G : TEMA_RAIO_M;
  d.fillRoundRect(bx, y, b->bl, b->ba, raio, cor);

  // Clip to the bubble itself (inside the strip's clip): an emoji at the end of
  // the line does not paint the gap up to the next one.
  int32_t cx, cy, cl, ca;
  d.getClipRect(&cx, &cy, &cl, &ca);
  const int rx0 = (bx > cx) ? bx : cx;
  const int ry0 = (y > cy) ? y : cy;
  const int rx1 = (bx + b->bl < cx + cl) ? (bx + b->bl) : (cx + cl);
  const int ry1 = (y + b->ba < cy + ca) ? (y + b->ba) : (cy + ca);
  if (rx1 > rx0 && ry1 > ry0) {
    d.setClipRect(rx0, ry0, rx1 - rx0, ry1 - ry0);
    d.setTextDatum(top_left);
    int cur = y + 14;
    if (b->nome[0]) {
      tab5_texto(d, tam_nome(c));
      d.setTextColor(b->cor_nome ? b->cor_nome : (uint16_t)TEMA_DESTAQUE, cor);
      d.drawString(b->nome, bx + 12, cur);
      cur += b->nome_a;
    }
    tab5_texto(d, tam_msg(c));
    d.setTextColor(TEMA_TEXTO, cor);
    for (int k = 0; k < b->n_linhas; ++k) { d.drawString(b->linhas[k], bx + 12, cur + k * linha_step(c)); }
    cur += b->n_linhas * linha_step(c) + linha_step(c) / 4;
    if (b->meta[0]) {
      tab5_texto(d, tam_meta(c));
      d.setTextColor(b->cor_meta ? b->cor_meta : (uint16_t)TEMA_TEXTO_2, cor);
      d.drawString(b->meta, bx + 12, cur);
    }
    d.setClipRect(cx, cy, cl, ca);
  }
  if (b->minha) { marca_envio(d, bx - 8 - MARCA_L / 2, y + b->ba / 2, b->envio); }

  if (hora_a(b)) {
    struct tm t;
    tab5_hora_local(b->quando, &t);
    char hora[8];
    tab5_hora_hhmm(&t, hora, sizeof(hora));
    const int hy = y + b->ba;
    d.fillRect(x0, hy, larg, HORA_A, fundo(d));
    tab5_texto(d, TAM_HORA);
    d.setTextColor(TEMA_TEXTO_3, fundo(d));
    d.setTextDatum(b->minha ? top_right : top_left);
    d.drawString(hora, b->minha ? bx + b->bl - 6 : bx + 6, hy + 3);
    d.setTextDatum(top_left);
  }
  d.setTextStyle(estilo);
}

// ---------------------------------------------------------------------------
// LIFE CYCLE
// ---------------------------------------------------------------------------
void conversa_abrir(conversa_t* c, pintura_t* p, conversa_contar_t contar, conversa_ler_t ler,
                    conversa_enviar_t enviar, void* ctx)
{
  balao_t* bal = c->bal;   // the cache survives reopening (same app)
  const int cap = c->cap;
  memset(c, 0, sizeof(*c));
  c->bal = bal; c->cap = cap;
  c->contar = contar; c->ler = ler; c->enviar = enviar; c->ctx = ctx; c->p = p;
  c->medir = true;
  c->barra_base = c->barra_modo = c->barra_larg = c->barra_cursor = c->barra_topo = c->barra_env = -1;
  lista_definir(&c->lista, ROLAGEM_FIM, 0, balao_altura, balao_desenhar, c);
}

void conversa_fechar(conversa_t* c)
{
  if (c->bal) { heap_caps_free(c->bal); }
  c->bal = nullptr; c->cap = 0; c->n = 0;
  tecvirt_fechar(&c->kb);
}

void conversa_mudou(conversa_t* c) { c->medir = true; sistema_pedir_redesenho(); }

void conversa_escala(conversa_t* c, float escala)
{
  if (escala == c->escala) { return; }
  c->escala = escala;
  c->medir = true;   // each bubble's signature includes the scale: all re-measure
  if (c->p) { pintura_pedir(c->p); }
  sistema_pedir_redesenho_completo();
}

bool conversa_teclado_aberto(const conversa_t* c) { return tecvirt_aberto(&c->kb); }

void conversa_area_teclado(conversa_t* c, const area_t& ka) { c->tec = ka; }

// The keyboard rectangle: the one the app gave, or the conversation's own.
static const area_t& area_tec(const conversa_t* c) { return c->tec.largura > 0 ? c->tec : c->ret; }

// Reposition everything (keyboard opened/closed): full pass, through both paths
// the repaint rule requires (app paint + system schedule).
static void pedir_completo(conversa_t* c)
{
  if (c->p) { pintura_pedir(c->p); }
  sistema_pedir_redesenho_completo();
}

static void fechar_teclado(conversa_t* c)
{
  if (!tecvirt_aberto(&c->kb)) { return; }
  tecvirt_fechar(&c->kb);
  pedir_completo(c);
}

void conversa_fechar_teclado(conversa_t* c) { fechar_teclado(c); }

void conversa_trocar(conversa_t* c)
{
  fechar_teclado(c);
  c->medir = true;
  lista_ao_inicio(&c->lista);
  pedir_completo(c);
}

// ---------------------------------------------------------------------------
// DRAFT EDITING - always at the cursor, always on a UTF-8 character boundary
// ---------------------------------------------------------------------------
static int utf8_anterior(const char* s, int i) { return tab5_utf8_anterior(s, i); }
static int utf8_seguinte(const char* s, int i) { return tab5_utf8_seguinte(s, i); }

static void cursor_ajustar(conversa_t* c)
{
  const int n = (int)strlen(c->rascunho);
  if (c->cursor > n) { c->cursor = n; }
  if (c->cursor < 0) { c->cursor = 0; }
}

static void inserir(conversa_t* c, const char* t, size_t n)
{
  cursor_ajustar(c);
  const size_t len = strlen(c->rascunho);
  const size_t pedido = n;   // 't' has EXACTLY n bytes - it is not a terminated string
  size_t teto = sizeof(c->rascunho) - 1;
  if (c->limite > 0 && (size_t)c->limite < teto) { teto = (size_t)c->limite; }
  if (len >= teto) { return; }
  if (len + n > teto) { n = teto - len; }
  // Never cut in the middle of a character: back off to the boundary. Only
  // looks within the 'pedido' bytes received. This used to be strlen(t) - and
  // the virtual keyboard key arrives as ONE loose char on the stack, with no
  // terminating zero: strlen read the garbage after it, and when the next byte
  // happened to look like a UTF-8 continuation (0x80-0xBF) the letter was
  // discarded. It depended on what was left on the stack - hence "out of nowhere".
  while (n > 0 && n < pedido && ((unsigned char)t[n] & 0xC0) == 0x80) { --n; }
  if (!n) { return; }
  memmove(c->rascunho + c->cursor + n, c->rascunho + c->cursor, len - c->cursor + 1);
  memcpy(c->rascunho + c->cursor, t, n);
  c->cursor += (int)n;
}

static void apagar_antes(conversa_t* c)
{
  cursor_ajustar(c);
  if (!c->cursor) { return; }
  const int a = utf8_anterior(c->rascunho, c->cursor);
  memmove(c->rascunho + a, c->rascunho + c->cursor, strlen(c->rascunho) - c->cursor + 1);
  c->cursor = a;
}

static void enviar_rascunho(conversa_t* c)
{
  if (!c->rascunho[0] || !c->enviar) { return; }
  if (c->enviar(c->rascunho, c->ctx)) {
    c->rascunho[0] = 0;
    c->cursor = 0;
    lista_ao_inicio(&c->lista);   // the new message appears at the bottom: go to it
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// WRAPPING THE COMPOSE BAR, with offsets
// ---------------------------------------------------------------------------
// Unlike conversa_quebrar_linhas (which returns strings and swallows the
// spaces), this returns WHERE each line starts in the original text: a touch on
// the field must turn "x, y" into a cursor position, and the cursor needs an
// exact place even between spaces. The lines partition the text - the space
// left over at the end of a line belongs to it.
// ini[k] is the start of line k; ini[n] = end of text. Returns n (>= 1).
static int quebrar_offsets(lgfx::LovyanGFX& d, const char* s, int largura_px, int16_t* ini, int max)
{
  const int len = (int)strlen(s);
  int n = 0;
  int i = 0;
  ini[n++] = 0;
  while (i < len && n < max) {
    // Grow the line word by word; 'bom' = end of the last word that fitted.
    int bom = i;
    int j = i;
    for (;;) {
      int fim = j;
      while (s[fim] && s[fim] != ' ' && s[fim] != '\n') { ++fim; }
      int prox = fim;
      while (s[prox] == ' ') { ++prox; }
      if (fim == j && !s[fim]) { j = len; break; }
      char teste[CONVERSA_LINHA_BYTES];
      const int tam = fim - i;
      const bool cabe_bytes = tam < CONVERSA_LINHA_BYTES;
      bool cabe = false;
      if (cabe_bytes) {
        memcpy(teste, s + i, tam); teste[tam] = 0;
        cabe = (int)d.textWidth(teste) <= largura_px;
      }
      // The new line (the \n) ends the line and stays in it.
      if (cabe && s[fim] == '\n') { bom = fim + 1; break; }
      if (cabe) { bom = prox; j = prox; if (!s[prox]) { break; } }
      else { break; }
    }
    if (bom == i) {
      // Not even the first word fitted: cut by character.
      int e = i;
      char teste[CONVERSA_LINHA_BYTES];
      for (;;) {
        const int nx = utf8_seguinte(s, e);
        const int tam = nx - i;
        if (nx == e || tam >= CONVERSA_LINHA_BYTES) { break; }
        memcpy(teste, s + i, tam); teste[tam] = 0;
        if ((int)d.textWidth(teste) > largura_px && e > i) { break; }
        e = nx;
        if (!s[e]) { break; }
      }
      if (e == i) { e = utf8_seguinte(s, i); }   // never stalls
      bom = e;
    }
    i = bom;
    if (i < len) { ini[n++] = (int16_t)i; }
  }
  // Text ending in \n: the end cursor sits on a new, empty line.
  if (len > 0 && s[len - 1] == '\n' && n < max) { ini[n++] = (int16_t)len; }
  ini[n] = (int16_t)len;
  return n;
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
// SEND BUTTON: circle with the paper plane (arrow pointing right with the
// notch behind), as in phone messengers. Lit only with a draft: with no text
// there is nothing to send, and the dimmed button says so before the touch.
// Drawn with primitives - the system font does not have the glyph.
static void botao_enviar(lgfx::LovyanGFX& d, int x, int y, int l, int a, uint16_t cor,
                         bool aceso, uint16_t fundo)
{
  tab5_tela_limpar(x, y, l, a, fundo);
  const int raio = (l < a ? l : a) / 2;
  const int cx = x + l / 2, cy = y + a / 2;
  d.fillCircle(cx, cy, raio, aceso ? cor : (uint16_t)TEMA_SUPERFICIE_ALTA);
  const uint16_t tinta = aceso ? (uint16_t)TEMA_SOBRE_DESTAQUE : (uint16_t)TEMA_TEXTO_3;
  // Proportions as fractions of the radius; the tip pulls the drawing 1/12 to
  // the right so it looks centred (the visual weight sits behind, in the notch).
  const int ox = cx + raio / 12;
  const int px = ox + raio * 11 / 20;               // tip
  const int tx = ox - raio * 10 / 20;               // tail (the two wings)
  const int ty = raio * 10 / 20;                    // half height of the wings
  const int nx = ox - raio * 4 / 20;                // bottom of the notch
  d.fillTriangle(px, cy, tx, cy - ty, nx, cy, tinta);
  d.fillTriangle(px, cy, nx, cy, tx, cy + ty, tinta);
}

void conversa_desenhar(conversa_t* c, lgfx::LovyanGFX& d, const area_t& r,
                       bool completo, bool pode_escrever, const char* motivo)
{
  c->ret = r;
  c->pode_escrever = pode_escrever;
  if (!pode_escrever) { fechar_teclado(c); }

  const int base = r.y + r.altura - ESCRITA_A - (tecvirt_aberto(&c->kb) ? tecvirt_altura(area_tec(c)) : 0);
  const uint16_t FUNDO = fundo(d);
  if (completo) { tab5_tela_limpar(r.x, r.y, r.largura, base - r.y, FUNDO); }

  if (c->medir || r.largura != c->larg) {
    if (medir(c, d, r.largura)) { lista_itens_mudaram(&c->lista, c->n); }
  }

  const area_t janela = { r.x, r.y, r.largura, base - r.y };
  lista_desenhar(&c->lista, d, janela, FUNDO, completo);
  if (c->n == 0 && completo) {
    tab5_texto(d, 3.0f);
    d.setTextDatum(middle_center);
    d.setTextColor(TEMA_TEXTO_3, FUNDO);
    d.drawString(tr("nenhuma mensagem ainda"), r.x + r.largura / 2, r.y + (base - r.y) / 2);
    d.setTextDatum(top_left);
  }

  // ---- compose bar: only repaints what changed -----------------------------
  const char* fonte = !pode_escrever ? (motivo ? motivo : "")
                    : c->rascunho[0] ? c->rascunho
                    // The hint tells the truth of the case: with a keyboard docked the touch
                    // opens nothing (does not steal half the screen).
                    : (tab5_teclado_presente() ? tr("digite no teclado; ENTER envia") : tr("toque para escrever"));
  const int campo_l = r.largura - 24 - ENV_L;
  const bool eh_rascunho = pode_escrever && c->rascunho[0];
  c->disp = &d;
  cursor_ajustar(c);

  // THE WRAP IS ONLY REDONE WHEN THE TEXT OR THE WIDTH CHANGES. Previously
  // every pass re-wrapped and re-measured the whole draft - and a pass happens
  // when the finger goes down on the key, on release and on each letter, that
  // is, three times per letter typed.
  const pintura_t* p = c->p;
  const bool m_txt  = pintura_mudou_s(p, c->barra_txt, sizeof(c->barra_txt), fonte);
  const bool m_larg = pintura_mudou_i(p, &c->barra_larg, campo_l);
  tab5_texto(d, TAM_ESCRITA);
  if (m_txt || m_larg) {
    c->bar_n = quebrar_offsets(d, fonte, campo_l - 24, c->bar_ini, CONVERSA_MAX_LINHAS);
  }
  c->bar_rascunho = eh_rascunho;

  // Cursor line and a TWO-line window containing it. The window only moves
  // when the cursor leaves it - editing mid-text does not make the bar jump.
  int lc = c->bar_n - 1;
  if (eh_rascunho) {
    lc = 0;
    while (lc + 1 < c->bar_n && c->bar_ini[lc + 1] <= c->cursor) { ++lc; }
  }
  int topo = c->bar_topo;
  if (!eh_rascunho) { topo = 0; }
  else if (lc < topo) { topo = lc; }
  else if (lc > topo + 1) { topo = lc - 1; }
  if (topo > c->bar_n - 2) { topo = c->bar_n - 2; }
  if (topo < 0) { topo = 0; }
  c->bar_topo = topo;
  const int nl = (c->bar_n - topo) > 2 ? 2 : (c->bar_n - topo);
  c->bar_nl = nl;

  const bool m1 = pintura_mudou_i(p, &c->barra_base, base);
  const bool m2 = pintura_mudou_i(p, &c->barra_modo, (int)pode_escrever);
  const bool m6 = pintura_mudou_i(p, &c->barra_cursor, eh_rascunho ? c->cursor : -1);
  const bool m7 = pintura_mudou_i(p, &c->barra_topo, topo);
  const bool m8 = pintura_mudou_i(p, &c->barra_env, eh_rascunho);
  if (m1 || m2 || m_txt || m_larg || m6 || m7 || m8 || completo) {
    // The send button is only redrawn when the bar moves or when the draft
    // goes from empty to filled (or the reverse) - not on every letter.
    const bool tudo = m1 || m2 || completo;
    const bool env  = tudo || m8;
    if (tudo) { tab5_tela_limpar(r.x, base, r.largura, ESCRITA_A, FUNDO); }
    else      { tab5_tela_limpar(r.x, base, r.largura - ENV_L, ESCRITA_A, FUNDO); }
    d.fillRoundRect(r.x + 12, base + 6, campo_l, ESCRITA_A - 12, TEMA_RAIO_M, pode_escrever ? TEMA_SUPERFICIE_ALTA : FUNDO);
    d.drawRoundRect(r.x + 12, base + 6, campo_l, ESCRITA_A - 12, TEMA_RAIO_M, TEMA_LINHA);
    tab5_texto(d, TAM_ESCRITA);
    d.setTextDatum(top_left);
    d.setTextColor(pode_escrever ? (uint16_t)TEMA_TEXTO : (uint16_t)TEMA_TEXTO_3, pode_escrever ? (uint16_t)TEMA_SUPERFICIE_ALTA : FUNDO);
    // Lines centred in the field by the font's REAL height (a single line sits
    // in the middle; two split the field) - the old fixed step left the second
    // touching the bottom edge.
    const int fa = d.fontHeight();
    const int passo = fa + 4;
    const int y0 = base + 6 + (ESCRITA_A - 12 - (nl * passo - 4)) / 2;
    c->bar_x0 = r.x + 24;
    c->bar_y0 = y0;
    c->bar_passo = passo;
    char lin[CONVERSA_LINHA_BYTES];
    for (int k = 0; k < nl; ++k) {
      const int ini = c->bar_ini[topo + k], fim = c->bar_ini[topo + k + 1];
      int tam = fim - ini;
      if (tam >= CONVERSA_LINHA_BYTES) { tam = CONVERSA_LINHA_BYTES - 1; }
      while (tam > 0 && fonte[ini + tam - 1] == '\n') { --tam; }   // the new line is not drawn
      memcpy(lin, fonte + ini, tam); lin[tam] = 0;
      d.drawString(lin, r.x + 24, y0 + k * passo);
    }
    if (eh_rascunho) {
      const int k = lc - topo;
      const int ini = c->bar_ini[lc];
      char pre[CONVERSA_LINHA_BYTES];
      int tam = c->cursor - ini;
      if (tam >= CONVERSA_LINHA_BYTES) { tam = CONVERSA_LINHA_BYTES - 1; }
      if (tam < 0) { tam = 0; }
      memcpy(pre, fonte + ini, tam); pre[tam] = 0;
      int cx = r.x + 24 + (int)d.textWidth(pre) + 2;
      if (cx > r.x + 12 + campo_l - 6) { cx = r.x + 12 + campo_l - 6; }
      d.fillRect(cx, y0 + k * passo, 3, fa, TEMA_DESTAQUE);
    }
    // No send button when sending is not possible: a button that does not work is worse than
    // not having one.
    if (pode_escrever && env) {
      botao_enviar(d, r.x + r.largura - ENV_L, base + 6, ENV_L - 14, ESCRITA_A - 12,
                   c->cor_enviar ? c->cor_enviar : (uint16_t)TEMA_DESTAQUE, eh_rascunho, FUNDO);
    }
  }

  // ---- virtual keyboard (the system's: it knows what to repaint) -----------
  tecvirt_posicionar(&c->kb, area_tec(c));
  tecvirt_pintar(&c->kb, d, completo);
}

// ---------------------------------------------------------------------------
// KEYS
// ---------------------------------------------------------------------------
// The keyboard looks at the text before the cursor to arm the sentence-start
// capital: called whenever the text or the cursor changes while it is open.
static void contexto(conversa_t* c) { tecvirt_contexto(&c->kb, c->rascunho, c->cursor); }

static void aplicar_tecla(conversa_t* c, int acao)
{
  if (acao > 0) {
    char u[4];   // codepoint with shift already applied (the keyboard applies it)
    inserir(c, u, (size_t)tecvirt_utf8(acao, u));
    contexto(c);
    sistema_pedir_redesenho();
    return;
  }
  if (acao == TECVIRT_APAGAR) { apagar_antes(c); contexto(c); sistema_pedir_redesenho(); return; }
  // The action key is NEW LINE, as in WhatsApp: the bar's button is what sends.
  if (acao == TECVIRT_LINHA)  { inserir(c, "\n", 1); contexto(c); sistema_pedir_redesenho(); return; }
  if (acao == TECVIRT_ENVIAR) { enviar_rascunho(c); }
  if (acao == TECVIRT_ENVIAR || acao == TECVIRT_FECHAR) { fechar_teclado(c); }
}

// The touch in SCREEN coordinates, and relative to the rectangle (what the keyboard uses).
static void para_tela(int x, int y, int* xs, int* ys)
{
  const area_t a = sistema_area();
  *xs = a.x + x;
  *ys = a.y + y;
}

// Where the compose bar starts (screen): it rises together with an open keyboard.
static int base_escrita(const conversa_t* c)
{
  return c->ret.y + c->ret.altura - ESCRITA_A - (tecvirt_aberto(&c->kb) ? tecvirt_altura(area_tec(c)) : 0);
}

// PASTE: the clipboard text goes in at the cursor. Line breaks stay (the
// conversation has lines); tabs and other control characters become spaces.
static void colar(conversa_t* c)
{
  char t[CONVERSA_TEXTO];
  const size_t n = transf_colar(t, sizeof(t));
  if (!n) { return; }
  for (size_t i = 0; i < n; ++i) { if ((unsigned char)t[i] < 0x20 && t[i] != '\n') { t[i] = ' '; } }
  inserir(c, t, n);
  contexto(c);
  sistema_recado(tr("Colado"));
  sistema_pedir_redesenho();
}

// HOLD: on a bubble copies the message text; on the compose bar, pastes.
static void segurar(conversa_t* c, int x, int y)
{
  int xs, ys;
  para_tela(x, y, &xs, &ys);
  if (tecvirt_contem(&c->kb, xs, ys)) { return; }
  sistema_consumir_toque();
  if (ys >= base_escrita(c)) {
    if (c->pode_escrever) { colar(c); }
    return;
  }
  static conversa_msg_t m;   // UI task only
  const int i = lista_item_em(&c->lista, x, y, nullptr);
  if (i >= 0 && c->ler(i, &m, c->ctx) && m.texto[0]) {
    transf_copiar(m.texto);
    sistema_recado(tr("Copiado"));
  }
}

// The keyboard (fingers, key feedback, release) belongs to the system: here we
// only apply the keys it confirmed in this sample.
void conversa_tocar_nivel(conversa_t* c, int x, int y, bool pressionado)
{
  int acoes[8];
  const int n = tecvirt_nivel(&c->kb, acoes, 8);
  for (int i = 0; i < n && tecvirt_aberto(&c->kb); ++i) { aplicar_tecla(c, acoes[i]); }
  if (toque_longo(&c->lp, x, y, pressionado)) { segurar(c, x, y); }

  // Scrolling the bubbles (the list window already ends above the bar and the keyboard).
  if (lista_arrastar(&c->lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

// TOUCH ON THE TEXT: moves the cursor to the character nearest the finger,
// without deleting anything. Uses the positions from the LAST paint of the bar
// (bar_x0/bar_y0/bar_passo and the visible lines) and measures with its font.
static void cursor_no_toque(conversa_t* c, int xs, int ys)
{
  if (!c->bar_rascunho || !c->disp || c->bar_nl <= 0 || c->bar_passo <= 0) { return; }
  int k = (ys - c->bar_y0) / c->bar_passo;
  if (ys < c->bar_y0) { k = 0; }
  if (k >= c->bar_nl) { k = c->bar_nl - 1; }
  const int lin = c->bar_topo + k;
  const int ini = c->bar_ini[lin];
  int fim = c->bar_ini[lin + 1];
  // On lines other than the last, the wrap space belongs to the line but a
  // cursor after it would be drawn on the line below: stop at the last
  // visible character.
  if (lin + 1 < c->bar_n) { while (fim > ini && (c->rascunho[fim - 1] == ' ' || c->rascunho[fim - 1] == '\n')) { --fim; } }

  lgfx::LovyanGFX& d = *c->disp;
  tab5_texto(d, TAM_ESCRITA);
  const int alvo = xs - c->bar_x0;
  char pre[CONVERSA_LINHA_BYTES];
  int melhor = ini, melhor_dist = 1 << 30;
  for (int i = ini;; i = utf8_seguinte(c->rascunho, i)) {
    int tam = i - ini;
    if (tam >= CONVERSA_LINHA_BYTES) { break; }
    memcpy(pre, c->rascunho + ini, tam); pre[tam] = 0;
    const int dist = abs((int)d.textWidth(pre) - alvo);
    if (dist < melhor_dist) { melhor_dist = dist; melhor = i; }
    if (i >= fim || !c->rascunho[i]) { break; }
  }
  c->cursor = melhor;
  contexto(c);
  sistema_pedir_redesenho();
}

void conversa_tocar(conversa_t* c, int x, int y)
{
  int xs, ys;
  para_tela(x, y, &xs, &ys);
  const area_t& r = c->ret;

  // Keyboard open: its strip belongs to it alone. The click here is just the
  // safety net for a touch too quick for the level to have seen (otherwise it would double a letter).
  if (tecvirt_contem(&c->kb, xs, ys)) {
    const int ac = tecvirt_clique(&c->kb, xs, ys);
    if (ac) { aplicar_tecla(c, ac); }
    return;
  }

  // The end of a scroll is not a click on anything.
  if (lista_item_tocado(&c->lista, x, y, nullptr) == LISTA_ARRASTE) { return; }

  // Compose bar: the arrow button sends; the field opens the virtual keyboard (only
  // without the physical one docked - with it, we do not steal half the screen).
  const int base = base_escrita(c);
  if (ys >= base && ys < base + ESCRITA_A && c->pode_escrever) {
    if (xs >= r.x + r.largura - ENV_L) {
      enviar_rascunho(c);
      fechar_teclado(c);
    } else if (tecvirt_aberto(&c->kb)) {
      cursor_no_toque(c, xs, ys);
    } else if (!tab5_teclado_presente()) {
      tecvirt_abrir(&c->kb, TECVIRT_PARAGRAFO);   // new line on the green key; send via the bar's button
      cursor_no_toque(c, xs, ys);       // the same touch that opens it already positions the cursor
      contexto(c);                      // empty draft: opens with the capital already armed
      pedir_completo(c);
    }
  }
}

// ---------------------------------------------------------------------------
// CURSOR FROM THE PHYSICAL KEYBOARD
// ---------------------------------------------------------------------------
// Del: deletes the character AFTER the cursor (Backspace deletes the one before).
static void apagar_depois(conversa_t* c)
{
  cursor_ajustar(c);
  if (!c->rascunho[c->cursor]) { return; }
  const int b = utf8_seguinte(c->rascunho, c->cursor);
  memmove(c->rascunho + c->cursor, c->rascunho + b, strlen(c->rascunho) - b + 1);
}

// Ctrl + arrow: jump a word (the spaces and then the word, as in any
// editor).
static void palavra(conversa_t* c, bool frente)
{
  const char* r = c->rascunho;
  int i = c->cursor;
  if (frente) {
    while (r[i] == ' ') { i = utf8_seguinte(r, i); }
    while (r[i] && r[i] != ' ') { i = utf8_seguinte(r, i); }
  } else {
    while (i > 0 && r[utf8_anterior(r, i)] == ' ') { i = utf8_anterior(r, i); }
    while (i > 0 && r[utf8_anterior(r, i)] != ' ') { i = utf8_anterior(r, i); }
  }
  c->cursor = i;
}

// How many CHARACTERS (not bytes) there are from 'ini' to 'fim'.
static int n_caracteres(const char* s, int ini, int fim)
{
  int n = 0;
  for (int i = ini; i < fim && s[i]; i = utf8_seguinte(s, i)) { ++n; }
  return n;
}

// Up/down arrow: the bar line above/below, in the same column (in
// characters). Uses the wrap from the LAST paint (bar_ini) - the bar is
// repainted on every key, so it matches the current text. Already on the
// first line, up goes to the start; on the last, down goes to the end.
static void linha_vizinha(conversa_t* c, bool baixo)
{
  cursor_ajustar(c);
  const int len = (int)strlen(c->rascunho);
  const int n = c->bar_n;
  if (!c->bar_rascunho || n <= 0 || c->bar_ini[n] != len) {   // stale or missing wrap
    c->cursor = baixo ? len : 0;
    return;
  }
  int k = 0;
  while (k + 1 < n && c->cursor >= c->bar_ini[k + 1]) { ++k; }
  const int alvo = baixo ? k + 1 : k - 1;
  if (alvo < 0)  { c->cursor = 0;   return; }
  if (alvo >= n) { c->cursor = len; return; }
  const int col = n_caracteres(c->rascunho, c->bar_ini[k], c->cursor);
  int i = c->bar_ini[alvo];
  int fim = c->bar_ini[alvo + 1];
  // The wrap space belongs to the line but a cursor after it would fall on
  // the line below: stop at the last visible character (as with touch).
  if (alvo + 1 < n) { while (fim > i && (c->rascunho[fim - 1] == ' ' || c->rascunho[fim - 1] == '\n')) { --fim; } }
  for (int k2 = 0; k2 < col && i < fim; ++k2) { i = utf8_seguinte(c->rascunho, i); }
  c->cursor = i;
}

void conversa_teclar(conversa_t* c, const char* texto, uint8_t codigo, uint8_t mod)
{
  if (!c->pode_escrever) { return; }
  const bool ctrl = (mod & TAB5_TECLA_CTRL) != 0;
  switch (codigo) {
    // Enter sends (as on a PC); Shift+Enter is a new line.
    case TAB5_TECLA_ENTER:
      if (mod & TAB5_TECLA_SHIFT) { inserir(c, "\n", 1); contexto(c); sistema_pedir_redesenho(); return; }
      enviar_rascunho(c); fechar_teclado(c); return;
    case TAB5_TECLA_APAGAR: apagar_antes(c);  sistema_pedir_redesenho(); return;
    case TAB5_TECLA_DEL:    apagar_depois(c); sistema_pedir_redesenho(); return;
    // Esc clears the whole draft (the cursor goes back to the start).
    case TAB5_TECLA_ESC:    c->rascunho[0] = 0; c->cursor = 0; sistema_pedir_redesenho(); return;
    case TAB5_TECLA_ESQUERDA:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, false); } else { c->cursor = utf8_anterior(c->rascunho, c->cursor); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_DIREITA:
      cursor_ajustar(c);
      if (ctrl) { palavra(c, true); } else { c->cursor = utf8_seguinte(c->rascunho, c->cursor); }
      sistema_pedir_redesenho();
      return;
    // Ctrl + up/down: start/end of the text in one go.
    case TAB5_TECLA_CIMA:
      if (ctrl) { c->cursor = 0; } else { linha_vizinha(c, false); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_BAIXO:
      if (ctrl) { c->cursor = (int)strlen(c->rascunho); } else { linha_vizinha(c, true); }
      sistema_pedir_redesenho();
      return;
    case TAB5_TECLA_TAB:    return;   // nothing to do in a message
    default: break;
  }
  // With Ctrl or Alt it is a shortcut, not text: a "Ctrl+c" must not become a "c".
  if (ctrl && texto && (texto[0] == 'v' || texto[0] == 'V')) { colar(c); return; }
  if (mod & (TAB5_TECLA_CTRL | TAB5_TECLA_ALT)) { return; }
  // Control keys do not become characters. An accented sequence arrives as more
  // than one byte and goes in whole (or not at all) - inserir() never cuts it in half.
  char limpo[16];
  size_t n = 0;
  for (const char* ch = texto; ch && *ch && n < sizeof(limpo) - 1; ++ch) {
    if ((unsigned char)*ch < 0x20) { continue; }
    limpo[n++] = *ch;
  }
  limpo[n] = 0;
  inserir(c, limpo, n);
  sistema_pedir_redesenho();
}
