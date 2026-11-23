// MUSIC PLAYER - the heart of the service: state and queue, the controls, the
// playback loop, the background part and swapping the queue. The rest lives in
// tab5_musica_*.cpp (see tab5_musica_interno.hpp).
#include "tab5_musica_interno.hpp"
#include "idioma.h"
#include "app.h"
#include "midia.h"
#include "ajustes.h"
#include "tab5_sd.hpp"
#include "tab5_usb.hpp"
#include "tab5_volumes.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <atomic>
#include <cstdio>
#include <cstring>

#include "minimp3_externo.h"

static const char* TAG = "musica";

// The background part the service uses (defined at the end of the file).
extern const fundo_def_t g_fundo_musica;
// Who asked for the queue (the title in the miniplayer opens this app). A copy: the name of
// a Berry app can vanish when the scripts are reloaded.
static char s_app_dono[32] = "Música";

// ---------------------------------------------------------------------------
// MEMORY - all in PSRAM, allocated in ao_iniciar and released in ao_parar.
// ---------------------------------------------------------------------------
// THREE blocks in rotation: the speaker holds two (the one playing and the
// next), the third is the one being decoded. 16 MP3 frames
// (18432 samples per channel) = 418 ms at 44.1 kHz: with the queue full, ~0.6 s
// of slack for the loop - a card read stalled behind another service's write
// (already measured: spikes of hundreds of ms) does not become a gap in the sound.
// (BLOCO_QUADROS lives in tab5_musica_interno.hpp.)
#define BLOCOS         3

// SD CARD READING - measured on the device: reading straight into PSRAM, at
// an arbitrary address, 16 KB took ~250 ms (60 KB/s) and the sound stuttered on
// every block. The SD driver only DMAs several sectors at once when the
// destination is ALIGNED (and the file position starts on a sector); otherwise it falls
// back to one 512-byte sector at a time, copying. Hence: a DMA-aligned read
// buffer, always read in whole chunks starting at a sector
// boundary, and copied (memcpy, cheap) into the decoder's buffer.
// (LER_PEDACO, ARQ_BUF and SETOR also live in tab5_musica_interno.hpp.)

static int16_t*  s_bloco[BLOCOS] = { };
// The queue: the paths (relative to the music folder), copied from the requester
// (the window may close). A folder or a playlist - in a playlist each
// track may live in a different folder.
//
// The play ORDER is g_musica_ordem (indices into g_musica_fila): the folder's, or
// shuffled. The loop's position is in the ORDER - the next one is
// always pos + 1, with or without shuffle.
caminho_t* g_musica_fila = nullptr;
int16_t*   g_musica_ordem = nullptr;
int        g_musica_fila_n = 0;
char       g_musica_fila_raiz[160] = "";   // the music folder, absolute path
// The queue changed (musica_fila_tocar): the loop rechecks which disks to hold.
static std::atomic<uint32_t> s_fila_gen{0};

// ---------------------------------------------------------------------------
// STATE - written by the loop, read by the window (a copy, under the lock).
// ---------------------------------------------------------------------------
Trava g_musica_trava;   // queue + state
tab5_musica_estado_t   g_musica_est = { };
static uint64_t s_dec_soma_us = 0;   // diagnostics (tab5_musica_estado_t)

// Requests from the window and the miniplayer: the loop serves them between two blocks.
static std::atomic<int>     s_ped_abrir{-1};     // index in the queue; -1 = none
static std::atomic<bool>    s_ped_alternar{false};
static std::atomic<int>     s_ped_pular{0};      // +1 next, -1 previous
static std::atomic<int64_t> s_ped_ir{-1};        // ms; -1 = none
static std::atomic<bool>    s_ped_ordem{false};  // shuffle changed: rebuild the order

// ---------------------------------------------------------------------------
// THE CONTROLS - from the window and the miniplayer (interface task): they only ask.
// ---------------------------------------------------------------------------
void tab5_musica_tocar_pausar(void) { s_ped_alternar = true; }
void tab5_musica_proxima(void)      { s_ped_pular = 1; }
void tab5_musica_anterior(void)     { s_ped_pular = -1; }
void tab5_musica_ir_para(uint32_t ms) { s_ped_ir = (int64_t)ms; }
// Without waiting (the interface, Berry and the console call it): the kernel stops in its
// loop, and the service exits within milliseconds.
void tab5_musica_parar(void)        { sistema_fundo_parar_nome(g_fundo_musica.nome); }

bool tab5_musica_aleatorio(void) { return ajustes_modo_tocador() & 1; }
tab5_musica_repetir_t tab5_musica_repetir(void)
{
  const int r = (ajustes_modo_tocador() >> 1) & 3;
  return r <= TAB5_MUSICA_REPETIR_UMA ? (tab5_musica_repetir_t)r : TAB5_MUSICA_REPETIR_NAO;
}

void tab5_musica_definir_aleatorio(bool sim)
{
  if (sim == tab5_musica_aleatorio()) { return; }
  ajustes_definir_modo_tocador((uint8_t)((ajustes_modo_tocador() & ~1u) | (sim ? 1u : 0u)));
  s_ped_ordem = true;   // the loop rebuilds the order between two blocks, without cutting the sound
  g_musica_trava.travar();
  g_musica_est.aleatorio = sim;
  g_musica_trava.destravar();
}

void tab5_musica_definir_repetir(tab5_musica_repetir_t r)
{
  ajustes_definir_modo_tocador((uint8_t)((ajustes_modo_tocador() & ~6u) | ((unsigned)r << 1)));
  g_musica_trava.travar();
  g_musica_est.repetir = (uint8_t)r;
  g_musica_trava.destravar();
}

// A truly random number (the chip's RNG; Wi-Fi does not even need to be
// on for it to serve a shuffle).
static uint32_t sorteio(uint32_t n) { return n ? esp_random() % n : 0; }

// THE QUEUE ORDER, under the lock. 'fica' = index in g_musica_fila that has to stay at
// position 'pos_fica' (the track playing does not move as far as the ear is concerned):
// with shuffle, it goes to the start and the rest is shuffled after it;
// without, the order is the folder's and its position is the natural one. Returns the position
// of 'fica' in the new order (-1 = no 'fica').
static int montar_ordem(int fica, bool aleatorio)
{
  if (!g_musica_ordem) { return -1; }
  for (int i = 0; i < g_musica_fila_n; ++i) { g_musica_ordem[i] = (int16_t)i; }
  if (!aleatorio) { return fica; }
  // Fisher-Yates
  for (int i = g_musica_fila_n - 1; i > 0; --i) {
    const int j = (int)sorteio((uint32_t)i + 1);
    const int16_t t = g_musica_ordem[i]; g_musica_ordem[i] = g_musica_ordem[j]; g_musica_ordem[j] = t;
  }
  if (fica < 0) { return -1; }
  for (int i = 0; i < g_musica_fila_n; ++i) {
    if (g_musica_ordem[i] == fica) { g_musica_ordem[i] = g_musica_ordem[0]; g_musica_ordem[0] = (int16_t)fica; break; }
  }
  return 0;
}

static const midia_controles_t CONTROLES = {
  .tocar_pausar = tab5_musica_tocar_pausar,
  .proxima      = tab5_musica_proxima,
  .anterior     = tab5_musica_anterior,
  .ir_para      = tab5_musica_ir_para,
};

void tab5_musica_zerar_diag(void)
{
  g_musica_trava.travar();
  g_musica_est.blocos = g_musica_est.secas = g_musica_est.dec_max_us = g_musica_est.dec_media_us = 0;
  g_musica_est.ler_no_pior_us = g_musica_est.ler_soma_ms = g_musica_est.esp_max_us = 0;
  s_dec_soma_us = 0;
  g_musica_trava.destravar();
}

void tab5_musica_estado(tab5_musica_estado_t* out)
{
  g_musica_trava.travar();
  *out = g_musica_est;
  g_musica_trava.destravar();
  out->ativo = sistema_fundo_ativo(&g_fundo_musica);
  out->aleatorio = tab5_musica_aleatorio();
  out->repetir = (uint8_t)tab5_musica_repetir();
}

// WHERE THE SOUND REALLY IS: g_musica_quadros is what has been QUEUED, up to two
// blocks (~0.8 s) ahead of what comes out of the speaker. For display and for
// pausing, subtract the queue (the playing block counts as half).
static uint64_t quadros_tocando(void)
{
  const int f = sistema_som_fluxo_na_fila();
  const uint64_t atras = f > 0 ? (uint64_t)(f - 1) * g_musica_ult_q + g_musica_ult_q / 2 : 0;
  return g_musica_quadros > atras ? g_musica_quadros - atras : 0;
}

// What the status bar, the miniplayer and the background part's pill show.
static void publicar(fundo_t* f, bool tocando, bool mudou_faixa)
{
  char titulo[64 + 48 + 4];
  uint32_t pos, dur;
  g_musica_trava.travar();
  g_musica_est.tocando = tocando;
  g_musica_est.pos_ms = g_musica_hz ? (uint32_t)(quadros_tocando() * 1000u / g_musica_hz) : 0;
  if (g_musica_fmt == FMT_MP3 && g_musica_mp3_quadros > 20 && g_musica_hz) {
    g_musica_est.kbps = (uint16_t)(g_musica_mp3_kbps_soma / g_musica_mp3_quadros);
    // VBR without Xing: the duration improves with the real average bitrate.
    if (!g_musica_dur_exata && g_musica_est.kbps) {
      g_musica_total_quadros = (uint64_t)(g_musica_dados_fim - g_musica_dados_ini) * 8u * g_musica_hz / ((uint64_t)g_musica_est.kbps * 1000u);
      g_musica_est.dur_ms = (uint32_t)(g_musica_total_quadros * 1000u / g_musica_hz);
    }
  }
  if (g_musica_est.artista[0]) { snprintf(titulo, sizeof(titulo), "%s - %s", g_musica_est.titulo, g_musica_est.artista); }
  else                  { snprintf(titulo, sizeof(titulo), "%s", g_musica_est.titulo); }
  pos = g_musica_est.pos_ms;
  dur = g_musica_est.dur_ms;
  g_musica_trava.destravar();
  midia_publicar(titulo, tocando, pos, dur);
  if (mudou_faixa) {
    char r[NOTIF_RESUMO];
    snprintf(r, sizeof(r), "%s", titulo);
    fundo_estado(f, 1, r);
  }
}

void tab5_musica_definir_cor(uint16_t cor565)
{
  ajustes_definir_cor_tocador(cor565);
  midia_definir_cor(cor565);
}

static void calar_fila(void)
{
  sistema_som_fluxo_calar();
  musica_espectro_esquecer();
}

// ---------------------------------------------------------------------------
// THE DISKS THE QUEUE USES - the loop holds each one while the queue has
// music on it, and only that: playing from the USB drive, the card stays free (it can go
// to the computer without stopping the sound), and vice versa.
// ---------------------------------------------------------------------------
typedef struct { bool cartao, pendrive; } discos_t;

static discos_t discos_da_fila(void)
{
  discos_t d = { false, false };
  g_musica_trava.travar();
  for (int i = 0; i < g_musica_fila_n && !(d.cartao && d.pendrive); ++i) {
    const tab5_vol_id_t v = absoluto(g_musica_fila[i]) ? tab5_volume_de(g_musica_fila[i]) : TAB5_VOL_CARTAO;
    if (v == TAB5_VOL_CARTAO)   { d.cartao = true; }
    if (v == TAB5_VOL_PENDRIVE) { d.pendrive = true; }
  }
  g_musica_trava.destravar();
  return d;
}

// Holds what the queue needs and releases what it no longer needs. 'seg' =
// what the loop holds now.
static void segurar_discos(discos_t* seg)
{
  const discos_t q = discos_da_fila();
  if (q.cartao && !seg->cartao)        { seg->cartao = tab5_sd_montar(); }
  else if (!q.cartao && seg->cartao)   { tab5_sd_desmontar(); seg->cartao = false; }
  if (q.pendrive && !seg->pendrive)      { seg->pendrive = tab5_usb_montar(); }
  else if (!q.pendrive && seg->pendrive) { tab5_usb_desmontar(); seg->pendrive = false; }
}

static void soltar_discos(discos_t* seg)
{
  if (seg->cartao)   { tab5_sd_desmontar(); seg->cartao = false; }
  if (seg->pendrive) { tab5_usb_desmontar(); seg->pendrive = false; }
}

static void erro_laco(const char* t)
{
  g_musica_trava.travar();
  snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "%s", t);
  g_musica_trava.destravar();
}

// ---------------------------------------------------------------------------
// THE LOOP
// ---------------------------------------------------------------------------
static void laco_musica(fundo_t* f)
{
  discos_t seg = { false, false };
  uint32_t gen = s_fila_gen.load();
  segurar_discos(&seg);
  if (!seg.cartao && !seg.pendrive) {
    erro_laco(discos_da_fila().pendrive ? tr("o pendrive não está conectado") : tr("sem cartão SD"));
    return;
  }
  midia_assumir(g_fundo_musica.nome, s_app_dono, &CONTROLES);
  midia_definir_cor(ajustes_cor_tocador());
  bool tocando = false, som = false;
  bool acabou = false;   // the track ended by itself (the next one comes in without cutting the tail)
  int vez = 0;
  int idx = -1;
  bool recomeco = true;   // the next block restarts the queue (an empty queue is normal)
  int64_t pausado_desde = 0;   // 0 = playing

  while (fundo_rodando(f)) {
    // THE QUEUE CHANGED: perhaps another disk (from the card to the USB drive...).
    if (s_fila_gen.load() != gen) { gen = s_fila_gen.load(); segurar_discos(&seg); }
    // THE CARD IS GOING TO THE COMPUTER (USB drive mode): release everything now - the
    // lender waits at most 3 s for the card's owners. Playing only from the
    // USB-A flash drive, the card is not even held: the sound carries on.
    if (seg.cartao && tab5_sd_emprestado()) { ESP_LOGI(TAG, "card lent: stopping"); break; }
    // THE USB-A FLASH DRIVE WAS PULLED OUT: whoever removes it waits at most 3 s.
    if (seg.pendrive && !tab5_usb_presente()) {
      ESP_LOGI(TAG, "the USB drive was removed: stopping");
      erro_laco("o pendrive foi retirado");
      break;
    }

    // ---- requests ----
    const int abrir = s_ped_abrir.exchange(-1);
    int pular = s_ped_pular.exchange(0);
    // Natural end: what is already in the speaker queue is the tail of this
    // track - it plays to the end, and the next one joins without silence.
    const bool natural = acabou && !pular && abrir < 0;
    if (natural) { pular = 1; }
    acabou = false;
    if (abrir >= 0 || (pular && !natural) || s_ped_ir.load() >= 0) { recomeco = true; }
    if (abrir >= 0) {
      pular = 0;
      if (som) { calar_fila(); }
      idx = abrir;
      tocando = musica_abrir_item(idx);
      if (!tocando) { pular = 1; }   // bad file: move along the queue
      s_ped_ir = -1;
      publicar(f, tocando, true);
    }
    if (s_ped_ordem.exchange(false)) {
      // Shuffle changed: the current track stays, the rest is rearranged.
      g_musica_trava.travar();
      const int atual = (idx >= 0 && idx < g_musica_fila_n && g_musica_ordem) ? g_musica_ordem[idx] : -1;
      const int nova = montar_ordem(atual, tab5_musica_aleatorio());
      if (nova >= 0) { idx = nova; g_musica_est.idx = nova; }
      g_musica_trava.destravar();
      publicar(f, tocando, false);
    }
    if (s_ped_alternar.exchange(false) && musica_arquivo_aberto()) {
      tocando = !tocando;
      if (!tocando && som) {
        // Pause at the point that was HEARD: what was in the queue is discarded, and
        // reading goes back there (otherwise resuming would skip ~0.8 s).
        const uint64_t ouvido = quadros_tocando();
        sistema_som_fluxo_fechar();
        musica_espectro_esquecer();
        som = false;
        if (g_musica_hz) { musica_ir_para_ms((uint32_t)(ouvido * 1000u / g_musica_hz)); }
      }
      publicar(f, tocando, false);
    }
    if (pular) {
      // "Previous" after 3 s goes back to the start of the same track, like every player.
      if (pular < 0 && g_musica_hz && g_musica_quadros > (uint64_t)g_musica_hz * 3) {
        musica_ir_para_ms(0);
        if (som) { calar_fila(); }
      } else {
        int n;
        g_musica_trava.travar(); n = g_musica_fila_n; g_musica_trava.destravar();
        const tab5_musica_repetir_t rep = tab5_musica_repetir();
        // Repeat one: at the natural end, the same one again (no silence). The
        // buttons move along the queue the same way.
        int novo = (natural && rep == TAB5_MUSICA_REPETIR_UMA) ? idx : idx + pular;
        const int passo = pular > 0 ? 1 : -1;
        bool achou = false;
        // Skip files that do not open, until going round once.
        if (som && !natural) { calar_fila(); }
        for (int k = 0; k < n; ++k, novo += passo) {
          if (novo < 0 || novo >= n) {
            // Went past the end: only continues with repeat all.
            if (rep != TAB5_MUSICA_REPETIR_TUDO) { break; }
            if (novo >= n) {
              novo = 0;
              if (tab5_musica_aleatorio() && n > 1) {
                // Reshuffle, without immediately repeating the one that ended.
                g_musica_trava.travar();
                const int ultima = (idx >= 0 && idx < n) ? g_musica_ordem[idx] : -1;
                montar_ordem(-1, true);
                if (g_musica_ordem[0] == ultima) { g_musica_ordem[0] = g_musica_ordem[1]; g_musica_ordem[1] = (int16_t)ultima; }
                g_musica_trava.destravar();
              }
            } else {
              novo = n - 1;
            }
          }
          if (musica_abrir_item(novo)) { achou = true; break; }
        }
        if (achou) { idx = novo; tocando = true; }
        else if (pular > 0) { ESP_LOGI(TAG, "end of queue"); break; }   // finished: shut down
        else { musica_ir_para_ms(0); }
      }
      publicar(f, tocando, true);
    }
    const int64_t ir = s_ped_ir.exchange(-1);
    if (ir >= 0 && musica_arquivo_aberto()) {
      if (som) { calar_fila(); }
      musica_ir_para_ms((uint32_t)ir);
      publicar(f, tocando, false);
    }

    // ---- paused: wait for the next request ----
    // A LONG PAUSE ENDS IT (contract in kernel/fundo.h): forgotten while paused, the
    // player would occupy a background slot and the status bar forever.
    if (!tocando || !musica_arquivo_aberto()) {
      const int64_t agora = esp_timer_get_time();
      // With nothing to play (the list ended, stopped) there is nothing to resume:
      // end straight away and the pill leaves the status bar.
      const int max_s = !musica_arquivo_aberto() ? TAB5_MUSICA_VAZIO_MAX_S : TAB5_MUSICA_PAUSA_MAX_S;
      if (!pausado_desde) { pausado_desde = agora; }
      else if (agora - pausado_desde > (int64_t)max_s * 1000000) {
        ESP_LOGI(TAG, "%s for %d s: shutting down", !musica_arquivo_aberto() ? "nothing to play" : "paused", max_s);
        break;
      }
      if (!fundo_esperar(f, 100)) { break; }
      continue;
    }
    pausado_desde = 0;

    // ---- playing ----
    if (!som) {
      if (!sistema_som_fluxo_abrir(2000)) {
        g_musica_trava.travar();
        snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "alto-falante ocupado");
        g_musica_trava.destravar();
        tocando = false;
        publicar(f, false, false);
        continue;
      }
      som = true;
      recomeco = true;
    }
    if (!sistema_som_fluxo_livre()) {
      if (!fundo_esperar(f, 15)) { break; }
      continue;
    }
    uint32_t hz = 0;
    bool estereo = true;
    g_musica_ler_us = 0;
    const int64_t t0 = esp_timer_get_time();
    const size_t q = musica_decodificar_bloco(s_bloco[vez], &hz, &estereo);
    const uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t0);
    const int na_fila = sistema_som_fluxo_na_fila();   // AFTER: the gap happens while decoding
    if (q == 0) { acabou = true; continue; }   // the next in the queue, on the next round
    g_musica_trava.travar();
    ++g_musica_est.blocos;
    if (na_fila == 0 && !recomeco) {
      ++g_musica_est.secas;
      ESP_LOGW(TAG, "the speaker queue ran dry (block %lu: %lu us, %lu of them reading the card)",
               (unsigned long)g_musica_est.blocos, (unsigned long)dec_us, (unsigned long)g_musica_ler_us);
    }
    if (dec_us > g_musica_est.dec_max_us) { g_musica_est.dec_max_us = dec_us; g_musica_est.ler_no_pior_us = g_musica_ler_us; }
    g_musica_est.ler_soma_ms += g_musica_ler_us / 1000;
    s_dec_soma_us += dec_us;
    g_musica_est.dec_media_us = (uint32_t)(s_dec_soma_us / g_musica_est.blocos);
    g_musica_est.bloco_ms = hz ? (uint32_t)(q * 1000u / hz) : 0;
    g_musica_trava.destravar();
    recomeco = false;
    // WHEN THIS BLOCK GOES OUT: straight away (empty queue) or when what is already
    // queued finishes - it is the clock for the spectrum bars.
    const int64_t agora_us = esp_timer_get_time();
    const int64_t ini_us = (na_fila == 0 || g_musica_fim_prev_us < agora_us) ? agora_us + LATENCIA_US : g_musica_fim_prev_us;
    g_musica_fim_prev_us = ini_us + (int64_t)q * 1000000 / hz;
    if (!sistema_som_fluxo_tocar(s_bloco[vez], q, hz, estereo)) {
      ESP_LOGI(TAG, "the speaker was taken (USB drive): stopping");
      break;
    }
    {
      const int64_t t0e = esp_timer_get_time();
      musica_espectro_bloco(s_bloco[vez], q, hz, estereo, ini_us);
      const uint32_t esp_us = (uint32_t)(esp_timer_get_time() - t0e);
      if (esp_us > g_musica_est.esp_max_us) { g_musica_est.esp_max_us = esp_us; }
    }
    vez = (vez + 1) % BLOCOS;
    g_musica_ult_q = (uint32_t)q;
    const uint32_t seg_antes = g_musica_hz ? (uint32_t)(g_musica_quadros / g_musica_hz) : 0;
    g_musica_quadros += q;
    if (g_musica_hz && (uint32_t)(g_musica_quadros / g_musica_hz) != seg_antes) { publicar(f, true, false); }
  }

  musica_fechar_arquivo();
  if (som) { sistema_som_fluxo_fechar(); }
  musica_espectro_esquecer();
  midia_soltar();
  soltar_discos(&seg);
}

static void liberar(void)
{
  for (int i = 0; i < BLOCOS; ++i) { heap_caps_free(s_bloco[i]); s_bloco[i] = nullptr; }
  heap_caps_free(g_musica_arq_buf); g_musica_arq_buf = nullptr;
  heap_caps_free(g_musica_ler_buf); g_musica_ler_buf = nullptr;
  musica_espectro_soltar();
  heap_caps_free(g_musica_quadro); g_musica_quadro = nullptr;
  heap_caps_free(g_musica_mp3); g_musica_mp3 = nullptr;
  heap_caps_free(minimp3_scratch); minimp3_scratch = nullptr;
}

// On the interface, before the kernel starts the loop: all the memory, in PSRAM.
// If short: release what it got and refuse (the kernel does not call ao_parar).
static bool ao_iniciar(void)
{
  bool ok = true;
  for (int i = 0; i < BLOCOS; ++i) {
    s_bloco[i] = (int16_t*)heap_caps_malloc(BLOCO_QUADROS * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ok = ok && s_bloco[i];
  }
  g_musica_arq_buf = (uint8_t*)heap_caps_malloc(ARQ_BUF, MALLOC_CAP_SPIRAM);
  g_musica_ler_buf = (uint8_t*)heap_caps_aligned_alloc(128, LER_PEDACO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  g_musica_quadro = (int16_t*)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  g_musica_mp3 = (mp3dec_t*)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
  // In PSRAM: measured A/B against internal RAM, the time per block was the
  // same (~15-20 ms for 156 ms of sound) - not worth spending 23 KB of internal RAM.
  minimp3_scratch = heap_caps_malloc(minimp3_scratch_bytes(), MALLOC_CAP_SPIRAM);
  ok = ok && g_musica_arq_buf && g_musica_ler_buf && g_musica_quadro && g_musica_mp3 && minimp3_scratch && musica_espectro_alocar();
  if (ok) { sistema_recurso_usar(RECURSO_SOM, "Música"); }
  if (!ok) {
    ESP_LOGE(TAG, "no PSRAM for the player");
    g_musica_trava.travar();
    snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "sem memória");
    g_musica_trava.destravar();
    liberar();
  }
  return ok;
}

static void ao_parar(void)
{
  sistema_recurso_soltar(RECURSO_SOM, "Música");
  liberar();
  g_musica_trava.travar();
  g_musica_est.tocando = false;
  g_musica_est.pos_ms = 0;
  g_musica_trava.destravar();
  ESP_LOGI(TAG, "stopped");
}

static bool sem_bip(void) { return false; }   // music does not "notify" with a beep

// 6 KB stack: the pool's biggest slot. The minimp3 scratch (16 KB) and the
// buffers are in PSRAM; only the decoder's variables live on the stack.
// While playing, it declares RECURSO_SOM (ao_iniciar/ao_parar): its policy holds the
// clock at 180 MHz with the screen off - at the minimum (40) MP3 does not decode in
// time and the sound fails.
const fundo_def_t g_fundo_musica = {
  .nome       = "Música",
  .corpo      = laco_musica,
  .pilha      = 6144,
  .perfil     = { .alto_falante = true },
  .ao_iniciar = ao_iniciar,
  .ao_parar   = ao_parar,
  .com_som    = sem_bip,
  // Above the interface (4) and the camera capture (3): with both running the
  // decoder at 1 never got a turn and the sound stuttered. It fits because the loop
  // decodes a block (~418 ms of sound) in a fraction of that and sleeps until
  // the speaker queue has room - see fundo_def_t.prioridade.
  .prioridade = 5,
};

// SWAPS THE QUEUE and starts the player (or just swaps, if it was already playing). 'itens' = 'n'
// paths relative to 'raiz'; 'inicio' = the index in itens that plays
// first (-1 = the start of the order). 'pasta_rel'/'playlist' say where the
// queue came from (one of the two).
bool musica_fila_tocar(const caminho_t* itens, int n, int inicio, const char* raiz,
                       const char* pasta_rel, const char* playlist, const char* app)
{
  if (n <= 0) { return false; }
  if (n > TAB5_MUSICA_FILA_MAX) { n = TAB5_MUSICA_FILA_MAX; }
  if (inicio >= n) { inicio = -1; }
  g_musica_trava.travar();
  if (!g_musica_fila)  { g_musica_fila  = (caminho_t*)heap_caps_malloc(sizeof(caminho_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM); }
  if (!g_musica_ordem) { g_musica_ordem = (int16_t*)heap_caps_malloc(sizeof(int16_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM); }
  if (!g_musica_fila || !g_musica_ordem) { g_musica_trava.destravar(); return false; }
  memcpy(g_musica_fila, itens, sizeof(caminho_t) * (size_t)n);
  g_musica_fila_n = n;
  ++s_fila_gen;
  snprintf(g_musica_fila_raiz, sizeof(g_musica_fila_raiz), "%s", raiz);
  const bool alea = tab5_musica_aleatorio();
  int pos = montar_ordem(inicio, alea);
  if (pos < 0) { pos = 0; }
  if (pasta_rel) {
    snprintf(g_musica_est.rel, sizeof(g_musica_est.rel), "%s", pasta_rel);
    if (absoluto(pasta_rel)) { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s", pasta_rel); }
    else if (pasta_rel[0])   { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s/%s", raiz, pasta_rel); }
    else                     { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s", raiz); }
  } else {
    g_musica_est.rel[0] = g_musica_est.pasta[0] = '\0';
  }
  snprintf(g_musica_est.playlist, sizeof(g_musica_est.playlist), "%s", playlist ? playlist : "");
  g_musica_est.n = n;
  g_musica_est.erro[0] = '\0';
  g_musica_est.aleatorio = alea;
  g_musica_est.repetir = (uint8_t)tab5_musica_repetir();
  snprintf(s_app_dono, sizeof(s_app_dono), "%s", app && app[0] ? app : g_fundo_musica.nome);
  g_musica_trava.destravar();

  s_ped_alternar = false;
  s_ped_pular = 0;
  s_ped_ir = -1;
  s_ped_ordem = false;
  s_ped_abrir = pos;
  if (sistema_fundo_ativo(&g_fundo_musica)) {
    // Already playing: the queue swaps, and the miniplayer now opens whoever asked now.
    midia_assumir(g_fundo_musica.nome, s_app_dono, &CONTROLES);
    midia_definir_cor(ajustes_cor_tocador());
  } else if (!sistema_fundo_iniciar(&g_fundo_musica)) {
    s_ped_abrir = -1;
    return false;
  }
  return true;
}

bool tab5_musica_tocar_um(const char* caminho, const char* app)
{
  if (!absoluto(caminho) || strlen(caminho) >= sizeof(caminho_t)) { return false; }
  caminho_t* fila = (caminho_t*)heap_caps_malloc(sizeof(caminho_t), MALLOC_CAP_SPIRAM);
  if (!fila) { return false; }
  snprintf(*fila, sizeof(caminho_t), "%s", caminho);
  // Absolute path: the queue root is not used; there is no source folder.
  const bool ok = musica_fila_tocar(fila, 1, 0, "", nullptr, nullptr, app);
  heap_caps_free(fila);
  return ok;
}

