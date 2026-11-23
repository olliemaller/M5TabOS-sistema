// VIDEO APP - plays the videos on the SD card: .mp4 (H.264, the phone one, with its own
// sound) and .avi (MJPEG, with the sound from the .mp3 of the same name alongside).
//
// This app is ONLY THE INTERFACE. What reads the file, decodes (H.264 on the CPU,
// JPEG in hardware), scales the frame (PPA) and plays the sound is the system service
// (services/tab5_video): the app lists, asks to open, shows the frame the
// service delivers and handles the buttons. To create the files on the Mac:
// ferramentas/video_para_tab5.sh.
//
// TWO SCREENS (the one that applies is the service's: a video is open or not):
//   LIST    - the videos in the SD card's Videos folder (listed by the worker).
//   PLAYING - the frame at the top, below it the progress bar (tap = seek
//             to), the time, Pausar/Tocar (pause/play) and Lista (list).
//
// THE PACE IS THE VIDEO'S. Each new frame, pause, end or error arrives as
// AVISO_VIDEO and redraws the app at once: no clock on top (as in the
// Camera). The frame goes from the PPA straight to the app's DMA surface
// (sistema_superficie); here we only notify the compositor.
//
// The app is ORI_FIXO_EM_PE: the DMA surface only exists with the screen in portrait.

#include "app.h"
#include "idioma.h"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "lista.hpp"
#include "pintura.hpp"
#include "trabalho.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_video.hpp"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <atomic>
#include <cstdio>
#include <cstring>

static const char* TAG = "app.video";

static const uint16_t ROXO  = 0xFB5F;   // rgb(255,106,255) - only the app ICON (stays as is; the screen uses the theme)

#define PASTA_VIDEOS "/sdcard/Videos"   // where the app looks (created if it does not exist)
#define LIN_MAX   200                   // videos in the list
#define LIN_A     112                   // height of a list row
#define TOPO_A    104                   // the list title
#define MARGEM    24

// ---------------------------------------------------------------------------
// THE LIST - built by the worker, in an array the UI is NOT reading; only
// in ao_aviso does the UI switch to it (kernel/trabalho.hpp).
// ---------------------------------------------------------------------------
typedef struct {
  char     nome[TAB5_ARQ_NOME_MAX];
  uint64_t bytes;
} linha_t;
EXT_RAM_BSS_ATTR static linha_t s_a[LIN_MAX], s_b[LIN_MAX];
static linha_t* s_lin = s_a;            // the one the UI draws
static linha_t* s_tras = s_b;           // the one the worker builds
static int  s_n = 0;
static bool s_sem_cartao = false;
static bool s_carregando = false;
static std::atomic<int>  s_n_tras{0};
static std::atomic<bool> s_sem_cartao_tras{false};
static std::atomic<bool> s_pronto{false};
static lista_t s_lista;

// WHAT THE APP KNOWS ABOUT THE SERVICE (to notice screen changes).
static bool s_player = false;           // showing the player (a video is open)
static int  s_ql = 0, s_qa = 0;         // the frame size the screen was built with
static bool s_acordado = false;         // we asked for the screen to stay on
static pintura_t s_p;
static int  s_sig = -1;                 // what the controls were showing

// ---------------------------------------------------------------------------
// LISTING (on the worker)
// ---------------------------------------------------------------------------
static void trab_listar(void*)
{
  tab5_arq_item_t* it = (tab5_arq_item_t*)heap_caps_malloc(sizeof(tab5_arq_item_t) * LIN_MAX, MALLOC_CAP_SPIRAM);
  int n = 0;
  bool sem = true;
  if (it) {
    int total = 0;
    int k = tab5_arq_listar(PASTA_VIDEOS, it, LIN_MAX, &total, TAB5_ARQ_ORDEM_NOME, TAB5_VIDEO_EXTENSOES);
    if (k < 0) {   // the folder does not exist yet: create it (the SD card may be there)
      tab5_arq_criar_pasta("/sdcard", "Videos");
      k = tab5_arq_listar(PASTA_VIDEOS, it, LIN_MAX, &total, TAB5_ARQ_ORDEM_NOME, TAB5_VIDEO_EXTENSOES);
    }
    sem = k < 0;
    for (int i = 0; i < k && n < LIN_MAX; ++i) {
      if (it[i].tipo == TAB5_ARQ_PASTA) { continue; }
      snprintf(s_tras[n].nome, sizeof(s_tras[n].nome), "%s", it[i].nome);
      s_tras[n].bytes = it[i].bytes;
      ++n;
    }
    heap_caps_free(it);
  }
  s_n_tras = n;
  s_sem_cartao_tras = sem;
  s_pronto = true;
}

static void carregar(void)
{
  s_carregando = true;
  s_pronto = false;
  if (!sistema_trabalho(trab_listar, nullptr)) { s_carregando = false; }
}

// ---------------------------------------------------------------------------
// THE TIME
// ---------------------------------------------------------------------------
static void tempo_txt(uint32_t ms, char* out, size_t n)
{
  const uint32_t s = ms / 1000;
  if (s >= 3600) { snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60)); }
  else           { snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }
}

// ---------------------------------------------------------------------------
// THE PLAYER GEOMETRY - in coordinates RELATIVE to the area (those of ao_tocar);
// the drawing adds a.x and a.y (screen coordinates).
// ---------------------------------------------------------------------------
#define BARRA_X  MARGEM
#define BARRA_A  16
#define BOTAO_A  96
#define CONTROLES_A 330          // height reserved for the controls

typedef struct {
  int vy, vh;                    // the video rectangle (y a multiple of 8 ON SCREEN, height a multiple of 8)
  int by;                        // y of the progress bar
  int bl;                        // width of the bar
  int bty;                       // y of the buttons
  int bl1;                       // width of each button
} geo_t;

static geo_t geometria(const area_t& a, int ql, int qa)
{
  geo_t g;
  g.vy = ((a.y + 7) & ~7) - a.y;   // the surface requires y to be a multiple of 8
  int vh = 0;
  if (ql > 0 && qa > 0) {
    const int disp = (a.altura - g.vy - CONTROLES_A) & ~7;
    int s16 = (a.largura * 16) / ql;
    const int s16a = (disp * 16) / qa;
    if (s16a < s16) { s16 = s16a; }
    if (s16 < 1) { s16 = 1; }
    vh = ((qa * s16 / 16) + 7) & ~7;
    if (vh > disp) { vh = disp; }
  }
  g.vh  = vh;
  g.by  = g.vy + vh + 56;
  g.bl  = a.largura - 2 * BARRA_X;
  g.bty = g.by + 112;
  g.bl1 = (a.largura - 3 * MARGEM) / 2;
  return g;
}

// ---------------------------------------------------------------------------
// DRAWING
// ---------------------------------------------------------------------------
static int linha_altura(int, void*) { return LIN_A; }

static void linha_desenhar(lgfx::LovyanGFX& d, int i, int x0, int y, int largura, void*)
{
  if (i < 0 || i >= s_n) { return; }
  const linha_t& l = s_lin[i];
  const int cy = y + LIN_A / 2, q = 64, qx = x0 + MARGEM;
  d.fillRect(x0, y, largura, LIN_A, TEMA_FUNDO);
  d.fillRoundRect(qx, cy - q / 2, q, q, TEMA_RAIO_M, TEMA_SUPERFICIE_ALTA);
  d.fillTriangle(qx + q * 3 / 10, cy - q / 4, qx + q * 3 / 10, cy + q / 4, qx + q * 3 / 4, cy, TEMA_DESTAQUE);
  char nome[TAB5_ARQ_NOME_MAX], tam[24];
  snprintf(nome, sizeof(nome), "%s", l.nome);
  char* ponto = strrchr(nome, '.');
  if (ponto) { *ponto = '\0'; }
  tab5_bytes_txt(tam, sizeof(tam), l.bytes);
  const int tx = qx + q + 20;
  tab5_escrever(d, nome, tx, cy - 6, 30, true, TEMA_TEXTO, TEMA_FUNDO, bottom_left, largura - tx - MARGEM - 8);
  tab5_escrever(d, tam, tx, cy + 4, 22, false, TEMA_TEXTO_2, TEMA_FUNDO, top_left);
}

static void desenhar_lista(lgfx::LovyanGFX& d, const area_t& a, bool completa)
{
  if (completa) {
    d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
    tab5_escrever(d, tr("Vídeos"), a.x + MARGEM, a.y + 28, TEMA_LETRA_TITULO, true, TEMA_TEXTO, TEMA_FUNDO, top_left);
  }
  const area_t janela = { a.x, a.y + TOPO_A, a.largura, a.altura - TOPO_A };
  if (s_n == 0) {
    if (completa) {
      d.fillRect(janela.x, janela.y, janela.largura, janela.altura, TEMA_FUNDO);
      const int cx = a.x + a.largura / 2, cy = janela.y + janela.altura / 3;
      if (s_carregando) {
        tab5_escrever(d, tr("Carregando..."), cx, cy, 28, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else if (s_sem_cartao) {
        tab5_escrever(d, tr("Sem cartão SD"), cx, cy, 30, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      } else {
        tab5_escrever(d, tr("Nenhum vídeo ainda"), cx, cy, 30, true, TEMA_TEXTO, TEMA_FUNDO, middle_center);
        tab5_escrever(d, tr("Copie vídeos .mp4 (do celular) ou"), cx, cy + 50, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
        tab5_escrever(d, tr(".avi (MJPEG) para a pasta Videos"), cx, cy + 84, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
        tab5_escrever(d, tr("do cartão. Leve: 360p ou 480p."), cx, cy + 118, 24, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
      }
    }
    lista_esconder(&s_lista);
  } else {
    lista_desenhar(&s_lista, d, janela, TEMA_FUNDO, completa);
  }
}

// Screen for one still opening or that errored: just the text and the Lista button.
static void desenhar_aviso(lgfx::LovyanGFX& d, const area_t& a, const tab5_video_estado_t& e, bool completa)
{
  if (!completa) { return; }
  d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
  const int cx = a.x + a.largura / 2;
  if (e.erro) {
    tab5_escrever(d, tr("Não deu para tocar"), cx, a.y + 200, 36, true, TEMA_PERIGO, TEMA_FUNDO, middle_center);
    tab5_escrever(d, tr(e.erro_txt), cx, a.y + 260, 26, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center, a.largura - 2 * MARGEM);
  } else {
    tab5_escrever(d, tr("Abrindo o vídeo..."), cx, a.y + 200, 32, false, TEMA_TEXTO_2, TEMA_FUNDO, middle_center);
  }
  tab5_botao(d, a.x + MARGEM, a.y + 340, a.largura - 2 * MARGEM, BOTAO_A, tr("Lista"), TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE, TAB5_BOTAO_TEXTO_GRANDE);
}

static void desenhar_player(lgfx::LovyanGFX& d, const area_t& a, const tab5_video_estado_t& e, bool completa)
{
  if (e.erro || e.l == 0) { desenhar_aviso(d, a, e, completa); return; }
  const geo_t g = geometria(a, e.l, e.a);

  // ---- the frame: from the PPA to the surface; here we only notify the compositor ----
  const area_t ret = { a.x, a.y + g.vy, a.largura, g.vh };
  superficie_t sup;
  if (!sistema_superficie(ret, &sup)) {
    if (completa) {
      d.fillRect(a.x, a.y, a.largura, a.altura, TEMA_FUNDO);
      tab5_escrever(d, tr("Sem memória para o vídeo"), a.x + a.largura / 2, a.y + 200, 30, false, TEMA_PERIGO, TEMA_FUNDO, middle_center);
    }
    return;
  }
  // Full pass: the area was cleared, the frame already shown has to come back.
  if (tab5_video_pintar(sup.buf, sup.bytes, sup.pic_l, sup.pic_a, sup.x, sup.y, ret.largura, ret.altura, !completa)) {
    sistema_superficie_mudou(ret);
  }

  // ---- the controls: only when what they show changed ----
  const int barra_px = e.dur_ms ? (int)((uint64_t)e.pos_ms * (uint64_t)g.bl / e.dur_ms) : 0;
  const int sig = (int)((e.pos_ms / 1000) * 4 + (e.tocando ? 2 : 0) + (e.fim ? 1 : 0)) ^ (barra_px << 20);
  if (!pintura_mudou_i(&s_p, &s_sig, sig)) { return; }

  const int y0 = a.y + g.vy + g.vh;
  d.fillRect(a.x, y0, a.largura, a.altura - g.vy - g.vh, TEMA_FUNDO);

  // the name
  tab5_escrever(d, e.nome, a.x + MARGEM, y0 + 8, 26, true, TEMA_TEXTO, TEMA_FUNDO, top_left, a.largura - 2 * MARGEM);

  // the progress bar
  const int bx = a.x + BARRA_X, by = a.y + g.by;
  d.fillRoundRect(bx, by, g.bl, BARRA_A, BARRA_A / 2, TEMA_SUPERFICIE_ALTA);   // track
  if (barra_px > 0) { d.fillRoundRect(bx, by, barra_px < BARRA_A ? BARRA_A : barra_px, BARRA_A, BARRA_A / 2, TEMA_DESTAQUE); }
  d.fillCircle(bx + (barra_px < BARRA_A / 2 ? BARRA_A / 2 : barra_px), by + BARRA_A / 2, 14, TEMA_TEXTO);

  // the time: position on the left, duration on the right, and (small) what the service measures
  char t1[16], t2[16], diag[64];
  tempo_txt(e.pos_ms, t1, sizeof(t1));
  tempo_txt(e.dur_ms, t2, sizeof(t2));
  tab5_escrever(d, t1, bx, by + 32, 26, false, TEMA_TEXTO, TEMA_FUNDO, top_left);
  tab5_escrever(d, t2, bx + g.bl, by + 32, 26, false, TEMA_TEXTO_2, TEMA_FUNDO, top_right);
  snprintf(diag, sizeof(diag), "%.0f %s  %u %s%s", e.fps, tr("qps"), (unsigned)e.perdidos, tr("pulados"),
           e.com_som ? "" : tr("  (sem som)"));
  tab5_escrever(d, diag, bx + g.bl / 2, by + 36, 20, false, TEMA_TEXTO_2, TEMA_FUNDO, top_center);

  // the buttons
  const char* txt = e.fim ? tr("De novo") : (e.tocando ? tr("Pausar") : tr("Tocar"));
  tab5_botao(d, a.x + MARGEM, a.y + g.bty, g.bl1, BOTAO_A, txt, TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE, TAB5_BOTAO_TEXTO_GRANDE);
  tab5_botao(d, a.x + 2 * MARGEM + g.bl1, a.y + g.bty, g.bl1, BOTAO_A, tr("Lista"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO, TAB5_BOTAO_TEXTO_GRANDE);
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);
  if (!s_player) { desenhar_lista(d, a, pintura_completa(&s_p)); return; }
  lista_esconder(&s_lista);
  tab5_video_estado_t e;
  tab5_video_estado(&e);
  desenhar_player(d, a, e, pintura_completa(&s_p));
}

// ---------------------------------------------------------------------------
// NOTICES - the service changed or the worker finished
// ---------------------------------------------------------------------------
static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carregando && s_pronto.load()) {
    s_carregando = false;
    linha_t* t = s_lin; s_lin = s_tras; s_tras = t;
    s_n = s_n_tras;
    s_sem_cartao = s_sem_cartao_tras;
    lista_itens_mudaram(&s_lista, s_n);
    sistema_pedir_redesenho_completo();
  }
  if (av & AVISO_VIDEO) {
    tab5_video_estado_t e;
    tab5_video_estado(&e);
    // THE SCREEN SWITCHED (a video opened or closed) or the video revealed the frame
    // size: rebuild, with the area cleared.
    if (e.aberto != s_player || e.l != s_ql || e.a != s_qa) {
      const bool voltou = s_player && !e.aberto;
      s_player = e.aberto;
      s_ql = e.l;
      s_qa = e.a;
      s_sig = -1;
      if (voltou) { carregar(); }
      sistema_pedir_redesenho_completo();
    }
    // The screen stays on while playing: without touch, idle would blank the film.
    const bool toca = e.aberto && e.tocando;
    if (toca != s_acordado) {
      s_acordado = toca;
      sistema_manter_acordado(SISTEMA_ACORDADO_APP, toca);
    }
  }
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// TOUCH - x and y RELATIVE to the area
// ---------------------------------------------------------------------------
static void abrir_video(const linha_t& l)
{
  char caminho[sizeof(PASTA_VIDEOS) + TAB5_ARQ_NOME_MAX + 2];
  snprintf(caminho, sizeof(caminho), "%s/%s", PASTA_VIDEOS, l.nome);
  if (!tab5_video_abrir(caminho)) { ESP_LOGW(TAG, "did not open %s", caminho); }
  // The screen switches on the AVISO_VIDEO the service publishes.
}

static void voltar_para_lista(void)
{
  tab5_video_fechar();   // stops the sound and releases the disk; the notice goes back to the list
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  if (!s_player && s_n > 0 && lista_arrastar(&s_lista, x, y, pressionado)) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  if (!s_player) {
    int dy = 0;
    const int i = lista_item_tocado(&s_lista, x, y, &dy);
    if (i >= 0 && i < s_n) { abrir_video(s_lin[i]); }
    return;
  }
  tab5_video_estado_t e;
  tab5_video_estado(&e);
  const area_t a = sistema_area();
  if (e.erro || e.l == 0) {   // there is only the Lista button
    if (tab5_dentro(x, y, MARGEM, 340, a.largura - 2 * MARGEM, BOTAO_A)) { voltar_para_lista(); }
    return;
  }
  const geo_t g = geometria(a, e.l, e.a);
  if (tab5_dentro(x, y, 0, g.vy, a.largura, g.vh)) { tab5_video_pausar_tocar(); return; }   // tap on the video
  if (y >= g.by - 36 && y < g.by + BARRA_A + 36 && e.dur_ms) {                                 // on the bar: seek
    int px = x - BARRA_X;
    if (px < 0) { px = 0; }
    if (px > g.bl) { px = g.bl; }
    tab5_video_ir_ms((uint32_t)((uint64_t)px * e.dur_ms / (uint64_t)g.bl));
    return;
  }
  if (tab5_dentro(x, y, MARGEM, g.bty, g.bl1, BOTAO_A)) { tab5_video_pausar_tocar(); return; }
  if (tab5_dentro(x, y, 2 * MARGEM + g.bl1, g.bty, g.bl1, BOTAO_A)) { voltar_para_lista(); }
}

// ---------------------------------------------------------------------------
// OPEN AND CLOSE
// ---------------------------------------------------------------------------
static bool ao_abrir(void)
{
  tab5_video_estado_t e;
  tab5_video_estado(&e);   // opened from Arquivos (Files): the app starts in the player
  s_player = e.aberto;
  s_ql = e.l;
  s_qa = e.a;
  s_sig = -1;
  s_acordado = false;
  s_n = 0;
  s_sem_cartao = false;
  lista_definir(&s_lista, ROLAGEM_INICIO, 0, linha_altura, linha_desenhar, nullptr);
  if (!s_player) { carregar(); }
  else { ao_aviso(AVISO_VIDEO); }   // turns on keep-screen-on if it is already playing
  return true;
}

static void ao_fechar(void)
{
  tab5_video_fechar();   // closing the app stops the film and the sound
  if (s_acordado) { sistema_manter_acordado(SISTEMA_ACORDADO_APP, false); s_acordado = false; }
  s_player = false;
}

// ---------------------------------------------------------------------------
// HOME SCREEN ICON - a cinema square with the play triangle.
// ---------------------------------------------------------------------------
static void icone_video(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  d.fillRoundRect(x, y, lado, lado, lado / 6, d.color565(40, 20, 45));
  const int tela_l = lado * 62 / 100, tela_a = lado * 42 / 100;
  const int tx = x + (lado - tela_l) / 2, ty = y + (lado - tela_a) / 2;
  d.fillRoundRect(tx, ty, tela_l, tela_a, lado / 14, ROXO);
  const int cx = tx + tela_l / 2, cy = ty + tela_a / 2, r = tela_a / 4;
  d.fillTriangle(cx - r * 7 / 10, cy - r, cx - r * 7 / 10, cy + r, cx + r, cy, d.color565(40, 20, 45));
}

extern const app_t APP_VIDEO = {
  .nome  = "Vídeo",
  .cor   = ROXO,
  .perfil = { .alto_falante = true },   // the film's sound (cannot be requested after boot: see ajustes.h)
  .usa_sd = true,
  .exige_sd = false,                    // a video opened from the flash drive also plays
  .orientacao = ORI_FIXO_EM_PE,         // the frame is DMA to the screen at rotation 0
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,
  .periodo_ms = 0,                      // the clock is the frames' (AVISO_VIDEO)
  .avisos    = AVISO_TRABALHO | AVISO_VIDEO,
  .ao_aviso  = ao_aviso,
  .icone     = icone_video,
  .fundo_tema = true,                   // the system clears the area with the theme background
};
