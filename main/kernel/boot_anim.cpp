// SYSTEM INTRO - see the header (boot_anim.hpp) for what it is and when it
// appears. Only the drawing is here.
//
// HOW IT DRAWS: one whole frame per round, on the virtual screen (the system's
// drawing object), presented by the compositor - the same path as the home screen
// and apps. That way it never draws on the page the panel is scanning and
// the animation neither flickers nor tears.
//
// MEASUREMENTS: the mock-up was made on a 240 px-wide screen; everything here is
// written in its measurements and multiplied by K (720 / 240 = 3 on the Tab5 in
// portrait). Changing a number here means changing the same number in the mock-up.
//
// TIME: the timeline 't' goes from 0 to 1 over DURACAO_MS; each part of the
// animation starts and ends at a fixed fraction of it. The background points move
// by the real clock (px per second), not per frame - if the Tab5
// draws more slowly, the web moves the same, just with fewer frames.

#include "boot_anim.hpp"

#include <M5Unified.h>
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tab5_tema.hpp"
#include "tab5_ui.hpp"
#include "tela_virtual.hpp"
#include "compositor.hpp"
#include "tab5_quadro.hpp"
#include "idioma.h"

static const char* TAG = "boot_anim";

namespace {

// ---------------------------------------------------------------------------
// ANIMATION SETTINGS
// ---------------------------------------------------------------------------
constexpr int   DURACAO_MS = 1600;   // from empty screen to the whole "[ conectado ]"
constexpr int   ESPERA_MS  = 300;    // the finished name stays on screen for a moment
constexpr int   N_FUNDO    = 60;     // background points (the user's choice in the mock-up)
constexpr float ALCANCE    = 71.0f;  // maximum distance of a link (mock-up px)
constexpr float MAQ_LARG   = 240.0f; // mock-up screen width
constexpr float VEL_MAX    = 13.5f;  // maximum speed of a point (mock-up px per s)
constexpr int   ALT_FAIXA  = 32;     // screen lines per web band (see desenhar_teia)

// The seven main nodes: the mesh drawing, in mock-up px, from the
// group's centre. They stay FIXED; it is the background points that wander.
constexpr int   N_NOS = 7;
const float NOS_MAQ[N_NOS][2] = {
  {-55, -60}, {0, -85}, {55, -55}, {62, 10}, {20, 40}, {-35, 30}, {-65, -5},
};
constexpr float ESCALA_NOS = 1.45f;  // the node group is 45% larger than the base drawing

const char NOME[] = "M5tabOS";
constexpr int N_LETRAS = sizeof(NOME) - 1;
// The "scrambled" characters of the decryption and of the flying packets.
const char SIMBOLOS[] = "01#$%&*+<>/=?ABCDEFXZ";

struct ponto_t { float x, y, vx, vy; };
struct no_t    { float x, y; };

// ---------------------------------------------------------------------------
// SMALL SUMS
// ---------------------------------------------------------------------------
float limitar(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

// Starts fast and arrives slowly (cubic curve) - every movement in the animation.
float suave(float t)
{
  const float u = 1.0f - limitar(t);
  return 1.0f - u * u * u;
}

// Random number between 'lo' and 'hi'.
float sorteio(float lo, float hi)
{
  return lo + (hi - lo) * (float)(esp_random() % 10000) / 10000.0f;
}

char simbolo_sorteado(void)
{
  return SIMBOLOS[esp_random() % (sizeof(SIMBOLOS) - 1)];
}

// Colour 'cor' with opacity 'a' (0 = background only, 1 = colour only) over
// 'fundo'. M5GFX has no transparent line; as the animation always
// draws over the plain background, the blend is computed beforehand and the line comes out
// opaque - the same result on screen.
uint16_t misturar(uint16_t cor, uint16_t fundo, float a)
{
  a = limitar(a);
  const int r1 = (cor >> 11) & 31, g1 = (cor >> 5) & 63, b1 = cor & 31;
  const int r0 = (fundo >> 11) & 31, g0 = (fundo >> 5) & 63, b0 = fundo & 31;
  const int r = r0 + (int)lroundf((r1 - r0) * a);
  const int g = g0 + (int)lroundf((g1 - g0) * a);
  const int b = b0 + (int)lroundf((b1 - b0) * a);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

float distancia(float x0, float y0, float x1, float y1)
{
  return sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
}

// ---------------------------------------------------------------------------
// THE ANIMATION STATE (exists only while it runs)
// ---------------------------------------------------------------------------
// One web line, already with its final colour and the height band it occupies.
struct linha_t {
  float    x0, y0, x1, y1;
  int16_t  y_min, y_max;
  uint16_t cor;
};
// The maximum possible: every pair of background points, each node with each point
// (2 lines = 2 px thick) and every pair of nodes.
constexpr int MAX_LINHAS = N_FUNDO * (N_FUNDO - 1) / 2 + N_NOS * N_FUNDO * 2 + N_NOS * (N_NOS - 1) / 2;

struct anim_t {
  float    k;           // Tab5 px per mock-up px
  float    cx;          // horizontal centre
  float    y_nome;      // baseline of the name (as in the mock-up)
  int      larg, alt;
  no_t     nos[N_NOS];
  ponto_t  pontos[N_FUNDO];
  linha_t  linhas[MAX_LINHAS];   // the lines of the current frame (montar_teia)
  int      n_linhas;
  uint16_t cor_ponto;
};

void preparar(anim_t& a, int larg, int alt)
{
  a.larg = larg;
  a.alt = alt;
  a.k = larg / MAQ_LARG;
  a.cx = larg / 2.0f;
  a.y_nome = 220.0f * a.k;
  for (int i = 0; i < N_NOS; i++) {
    a.nos[i].x = a.cx + NOS_MAQ[i][0] * ESCALA_NOS * a.k;
    a.nos[i].y = a.y_nome - 26.0f * a.k + NOS_MAQ[i][1] * ESCALA_NOS * a.k;
  }
  // Every boot draws a different web.
  const float v = VEL_MAX * a.k;
  for (int i = 0; i < N_FUNDO; i++) {
    a.pontos[i] = { sorteio(0, (float)larg), sorteio(0, (float)alt), sorteio(-v, v), sorteio(-v, v) };
  }
}

// Moves the background points by 'dt' seconds, bouncing off the screen edges.
void mover_pontos(anim_t& a, float dt)
{
  for (int i = 0; i < N_FUNDO; i++) {
    ponto_t& p = a.pontos[i];
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    if (p.x < 0 || p.x > a.larg) { p.vx = -p.vx; p.x = p.x < 0 ? 0 : (float)a.larg; }
    if (p.y < 0 || p.y > a.alt)  { p.vy = -p.vy; p.y = p.y < 0 ? 0 : (float)a.alt; }
  }
}

// ---------------------------------------------------------------------------
// THE FRAME LAYERS, from back to front
// ---------------------------------------------------------------------------

// THE WEB - IN TWO STAGES, BECAUSE OF MEMORY.
//
// The screen lives in PSRAM, the same memory the panel reads non-stop (~118 MB/s).
// A slanted line written straight there touches one pixel here, another there: each
// pixel pulls in and writes back a 128 B block. With ~60 thousand pixels per frame, that
// became hundreds of MB/s of scattered traffic - on a cold boot, with the
// card mounting at the same time, the panel ran out of data for a frame (UNDERRUN and
// QUADRO ATRASADO in the log, with the core in linha_direta): the BLUE FLASH.
//
// Therefore: (1) montar_teia() only works out which lines exist and in what
// colour; (2) desenhar_teia() paints the screen in BANDS of a few lines in
// INTERNAL memory (fast and out of the panel's way) and pastes each finished band
// onto the back page in one go, in sequence - PSRAM only receives
// sequential writes, as in an ordinary copy.

// Builds the frame's line list. 'g' lights everything up together at the start (0..1);
// 's' is the seconds since the start (the blinking of the links between the nodes).
void montar_teia(anim_t& a, float g, float s)
{
  a.n_linhas = 0;
  auto juntar = [&a](float x0, float y0, float x1, float y1, uint16_t cor) {
    if (cor == TEMA_FUNDO || a.n_linhas >= MAX_LINHAS) { return; }   // too faint to show
    linha_t& l = a.linhas[a.n_linhas++];
    l = { x0, y0, x1, y1, (int16_t)floorf(fminf(y0, y1)), (int16_t)ceilf(fmaxf(y0, y1)), cor };
  };

  // Background points among themselves: thin and very faint lines, stronger the
  // closer they are.
  const float alc = ALCANCE * a.k;
  for (int i = 0; i < N_FUNDO; i++) {
    for (int j = i + 1; j < N_FUNDO; j++) {
      const ponto_t& p = a.pontos[i];
      const ponto_t& q = a.pontos[j];
      const float dx = p.x - q.x, dy = p.y - q.y;
      if (fabsf(dx) >= alc || fabsf(dy) >= alc) { continue; }   // far: don't even compute the root
      const float dist = sqrtf(dx * dx + dy * dy);
      if (dist >= alc) { continue; }
      juntar(p.x, p.y, q.x, q.y, misturar(TEMA_DESTAQUE, TEMA_FUNDO, 0.16f * (1.0f - dist / alc) * g));
    }
  }

  // Main nodes with the points passing close by: the link that "forms and
  // breaks" - cyan, 2 px (two lines side by side) and reaching further.
  const float alc_no = alc * 1.25f;
  for (int n = 0; n < N_NOS; n++) {
    for (int i = 0; i < N_FUNDO; i++) {
      const ponto_t& p = a.pontos[i];
      const float dist = distancia(a.nos[n].x, a.nos[n].y, p.x, p.y);
      if (dist >= alc_no) { continue; }
      const uint16_t cor = misturar(TEMA_APP_CIANO, TEMA_FUNDO, 0.38f * (1.0f - dist / alc_no) * g);
      juntar(a.nos[n].x, a.nos[n].y, p.x, p.y, cor);
      juntar(a.nos[n].x + 1, a.nos[n].y, p.x + 1, p.y, cor);
    }
  }

  // Main nodes among themselves: each nearby pair lights up and fades at its own
  // rhythm (the mesh "talking").
  for (int i = 0; i < N_NOS; i++) {
    for (int j = i + 1; j < N_NOS; j++) {
      if (distancia(a.nos[i].x, a.nos[i].y, a.nos[j].x, a.nos[j].y) >= 110.0f * a.k) { continue; }
      const float ritmo = 0.5f + 0.5f * sinf(s * 0.9f + i * 1.3f + j * 0.7f);
      if (ritmo <= 0.45f) { continue; }
      juntar(a.nos[i].x, a.nos[i].y, a.nos[j].x, a.nos[j].y,
             misturar(TEMA_TEXTO_2, TEMA_FUNDO, 0.22f * (ritmo - 0.45f) / 0.55f * g));
    }
  }

  a.cor_ponto = misturar(TEMA_TEXTO_2, TEMA_FUNDO, 0.35f * g);
}

// The piece of line 'l' that falls in the band (screen lines y0..y0+alt) of
// 'buf' (larg x alt). Step by step along the longer axis, always with the SAME
// arithmetic for the whole line - neighbouring bands join without a step.
void linha_na_faixa(uint16_t* buf, int larg, int y0, int alt, const linha_t& l)
{
  if (l.y_max < y0 || l.y_min >= y0 + alt) { return; }   // doesn't pass through this band
  const float dx = l.x1 - l.x0, dy = l.y1 - l.y0;
  const int n = (int)fmaxf(fabsf(dx), fabsf(dy));
  if (n == 0) { return; }
  const float px = dx / n, py = dy / n;
  // Only the steps whose height falls in the band (with half a pixel of slack).
  int k0 = 0, k1 = n;
  if (py != 0.0f) {
    float ka = (y0 - 0.5f - l.y0) / py, kb = (y0 + alt - 0.5f - l.y0) / py;
    if (ka > kb) { const float t = ka; ka = kb; kb = t; }
    k0 = ka > 0.0f ? (int)floorf(ka) : 0;
    k1 = kb < n ? (int)ceilf(kb) : n;
  }
  for (int k = k0; k <= k1; k++) {
    const int x = (int)(l.x0 + px * k + 0.5f);
    const int y = (int)(l.y0 + py * k + 0.5f) - y0;
    if ((unsigned)x < (unsigned)larg && (unsigned)y < (unsigned)alt) { buf[y * larg + x] = l.cor; }
  }
}

// Paints the web across the whole screen, band by band: clears the band to the background,
// draws the lines and points that fall in it and pastes it onto the back page
// (tela_virtual_colar - already marks the band as covered and drawn).
void desenhar_teia(lgfx::LovyanGFX& d, const anim_t& a, uint16_t* faixa, int alt_faixa)
{
  for (int y0 = 0; y0 < a.alt; y0 += alt_faixa) {
    const int alt = (y0 + alt_faixa <= a.alt) ? alt_faixa : a.alt - y0;
    // Clears the band two pixels at a time (a 32-bit word = two
    // 16-bit pixels): half the writes. The width (720) is always even.
    uint32_t* f32 = (uint32_t*)faixa;
    const uint32_t dois = ((uint32_t)TEMA_FUNDO << 16) | TEMA_FUNDO;
    const int n_pares = a.larg * alt / 2;
    for (int i = 0; i < n_pares; i++) { f32[i] = dois; }
    for (int i = 0; i < a.n_linhas; i++) { linha_na_faixa(faixa, a.larg, y0, alt, a.linhas[i]); }
    // The background points (4x4) on top of the lines.
    for (int i = 0; i < N_FUNDO; i++) {
      const int px = (int)a.pontos[i].x - 2, py = (int)a.pontos[i].y - 2 - y0;
      for (int yy = py; yy < py + 4; yy++) {
        if ((unsigned)yy >= (unsigned)alt) { continue; }
        for (int xx = px; xx < px + 4; xx++) {
          if ((unsigned)xx < (unsigned)a.larg) { faixa[yy * a.larg + xx] = a.cor_ponto; }
        }
      }
    }
    // Via DMA2D (the CPU took ~30 ms copying the whole screen in PSRAM);
    // if DMA refuses, via the CPU.
    if (!tela_virtual_colar_faixa(faixa, y0, alt)) { tela_virtual_colar(&d, 0, y0, a.larg, alt, faixa, a.larg); }
  }
}

// THE MAIN NODES: a bright point with a breathing cyan halo. They appear one
// after another at the start.
void desenhar_nos(lgfx::LovyanGFX& d, const anim_t& a, float t, float s)
{
  for (int i = 0; i < N_NOS; i++) {
    const float e = suave((t - i * 0.03f) / 0.12f);
    if (e <= 0.0f) { continue; }
    const float respira = 1.0f + 0.12f * sinf(s * 2.0f + i);
    d.fillCircle((int)a.nos[i].x, (int)a.nos[i].y, (int)(7.0f * a.k * e * respira),
                 misturar(TEMA_APP_CIANO, TEMA_FUNDO, 0.18f * e));
    d.fillCircle((int)a.nos[i].x, (int)a.nos[i].y, (int)(3.2f * a.k * e),
                 misturar(TEMA_TEXTO_2, TEMA_FUNDO, e));
  }
}

// THE PACKETS: characters that leave the nodes and fly to where the name goes.
void desenhar_pacotes(lgfx::LovyanGFX& d, const anim_t& a, float t)
{
  d.setFont(tab5_fonte_suave(false, TEMA_LETRA_CORPO));
  d.setTextDatum(middle_center);
  char c[2] = { 0, 0 };
  for (int k = 0; k < 32; k++) {
    const float p = suave((t - 0.2f - k * 0.011f) / 0.35f);
    if (p <= 0.0f || p >= 1.0f) { continue; }   // hasn't left yet, or has already arrived
    const no_t& n = a.nos[k % N_NOS];
    const float alvo_x = a.cx + (-48.0f + (k % N_NOS) * 16.0f) * a.k;
    const float alvo_y = a.y_nome - 10.0f * a.k;
    c[0] = simbolo_sorteado();
    d.setTextColor((k % 2) ? TEMA_SUCESSO : TEMA_APP_CIANO);
    d.drawString(c, (int)(n.x + (alvo_x - n.x) * p), (int)(n.y + (alvo_y - n.y) * p));
  }
}

// THE DECRYPTED NAME: each letter scrambles in green until its turn and then
// turns white. Letters in equal-width cells (as in a terminal) and a
// green cursor blinking at the end.
void desenhar_nome(lgfx::LovyanGFX& d, const anim_t& a, float t, int ms)
{
  constexpr float INICIO = 0.5f;
  if (t < INICIO) { return; }

  // NO box behind the name, on purpose: the name stays TOGETHER with the web,
  // the lines passing behind the letters (the user's request - the dark
  // box showed up as a square blotting out the web). The lines are very
  // faint, and the name in bold white reads over them.
  d.setFont(tab5_fonte_suave(true, TEMA_LETRA_GRANDE));
  d.setTextDatum(middle_center);
  const int casa = d.textWidth("M") + 6;               // the widest letter in the name
  const int x0 = (int)a.cx - casa * N_LETRAS / 2;
  const int y_meio = (int)(a.y_nome - 10.0f * a.k);
  char c[2] = { 0, 0 };
  for (int i = 0; i < N_LETRAS; i++) {
    const bool pronta = t >= INICIO + 0.05f + i * 0.04f;
    c[0] = pronta ? NOME[i] : simbolo_sorteado();
    d.setTextColor(pronta ? TEMA_TEXTO : TEMA_SUCESSO);
    d.drawString(c, x0 + casa * i + casa / 2, y_meio);
  }

  // Terminal cursor: 350 ms on, 350 off.
  if (t > INICIO + 0.4f && (ms / 350) % 2 == 0) {
    const int alt = TEMA_LETRA_GRANDE * 3 / 4;
    d.fillRect(x0 + casa * N_LETRAS + 4, y_meio - alt / 2, alt / 2, alt, TEMA_SUCESSO);
  }

  // "[ conectado ]" slowly lighting up underneath.
  const float e = suave((t - 0.82f) / 0.15f);
  if (e > 0.0f) {
    d.setFont(tab5_fonte_suave(false, TEMA_LETRA_CORPO));
    d.setTextColor(misturar(TEMA_TEXTO_3, TEMA_FUNDO, e));
    d.drawString(tr("[ conectado ]"), (int)a.cx, (int)(a.y_nome + 28.0f * a.k));
  }
}

}  // namespace

void boot_anim_mostrar(void)
{
  auto& d = tela_virtual();
  // The band is pasted line by line onto the page: only with the screen in portrait (rotation 0,
  // the boot one). Rotated, the arithmetic would be different - no animation, and boot carries on.
  if (d.getRotation() != 0) { ESP_LOGW(TAG, "screen rotated - no animation"); return; }

  // The state (~60 KB, the line list) in PSRAM, read sequentially; the
  // band (720 x 32 px = 45 KB) in INTERNAL memory - see desenhar_teia. Both
  // only during the animation.
  anim_t* a = (anim_t*)heap_caps_malloc(sizeof(anim_t), MALLOC_CAP_SPIRAM);
  // Aligned and in DMA memory: DMA2D reads the band straight from here.
  uint16_t* faixa = (uint16_t*)heap_caps_aligned_alloc(64, (size_t)d.width() * ALT_FAIXA * 2,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!a || !faixa) {
    ESP_LOGW(TAG, "out of memory - no animation");
    heap_caps_free(a);
    heap_caps_free(faixa);
    return;
  }
  preparar(*a, d.width(), d.height());

  // For the closing log: did the animation make the screen drop any frame?
  uint32_t pior_us = 0, periodo_us = 0;
  const uint32_t atrasos0 = tab5_quadro_atrasos(&pior_us, &periodo_us);
  const uint32_t underruns0 = tab5_quadro_underruns();

  const int64_t t0 = esp_timer_get_time();
  int64_t antes = t0;
  int quadros = 0;
  bool pulou = false;
  for (;;) {
    const int64_t agora = esp_timer_get_time();
    const int ms = (int)((agora - t0) / 1000);
    if (ms > DURACAO_MS + ESPERA_MS) { break; }

    // A touch skips the rest of the animation.
    M5.update();
    if (M5.Touch.getCount() > 0) { pulou = true; break; }

    const float t = limitar((float)ms / DURACAO_MS);
    const float s = ms / 1000.0f;
    mover_pontos(*a, (agora - antes) / 1e6f);
    antes = agora;

    montar_teia(*a, suave(t / 0.25f), s);
    desenhar_teia(d, *a, faixa, ALT_FAIXA);   // covers the whole screen: no need to clear first   // covers the whole screen: no need to clear first
    desenhar_nos(d, *a, t, s);
    desenhar_pacotes(d, *a, t);
    desenhar_nome(d, *a, t, ms);
    comp_apresentar();
    comp_esperar_troca();   // only draws the next one when this one is already on screen
    quadros++;
  }

  // Whoever skipped with a finger: wait for them to lift it (up to 1 s), so the touch doesn't become
  // a touch on the home screen that comes next.
  if (pulou) {
    for (int i = 0; i < 100 && M5.Touch.getCount() > 0; i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
      M5.update();
    }
  }

  const int total_ms = (int)((esp_timer_get_time() - t0) / 1000);
  const uint32_t atrasos = tab5_quadro_atrasos(&pior_us, &periodo_us) - atrasos0;
  const uint32_t underruns = tab5_quadro_underruns() - underruns0;
  // With a failure it becomes a WARNING (goes to the card log): it is the blue flash.
  if (atrasos || underruns) {
    ESP_LOGW(TAG, "%d frames in %d ms - the screen FAILED: %lu late, %lu underrun(s)",
             quadros, total_ms, (unsigned long)atrasos, (unsigned long)underruns);
  } else {
    ESP_LOGI(TAG, "%d frames in %d ms (%d f/s), screen without failures%s", quadros, total_ms,
             total_ms > 0 ? quadros * 1000 / total_ms : 0, pulou ? ", skipped by touch" : "");
  }

  heap_caps_free(a);
  heap_caps_free(faixa);
  // Restores the global text state as the system expects (the main loop
  // relies on top_left and the body font).
  d.setTextDatum(top_left);
  tab5_fonte_corpo(d);
}
