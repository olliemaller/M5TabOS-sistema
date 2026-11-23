// SHELL - the lock screen: locking and unlocking, the background animation, the
// clock, the date and the notices in front of it. Split out of shell.cpp; what
// the parts share lives in shell_interno.hpp.

#include "esp_attr.h"
#include "bloqueio_anim.hpp"
#include "tela_virtual.hpp"
#include "ajustes.h"
#include "idioma.h"
#include "tab5_tela.hpp"
#include "notificacao.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_hora.hpp"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "shell_interno.hpp"

// ---------------------------------------------------------------------------
// LOCK SCREEN
// ---------------------------------------------------------------------------
// v1, deliberately simple: big clock, date, the highlighted notice (the same
// source the status bar uses) and the swipe hint. No password. Everything
// that changes over time compares with the cache before painting, like the rest
// of the system - the clock is checked every second and only repaints when the
// minute ticks over.
static char     s_bq_hora[8]   = "";
EXT_RAM_BSS_ATTR static char s_bq_aviso[4 * (NOTIF_RESUMO + 24)];   // zeroed = ""
static uint32_t s_bq_dia       = 0;

static void data_por_extenso(char* out, size_t n)
{
  const uint32_t t = tab5_hora_epoch();
  if (!t) { snprintf(out, n, " "); return; }
  struct tm lt;
  tab5_hora_local(t, &lt);
  idioma_data_por_extenso(&lt, out, n);
}

// The lock screen's texts sit IN FRONT of the animation.
//
// Each text is painted ONCE (when it changes) into a small sprite, with a black
// background and the usual smooth font - the same drawing as before the
// animation. On each frame, only where the animation has passed over it, the
// sprite is copied to the screen with BLACK AS TRANSPARENT. That way the text
// never depends on what is on the back page: painting smooth text with no
// background makes M5GFX READ the screen pixels and blend the letter edges
// with them, which thickens the letters on every repaint and makes them ugly.
//
// Fixed boxes, non-overlapping, with slack around the text.
enum { BQ_HORA, BQ_DATA, BQ_AVISO, BQ_DICA, BQ_N };

static lgfx::LGFX_Sprite* s_bq_spr[BQ_N] = {};

// NOTICES ON THE LOCK SCREEN: one row per app, as many as fit between the date
// and the hint (up to 4). In landscape there is room for only one - the most severe.
static const int BQ_AVISO_LINHA = 52;
static int bq_linhas_aviso(const area_t& a)
{
  const int y_hora = a.y + a.altura / 4;
  const int y_dica = a.y + a.altura - 110;
  const int n = ((y_dica - 48) - (y_hora + 190)) / BQ_AVISO_LINHA;
  return n < 1 ? 1 : (n > 4 ? 4 : n);
}

static void bq_caixas(const area_t& a, bq_ret_t r[BQ_N])
{
  const int cx = a.x + a.largura / 2;
  const int y_hora = a.y + a.altura / 4;
  const int y_dica = a.y + a.altura - 110;
  r[BQ_HORA]  = { (int16_t)(cx - 240), (int16_t)(y_hora - 92),  480, 170 };
  r[BQ_DATA]  = { (int16_t)(cx - 350), (int16_t)(y_hora + 78),  700, 64 };
  r[BQ_AVISO] = { (int16_t)(a.x + 10), (int16_t)(y_hora + 190), (int16_t)(a.largura - 20),
                  (int16_t)(bq_linhas_aviso(a) * BQ_AVISO_LINHA + 18) };
  r[BQ_DICA]  = { (int16_t)(cx - 200), (int16_t)(y_dica - 48),  400, 80 };
}

// Draws text 'i' into 't' (a sprite or the screen), with (ox, oy) = the box's
// corner in screen coordinates. Always a black background: black (TFT_BLACK) is
// not a theme colour, and it is the TRANSPARENT key for the copy to the screen -
// it is what lets the animation show behind the text.
static void bq_texto(lgfx::LovyanGFX& t, const area_t& a, int i, const char* hora,
                     const char* data, const char* aviso, int ox, int oy)
{
  const int cx = a.x + a.largura / 2 - ox;
  const int y_hora = a.y + a.altura / 4 - oy;
  t.setTextDatum(middle_center);
  switch (i) {
    case BQ_HORA:
      tab5_fonte_corpo(t);
      tab5_texto(t, 7.0f);    // 56 px, the body font's largest native height; doubled below
      t.setTextSize(2);       // integer (not fractional): 1 font pixel = 2x2 on screen
      t.setTextColor(TEMA_TEXTO, TFT_BLACK);
      t.drawString(hora, cx, y_hora);
      t.setTextSize(1);
      break;
    case BQ_DATA:
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.5f);
      t.setTextColor(TEMA_TEXTO_2, TFT_BLACK);
      t.drawString(data, cx, y_hora + 110);
      break;
    case BQ_AVISO: {
      // 'aviso' is one line per app: the first character is the level ('0'..'3'),
      // the rest is the text, and '\n' separates them (built in desenhar_bloqueio).
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.0f);
      int y = y_hora + 225;
      for (const char* p = aviso; *p; ) {
        const char* fim = strchr(p, '\n');
        const size_t n = fim ? (size_t)(fim - p) : strlen(p);
        if (n > 1) {
          char linha[NOTIF_RESUMO + 24];
          snprintf(linha, sizeof(linha), "%.*s", (int)(n - 1), p + 1);
          t.setTextColor(cor_do_nivel((notif_nivel_t)(p[0] - '0')), TFT_BLACK);
          t.drawString(linha, cx, y);
          y += BQ_AVISO_LINHA;
        }
        if (!fim) { break; }
        p = fim + 1;
      }
    } break;
    default: {
      const int y_dica = a.y + a.altura - 110 - oy;
      t.fillRoundRect(cx - 60, y_dica - 40, 120, 8, 4, TEMA_DESTAQUE);   // swipe bar
      tab5_fonte_corpo(t);
      tab5_texto(t, 3.0f);
      t.setTextColor(TEMA_TEXTO_3, TFT_BLACK);
      t.drawString(tr("Deslize para cima"), cx, y_dica);
    } break;
  }
}

static void bq_sprites_soltar(void)
{
  for (int i = 0; i < BQ_N; ++i) {
    delete s_bq_spr[i];
    s_bq_spr[i] = nullptr;
  }
}

// Creates the sprites (PSRAM, only while locked). false = out of memory: the
// text falls back to direct drawing, with a black background.
static bool bq_sprites_criar(const bq_ret_t cx[BQ_N])
{
  for (int i = 0; i < BQ_N; ++i) {
    auto* sp = new lgfx::LGFX_Sprite();
    if (sp) {
      sp->setColorDepth(16);
      sp->setPsram(true);
    }
    if (!sp || !sp->createSprite(cx[i].w, cx[i].h)) {
      delete sp;
      bq_sprites_soltar();
      return false;
    }
    s_bq_spr[i] = sp;
  }
  return true;
}

void bloquear_tela(void)
{
  s_bloqueado   = true;
  s_painel_avisos = false;   // the notification centre does not survive locking
  s_bq_hora[0]  = 0;   // forces the first full draw
  s_sujo_layout = true;
  bq_anim_sortear();
}

void bloqueio_ao_ligar_tela(void)
{
  if (s_bloqueado)            { bq_anim_sortear(); }   // woke up still locked: another animation
  else if (ajustes_bloqueio()) { bloquear_tela(); }
}

void desbloquear_tela(void)
{
  s_bloqueado   = false;
  s_sujo_layout = true;   // back to home/app: full repaint, nav included
  bq_sprites_soltar();
  bq_anim_soltar();
}

// One lock-screen frame (~25 Hz): animation in the background, text in front.
void desenhar_bloqueio(lgfx::LovyanGFX& d, const area_t& a, bool completo)
{
  char hora[8], data[48], aviso[sizeof(s_bq_aviso)] = "";
  hora_agora(hora, sizeof(hora));
  data_por_extenso(data, sizeof(data));

  // ONE APP PER ROW, not just the highlighted one: with Mesh and ESP-NOW notifying
  // at the same time, the lock screen showed one and hid the other. The last row
  // that fits becomes "+N" when there are apps left over.
  notif_item_t lista[NOTIF_MAX_FONTES];
  const int n_av = notif_listar(lista, NOTIF_MAX_FONTES);
  const int cabem = bq_linhas_aviso(a);
  size_t usado = 0;
  for (int i = 0; i < n_av && i < cabem; ++i) {
    char txt[NOTIF_RESUMO + 24];
    if (i == cabem - 1 && n_av > cabem) {
      snprintf(txt, sizeof(txt), tr("+%d outros avisos"), n_av - i);
    } else if (lista[i].resumo[0]) {
      snprintf(txt, sizeof(txt), "%s: %s", tr(lista[i].nome), lista[i].resumo);
    } else {
      snprintf(txt, sizeof(txt), "%s: %d", tr(lista[i].nome), lista[i].quantidade);
    }
    const int k = snprintf(aviso + usado, sizeof(aviso) - usado, "%c%s\n",
                           (char)('0' + (int)lista[i].nivel), txt);
    if (k < 0 || (size_t)k >= sizeof(aviso) - usado) { break; }
    usado += (size_t)k;
  }

  const bool hora_mudou  = strcmp(hora, s_bq_hora) != 0;
  const bool aviso_mudou = strcmp(aviso, s_bq_aviso) != 0;
  snprintf(s_bq_hora, sizeof(s_bq_hora), "%s", hora);
  snprintf(s_bq_aviso, sizeof(s_bq_aviso), "%s", aviso);

  bq_ret_t caixa[BQ_N];
  bq_caixas(a, caixa);
  const bool mudou[BQ_N] = { completo || hora_mudou, completo || hora_mudou, completo || aviso_mudou, completo };

  // The sprites are born (or reborn) with the full draw and are only painted
  // again when the text changes.
  if (completo && !s_bq_spr[0]) { bq_sprites_criar(caixa); }
  const lgfx::TextStyle estilo = d.getTextStyle();
  for (int i = 0; i < BQ_N; ++i) {
    if (!mudou[i] || !s_bq_spr[i]) { continue; }
    s_bq_spr[i]->fillScreen(TFT_BLACK);
    bq_texto(*s_bq_spr[i], a, i, hora, data, aviso, caixa[i].x, caixa[i].y);
  }

  // Text that changed: the box is cleared (the animation redraws itself there) and repainted.
  bq_ret_t extras[BQ_N];
  int n_extras = 0;
  if (completo) {
    bq_anim_iniciar(a);
  } else {
    for (int i = 0; i < BQ_N; ++i) { if (mudou[i]) { extras[n_extras++] = caixa[i]; } }
  }
  bq_ret_t clips[BQ_N];
  bq_anim_quadro(d, extras, n_extras, caixa, BQ_N, clips);

  const uint16_t preto = 0;   // the sprite's transparent colour
  for (int i = 0; i < BQ_N; ++i) {
    bq_ret_t clip = clips[i];
    if (mudou[i]) { clip = caixa[i]; }
    if (clip.w <= 0) { continue; }
    d.setClipRect(clip.x, clip.y, clip.w, clip.h);
    if (s_bq_spr[i]) {
      s_bq_spr[i]->pushSprite(&d, caixa[i].x, caixa[i].y, preto);
    } else {
      bq_texto(d, a, i, hora, data, aviso, 0, 0);
    }
    d.clearClipRect();
  }
  d.setTextStyle(estilo);
  tab5_fonte_corpo(d);
}
