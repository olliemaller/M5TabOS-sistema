// SYSTEM VIDEO PLAYER - see tab5_video.hpp (the contract) and the end of this
// file (the public API). Here: the AVI reader, the decoding task and the
// PPA that scales to the screen.

#include "tab5_video.hpp"
#include "tab5_volumes.hpp"
#include "tab5_musica.hpp"
#include "tab5_mutex.hpp"
#include "tab5_h264.hpp"              // the H.264 decoder (MP4)
#include "tab5_midia.hpp"             // the MP4 reader (components/medf)
#include "medf_mp4.h"
#include "app.h"                  // sistema_avisar, AVISO_VIDEO
#include "idioma.h"                // N_: error texts the app translates

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   // task with its stack in PSRAM
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <strings.h>

static const char* TAG = "video";

// THE COLOUR ORDER OUT OF THE DECODER. BGR = the panel's 565 (red in the
// high bits). If red and blue come out swapped on the device, just flip it here.
#define VIDEO_ORDEM_RGB  JPEG_DEC_RGB_ELEMENT_ORDER_BGR

#define VIDEO_PILHA        12288  // task stack (PSRAM)
#define VIDEO_CAMINHO_MAX  256    // absolute path of the video and the mp3
#define VIDEO_PRIO         3      // above the worker (2), below the radio services
#define VIDEO_ESPERA_SOM_MS 3000  // how long it waits for the audio to start before carrying on without it
#define VIDEO_FOLGA_SOM_MS  1500  // how far the video clock may run ahead of the music's last position
#define VIDEO_FALHAS_MAX   30     // consecutive frames that fail to decode = give up

// ---------------------------------------------------------------------------
// THE AVI FILE
// ---------------------------------------------------------------------------
// An AVI is a RIFF: a sequence of chunks [4-byte id][4-byte little-endian
// size][data][1 alignment byte if the size is odd]. The ones that
// matter: LIST 'hdrl' (headers), LIST 'movi' (the frames: "00dc"
// chunks) and 'idx1' (the index: where each frame is).
#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FCC_RIFF FCC('R', 'I', 'F', 'F')
#define FCC_AVI  FCC('A', 'V', 'I', ' ')
#define FCC_LIST FCC('L', 'I', 'S', 'T')
#define FCC_HDRL FCC('h', 'd', 'r', 'l')
#define FCC_STRL FCC('s', 't', 'r', 'l')
#define FCC_MOVI FCC('m', 'o', 'v', 'i')
#define FCC_REC  FCC('r', 'e', 'c', ' ')
#define FCC_AVIH FCC('a', 'v', 'i', 'h')
#define FCC_STRH FCC('s', 't', 'r', 'h')
#define FCC_STRF FCC('s', 't', 'r', 'f')
#define FCC_IDX1 FCC('i', 'd', 'x', '1')
#define FCC_VIDS FCC('v', 'i', 'd', 's')
#define FCC_MJPG FCC('M', 'J', 'P', 'G')
#define FCC_MJPG_MIN FCC('m', 'j', 'p', 'g')

// Where a frame is: the position of the DATA in the file and the size. tam = 0 =
// "repeat the previous frame" (empty frame, too big or outside the file):
// the timing stays right.
typedef struct { uint32_t off, tam; } quadro_t;

// What the headers say.
typedef struct {
  uint32_t us_quadro;     // microseconds between frames (0 = not stated)
  uint32_t l, a;          // from the header (the first frame's JPEG rules)
  int      v;             // video stream number (the "00" of "00dc")
  bool     tem_video;
  bool     mjpeg;
} cab_t;

static uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool ler_em(tab5_leitor_t* r, uint64_t pos, void* dst, size_t n)
{
  return tab5_leitor_ir(r, pos) && tab5_leitor_ler(r, dst, n) == (int)n;
}

// Reads the header (id and size) of the chunk at 'pos'. false = does not fit before 'fim'.
static bool pedaco(tab5_leitor_t* r, uint64_t pos, uint64_t fim, uint32_t* id, uint32_t* tam)
{
  uint8_t c[8];
  if (pos + 8 > fim || !ler_em(r, pos, c, 8)) { return false; }
  *id = u32(c);
  *tam = u32(c + 4);
  return true;
}

// Where the next chunk starts after the one at 'pos' (with alignment).
static uint64_t depois(uint64_t pos, uint32_t tam) { return pos + 8 + (uint64_t)tam + (tam & 1); }

// A LIST 'strl' (the data of ONE stream): strh gives the type, strf the format.
static void ler_strl(tab5_leitor_t* r, uint64_t ini, uint64_t fim, int numero, cab_t* c)
{
  uint32_t tipo = 0, handler = 0, escala = 0, taxa = 0, compressao = 0, l = 0, a = 0;
  uint32_t id, tam;
  int n = 0;
  for (uint64_t p = ini; n < 32 && pedaco(r, p, fim, &id, &tam); p = depois(p, tam), ++n) {
    uint8_t b[40];
    if (id == FCC_STRH && tam >= 28 && ler_em(r, p + 8, b, 28)) {
      tipo = u32(b); handler = u32(b + 4); escala = u32(b + 20); taxa = u32(b + 24);
    } else if (id == FCC_STRF && tam >= 20 && ler_em(r, p + 8, b, 20)) {
      l = u32(b + 4); a = (uint32_t)(int32_t)u32(b + 8); compressao = u32(b + 16);
      if ((int32_t)a < 0) { a = (uint32_t)(-(int32_t)a); }   // negative height = top-down
    }
  }
  if (tipo != FCC_VIDS || c->tem_video) { return; }   // only the first video stream
  c->tem_video = true;
  c->v = numero;
  c->l = l;
  c->a = a;
  c->mjpeg = handler == FCC_MJPG || handler == FCC_MJPG_MIN || compressao == FCC_MJPG;
  if (taxa && escala) { c->us_quadro = (uint32_t)(((uint64_t)escala * 1000000u) / taxa); }
}

// The LIST 'hdrl': avih (the time between frames) and one strl per stream.
static void ler_hdrl(tab5_leitor_t* r, uint64_t ini, uint64_t fim, cab_t* c)
{
  uint32_t id, tam;
  int n = 0, fluxo = 0;
  for (uint64_t p = ini; n < 32 && pedaco(r, p, fim, &id, &tam); p = depois(p, tam), ++n) {
    uint8_t b[4];
    if (id == FCC_AVIH && tam >= 4 && ler_em(r, p + 8, b, 4)) {
      const uint32_t us = u32(b);
      if (!c->us_quadro) { c->us_quadro = us; }
    } else if (id == FCC_LIST && tam >= 4 && ler_em(r, p + 8, b, 4) && u32(b) == FCC_STRL) {
      ler_strl(r, p + 12, p + 8 + (uint64_t)tam, fluxo++, c);
    }
  }
}

// The frame table (grows if needed).
typedef struct {
  quadro_t* v;
  uint32_t  n, cap;
} tabela_t;

static bool tabela_cabe(tabela_t* t, uint32_t quer)
{
  if (quer > TAB5_VIDEO_QUADROS_MAX) { return false; }
  if (quer <= t->cap) { return true; }
  uint32_t cap = t->cap ? t->cap * 2 : 4096;
  while (cap < quer) { cap *= 2; }
  if (cap > TAB5_VIDEO_QUADROS_MAX) { cap = TAB5_VIDEO_QUADROS_MAX; }
  quadro_t* v = (quadro_t*)heap_caps_realloc(t->v, sizeof(quadro_t) * cap, MALLOC_CAP_SPIRAM);
  if (!v) { return false; }
  t->v = v;
  t->cap = cap;
  return true;
}

// Adds a frame whose chunk starts at 'cab' (the 8-byte header) and has
// 'tam' bytes of data. Outside the file or too big = empty frame.
static bool tabela_por(tabela_t* t, uint64_t cab, uint32_t tam, uint64_t limite)
{
  if (!tabela_cabe(t, t->n + 1)) { return false; }
  quadro_t q = { 0, 0 };
  if (tam && tam <= TAB5_VIDEO_QUADRO_MAX && cab + 8 + tam <= limite) {
    q.off = (uint32_t)(cab + 8);
    q.tam = tam;
  }
  t->v[t->n++] = q;
  return true;
}

static bool eh_quadro(uint32_t id, int v)
{
  return id == FCC('0' + v / 10, '0' + v % 10, 'd', 'c');
}

// THE 'idx1' INDEX: 16-byte entries [id][flags][offset][size]. The offset
// is relative to 'movi' in almost every file, but some writers put the
// absolute one: the first frame tells which (the chunk must really be there).
static bool ler_idx1(tab5_leitor_t* r, uint64_t pos, uint64_t tam_idx, uint64_t movi_ini,
                     uint64_t limite, int v, tabela_t* t)
{
  const uint32_t total = (uint32_t)(tam_idx / 16);
  uint8_t bloco[16 * 256];
  bool base_ok = false;
  uint64_t base = 0;
  for (uint32_t i = 0; i < total;) {
    const uint32_t n = (total - i) < 256 ? (total - i) : 256;
    if (!ler_em(r, pos + (uint64_t)i * 16, bloco, (size_t)n * 16)) { return false; }
    for (uint32_t k = 0; k < n; ++k) {
      const uint8_t* e = bloco + k * 16;
      if (!eh_quadro(u32(e), v)) { continue; }
      const uint32_t off = u32(e + 8), tam = u32(e + 12);
      if (!base_ok) {
        // Relative to the 4th byte before the first chunk ('movi'); otherwise absolute.
        const uint64_t cand[2] = { movi_ini - 4, 0 };
        for (int c = 0; c < 2 && !base_ok; ++c) {
          uint32_t id2, tam2;
          if (pedaco(r, cand[c] + off, limite, &id2, &tam2) && id2 == u32(e) && tam2 == tam) {
            base = cand[c];
            base_ok = true;
          }
        }
        if (!base_ok) { return false; }   // the index does not match the file: scan
      }
      if (!tabela_por(t, base + off, tam, limite)) { return false; }
    }
    i += n;
  }
  return t->n > 0;
}

// NO INDEX: walks 'movi' chunk by chunk (reads the whole file).
// "LIST rec " (grouped recording) is entered, other chunks are skipped.
static bool varrer_movi(tab5_leitor_t* r, uint64_t ini, uint64_t fim, uint64_t limite, int v, tabela_t* t)
{
  uint32_t id, tam;
  for (uint64_t p = ini; pedaco(r, p, fim, &id, &tam);) {
    if (id == FCC_LIST) { p += 12; continue; }
    if (eh_quadro(id, v) && !tabela_por(t, p, tam, limite)) { return false; }
    p = depois(p, tam);
  }
  return t->n > 0;
}

// Reads the whole file: header + table. false = not a usable MJPEG AVI
// ('erro' says why).
static bool ler_avi(tab5_leitor_t* r, cab_t* c, tabela_t* t, char* erro, size_t erro_tam)
{
  const uint64_t arq = tab5_leitor_tamanho(r);
  uint8_t h[12];
  if (arq < 12 || !ler_em(r, 0, h, 12) || u32(h) != FCC_RIFF || u32(h + 8) != FCC_AVI) {
    snprintf(erro, erro_tam, N_("não é um AVI"));
    return false;
  }
  uint64_t limite = arq;
  const uint64_t riff = (uint64_t)u32(h + 4) + 8;
  if (riff >= 12 && riff < arq) { limite = riff; }   // junk after the RIFF does not count

  uint64_t movi_ini = 0, movi_fim = 0, idx_pos = 0, idx_tam = 0;
  bool movi_ok = false, idx_ok = false;
  uint32_t id, tam;
  int n = 0;
  for (uint64_t p = 12; n < 4096 && pedaco(r, p, limite, &id, &tam); p = depois(p, tam), ++n) {
    const uint64_t fim_pedaco = p + 8 + (uint64_t)tam;
    uint8_t b[4];
    if (id == FCC_LIST && tam >= 4 && ler_em(r, p + 8, b, 4)) {
      if (u32(b) == FCC_HDRL) {
        ler_hdrl(r, p + 12, fim_pedaco < limite ? fim_pedaco : limite, c);
      } else if (u32(b) == FCC_MOVI && !movi_ok) {
        movi_ok = true;
        movi_ini = p + 12;
        movi_fim = fim_pedaco < limite ? fim_pedaco : limite;   // truncated recording: goes as far as the file does
      }
    } else if (id == FCC_IDX1 && !idx_ok) {
      idx_ok = true;
      idx_pos = p + 8;
      idx_tam = fim_pedaco <= limite ? tam : (limite > p + 8 ? limite - (p + 8) : 0);
    }
  }
  if (!c->tem_video || !movi_ok) { snprintf(erro, erro_tam, N_("AVI sem vídeo")); return false; }
  if (!c->mjpeg) { snprintf(erro, erro_tam, N_("só vídeo MJPEG")); return false; }
  if (c->us_quadro < 5000 || c->us_quadro > 1000000) { c->us_quadro = 40000; }   // 25 frames/s if the file does not say

  bool ok = idx_ok && ler_idx1(r, idx_pos, idx_tam, movi_ini, limite, c->v, t);
  if (!ok) {
    ESP_LOGW(TAG, "no usable index: scanning movi");
    t->n = 0;
    ok = varrer_movi(r, movi_ini, movi_fim, limite, c->v, t);
  }
  if (!ok) { snprintf(erro, erro_tam, N_("AVI sem quadros")); return false; }
  return true;
}

// ---------------------------------------------------------------------------
// THE SESSION - one video at a time. Everything here belongs to the video task,
// except what the lock protects (the published state and the ready frames).
// ---------------------------------------------------------------------------
static Trava s_ctrl;    // open/close: one at a time
static Trava s_trava;   // published state and frames

static tab5_video_estado_t s_est;           // under s_trava
// Three decoded frames (RGB565): the published one, the one the app may be
// scaling and the next one, prepared in advance (see mostrar).
#define VIDEO_BUFS 3
static uint8_t*  s_buf[VIDEO_BUFS];
static size_t    s_buf_bytes;
static int       s_passo, s_ql, s_qa;       // pixels per line, useful width and height
static int       s_ultimo = -1;             // the buffer of the newest published frame
static bool      s_novo = false;            // that frame has not been taken yet
static int       s_preso = -1;              // the buffer the app is using

static TaskHandle_t       s_tarefa = nullptr;
static SemaphoreHandle_t  s_saiu = nullptr;   // the task said: I'm leaving (close waits)
static std::atomic<bool>    s_ped_fechar{false};
static std::atomic<bool>    s_ped_alternar{false};
static std::atomic<int64_t> s_ped_ir{-1};

static char s_caminho[VIDEO_CAMINHO_MAX];
static char s_mp3[VIDEO_CAMINHO_MAX];

static tab5_musica_estado_t s_mus;          // copy of the music state (only the task uses it)

// ---------------------------------------------------------------------------
// PUBLISH
// ---------------------------------------------------------------------------
static void publicar_estado(const tab5_video_estado_t& e)
{
  s_trava.travar();
  const bool aberto = s_est.aberto;
  if (aberto) {
    s_est.tocando = e.tocando; s_est.com_som = e.com_som; s_est.fim = e.fim; s_est.erro = e.erro;
    s_est.pos_ms = e.pos_ms; s_est.dur_ms = e.dur_ms; s_est.quadros = e.quadros;
    s_est.mostrados = e.mostrados; s_est.perdidos = e.perdidos; s_est.l = e.l; s_est.a = e.a;
    s_est.fps = e.fps;
    snprintf(s_est.erro_txt, sizeof(s_est.erro_txt), "%s", e.erro_txt);
  }
  s_trava.destravar();
  sistema_avisar(AVISO_VIDEO);
}

// ---------------------------------------------------------------------------
// THE AUDIO
// ---------------------------------------------------------------------------
// The audio is the music "<video without .avi>.mp3" in the same folder; only counts if it exists.
static bool tem_mp3(void)
{
  snprintf(s_mp3, sizeof(s_mp3), "%s", s_caminho);
  char* ponto = strrchr(s_mp3, '.');
  if (!ponto) { return false; }
  snprintf(ponto, sizeof(s_mp3) - (size_t)(ponto - s_mp3), ".mp3");
  tab5_leitor_t* t = tab5_leitor_abrir(s_mp3);   // only to find out whether it exists
  if (!t) { return false; }
  tab5_leitor_fechar(t);
  return true;
}

static bool som_nosso(void)
{
  tab5_musica_estado(&s_mus);
  return s_mus.ativo && !strcmp(s_mus.caminho, s_mp3);
}

// ---------------------------------------------------------------------------
// THE TASK
// ---------------------------------------------------------------------------
typedef struct {
  tab5_leitor_t*          r;
  tabela_t                tab;
  cab_t                   cab;
  uint8_t*                entrada;       // the frame's JPEG (decoder memory)
  size_t                  entrada_bytes;
  jpeg_decoder_handle_t   dec;
  uint32_t                us_quadro;
  uint32_t                dur_ms;
  // THE CLOCK
  bool                    com_som;       // the clock is the mp3's
  bool                    som_visto;     // the audio has played at some point
  uint32_t                som_ultimo_ms; // the last position the music reported (see relogio_ms)
  bool                    som_pausado;   // we were the ones who paused the audio
  int64_t                 t_som;         // when the audio was requested (esp_timer, us)
  int64_t                 base_us;       // own clock: pos = now - base
  uint32_t                pos_ms;        // last known position
  // WHAT HAS BEEN SHOWN
  int64_t                 ultimo;        // index of the last frame handled (-1 = none)
  bool                    pulo_livre;    // after a seek: the jump does not count as lost
  int64_t                 adiantado;     // index of the frame already decoded in advance (-1 = none)
  int                     adiantado_buf; // the buffer it is in
  int                     falhas;        // consecutive frames that failed to decode
  uint32_t                mostrados, perdidos;
  // DIAGNOSTICS (go to the log at the end): where frames were skipped and what
  // the worst frame cost - card read and decode separately.
  uint32_t                perdidos_1s;   // skipped in the first second (audio still opening)
  uint32_t                pior_ler_us, pior_dec_us;
  uint32_t                lentos;        // frames that took longer than one period (read + decode)
  uint64_t                soma_dec_us;   // for the decode average
  uint32_t                n_dec;         // how many decodes (the average divides by it)
  int64_t                 t_fps;
  uint32_t                n_fps;
  float                   fps;
  // THE MP4 (H.264) - see "O MP4" further down
  bool                    e_mp4;
  medf_arena_t            arena;         // file reading (medf)
  medf_mp4_demux_t*       mp4;
  tab5_h264_t*            h264;
  uint8_t                 nal_tam;       // bytes of the length in front of each NAL (from avcC)
  bool                    acabou_leitura;// the file has no more video packets
  bool                    drenado;       // and the decoder has released those it was holding
  bool                    tem_q;         // a decoded frame is waiting for its time
  tab5_h264_quadro_t      q;
  bool                    alcancar;      // too far behind: keyframes only until catching up with the audio
  int64_t                 chave_us;      // the pts of the last decoded keyframe
  bool                    mostrar_ja;    // after a jump: show the first frame straight away
  int                     escala16;      // the PPA shrinks the frame during conversion (16 = original size)
  bool                    sem_memoria;   // the video does not fit in memory (the decoder said so)
} sessao_t;

static sessao_t s_v;

static void liberar_sessao(void)
{
  // The app may be scaling a frame right now (tab5_video_pintar): wait for it to let go.
  for (int i = 0; i < 40; ++i) {
    s_trava.travar();
    const bool preso = s_preso >= 0;
    s_trava.destravar();
    if (!preso) { break; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (s_v.dec) { jpeg_del_decoder_engine(s_v.dec); s_v.dec = nullptr; }
  if (s_v.h264) {
    if (s_v.tem_q) { tab5_h264_soltar(s_v.h264, s_v.q.id); }
    tab5_h264_fechar(s_v.h264);
  }
  if (s_v.mp4) { medf_mp4_demux_close(s_v.mp4); }
  tab5_midia_arena_soltar(&s_v.arena);
  if (s_v.entrada) { heap_caps_free(s_v.entrada); s_v.entrada = nullptr; }
  s_trava.travar();
  for (int i = 0; i < VIDEO_BUFS; ++i) { if (s_buf[i]) { heap_caps_free(s_buf[i]); s_buf[i] = nullptr; } }
  s_ultimo = -1; s_novo = false; s_preso = -1;
  s_trava.destravar();
  if (s_v.tab.v) { heap_caps_free(s_v.tab.v); }
  if (s_v.r) { tab5_leitor_fechar(s_v.r); }
  memset(&s_v, 0, sizeof(s_v));
}

static void falhar(const char* txt)
{
  ESP_LOGE(TAG, "%s", txt);
  tab5_video_estado_t e;
  tab5_video_estado(&e);
  e.tocando = false;
  e.erro = true;
  snprintf(e.erro_txt, sizeof(e.erro_txt), "%s", txt);
  publicar_estado(e);
}

// Reads frame 'i' from the table into the input. false = read failed.
static bool ler_quadro(uint32_t i)
{
  const quadro_t q = s_v.tab.v[i];
  return ler_em(s_v.r, q.off, s_v.entrada, q.tam);
}

// Opens the file, the index, the decoder and the buffers. false = error already published.
static bool preparar(void)
{
  char erro[64] = "";
  s_v.r = tab5_leitor_abrir(s_caminho);
  if (!s_v.r) { falhar(N_("não abriu o arquivo")); return false; }
  if (!ler_avi(s_v.r, &s_v.cab, &s_v.tab, erro, sizeof(erro))) { falhar(erro); return false; }

  uint32_t maior = 0, primeiro = UINT32_MAX;
  for (uint32_t i = 0; i < s_v.tab.n; ++i) {
    if (s_v.tab.v[i].tam > maior) { maior = s_v.tab.v[i].tam; }
    if (s_v.tab.v[i].tam && primeiro == UINT32_MAX) { primeiro = i; }
  }
  if (primeiro == UINT32_MAX) { falhar(N_("AVI sem quadros")); return false; }

  const jpeg_decode_memory_alloc_cfg_t cfg_in = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
  s_v.entrada = (uint8_t*)jpeg_alloc_decoder_mem(maior, &cfg_in, &s_v.entrada_bytes);
  if (!s_v.entrada) { falhar(N_("sem memória")); return false; }

  // The real size comes from the first frame's JPEG.
  jpeg_decode_picture_info_t info = { };
  if (!ler_quadro(primeiro) || jpeg_decoder_get_info(s_v.entrada, s_v.tab.v[primeiro].tam, &info) != ESP_OK) {
    falhar(N_("JPEG ilegível"));
    return false;
  }
  if (info.width < 16 || info.height < 16 || info.width > TAB5_VIDEO_L_MAX || info.height > TAB5_VIDEO_A_MAX) {
    falhar(N_("quadro grande demais"));
    return false;
  }
  const int passo = ((int)info.width + 15) & ~15, alt = ((int)info.height + 15) & ~15;
  const jpeg_decode_memory_alloc_cfg_t cfg_out = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
  size_t bytes = 0;
  uint8_t* b[VIDEO_BUFS] = { };
  bool tem_todos = true;
  for (int k = 0; k < VIDEO_BUFS; ++k) {
    b[k] = (uint8_t*)jpeg_alloc_decoder_mem((size_t)passo * alt * 2, &cfg_out, &bytes);
    if (!b[k]) { tem_todos = false; }
  }
  const jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 200 };
  const char* falta = !tem_todos ? N_("sem memória")
                    : jpeg_new_decoder_engine(&eng, &s_v.dec) != ESP_OK ? N_("decodificador JPEG ocupado") : nullptr;
  if (falta) {
    for (int k = 0; k < VIDEO_BUFS; ++k) { heap_caps_free(b[k]); }
    falhar(falta);
    return false;
  }
  s_trava.travar();
  for (int k = 0; k < VIDEO_BUFS; ++k) { s_buf[k] = b[k]; }
  s_buf_bytes = bytes;
  s_passo = passo; s_ql = (int)info.width; s_qa = (int)info.height;
  s_trava.destravar();

  s_v.us_quadro = s_v.cab.us_quadro;
  s_v.dur_ms = (uint32_t)(((uint64_t)s_v.tab.n * s_v.us_quadro) / 1000);
  s_v.ultimo = -1;
  s_v.adiantado = -1;
  ESP_LOGI(TAG, "%s: %dx%d, %u frames, %.1f fps, %u s", s_caminho, s_ql, s_qa, (unsigned)s_v.tab.n,
           1e6f / (float)s_v.us_quadro, (unsigned)(s_v.dur_ms / 1000));
  return true;
}

// A free buffer: not the published one, not the one the app is using, not 'fora'
// (the frame decoded in advance that is still valid). With three buffers one is always spare.
static int buffer_livre(int fora)
{
  Trava::Guarda g(s_trava);
  for (int k = 0; k < VIDEO_BUFS; ++k) {
    if (k != s_ultimo && k != s_preso && k != fora) { return k; }
  }
  return -1;
}

// Reads and decodes frame 'i' into buffer 'b'. Measures the cost (diagnostics).
static bool decodificar(uint32_t i, int b)
{
  const jpeg_decode_cfg_t cfg = { .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                  .rgb_order = VIDEO_ORDEM_RGB,
                                  .conv_std = JPEG_YUV_RGB_CONV_STD_BT601 };
  uint32_t saiu = 0;
  const int64_t t0 = esp_timer_get_time();
  const bool lido = ler_quadro(i);
  const int64_t t1 = esp_timer_get_time();
  esp_err_t err = lido ? jpeg_decoder_process(s_v.dec, &cfg, s_v.entrada, s_v.tab.v[i].tam,
                                              s_buf[b], (uint32_t)s_buf_bytes, &saiu)
                       : ESP_FAIL;
  const int64_t t2 = esp_timer_get_time();
  if ((uint32_t)(t1 - t0) > s_v.pior_ler_us) { s_v.pior_ler_us = (uint32_t)(t1 - t0); }
  if ((uint32_t)(t2 - t1) > s_v.pior_dec_us) { s_v.pior_dec_us = (uint32_t)(t2 - t1); }
  if (t2 - t0 > (int64_t)s_v.us_quadro) { ++s_v.lentos; }
  s_v.soma_dec_us += (uint64_t)(t2 - t1);
  s_v.n_dec++;
  if (err != ESP_OK) {
    if (s_v.falhas++ < 3) { ESP_LOGW(TAG, "frame %u: %s", (unsigned)i, lido ? esp_err_to_name(err) : "read failed"); }
    return false;
  }
  s_v.falhas = 0;
  return true;
}

// Publishes frame 'i' and prepares the next one straight away. Always leaves
// s_v.ultimo = i (a frame that fails is not retried).
//
// Why decode ahead: the cost of a frame varies - the average is a few ms, but
// now and then the card or the hardware JPEG take 40-70 ms (PSRAM contention
// with the audio and the screen). Decoded at display time, that frame
// overran its period and was skipped. Prepared right after the previous one, it
// has almost two periods of slack.
static void mostrar(uint32_t i)
{
  if (s_v.ultimo >= 0 && (int64_t)i > s_v.ultimo + 1 && !s_v.pulo_livre) {
    s_v.perdidos += (uint32_t)((int64_t)i - s_v.ultimo - 1);
    if ((uint64_t)i * s_v.us_quadro < 1000000) { s_v.perdidos_1s += (uint32_t)((int64_t)i - s_v.ultimo - 1); }
  }
  s_v.pulo_livre = false;
  s_v.ultimo = i;

  // The frame for this instant: the one decoded ahead, if it is that one; otherwise, decode now.
  int b = -1;
  if (s_v.adiantado == (int64_t)i) {
    b = s_v.adiantado_buf;
  } else if (s_v.tab.v[i].tam != 0) {              // empty frame: the previous one stays on screen
    b = buffer_livre(-1);
    if (b >= 0 && !decodificar(i, b)) { b = -1; }
  }
  s_v.adiantado = -1;                              // the ahead frame was used or is no longer valid

  if (b >= 0) {
    s_trava.travar();
    s_ultimo = b;
    s_novo = true;
    s_trava.destravar();

    ++s_v.mostrados;
    ++s_v.n_fps;
    const int64_t agora = esp_timer_get_time();
    if (agora - s_v.t_fps >= 1000000) {
      s_v.fps = (float)s_v.n_fps * 1e6f / (float)(agora - s_v.t_fps);
      s_v.t_fps = agora;
      s_v.n_fps = 0;
    }
  }

  // Prepare the next one while this one is on screen.
  const uint32_t j = i + 1;
  if (j < s_v.tab.n && s_v.tab.v[j].tam != 0) {
    const int bj = buffer_livre(-1);
    if (bj >= 0 && decodificar(j, bj)) { s_v.adiantado = j; s_v.adiantado_buf = bj; }
  }
}

// Waits for a request or for the time. The controls wake the task by notification.
static void esperar(uint32_t ms)
{
  ulTaskNotifyTake(pdTRUE, ms ? pdMS_TO_TICKS(ms) : 1);
}

// ---- the clock -----------------------------------------------------------
// The position NOW, in ms. With audio, it is the speaker's; without, esp_timer.
static uint32_t relogio_ms(void)
{
  const int64_t agora = esp_timer_get_time();
  if (s_v.com_som) {
    if (som_nosso()) {
      if (!s_mus.tocando) { return s_v.pos_ms; }   // loading or paused from outside: the video waits
      // THE MUSIC POSITION MOVES IN JUMPS: it only changes when a block of
      // audio goes out (hundreds of ms). Followed literally, the video showed
      // one frame per jump and skipped the rest (1 fps with audio, 25 without). So
      // the clock runs on its own (esp_timer) between jumps and REALIGNS on each
      // new music position (the audio rules). The frame shown never goes back
      // (s_v.pos_ms only grows: if the clock got ahead, the picture waits for the audio)
      // and never gets ahead of the music by more than a block or so (audio stalled: wait).
      if (!s_v.som_visto || s_mus.pos_ms != s_v.som_ultimo_ms) {
        s_v.som_ultimo_ms = s_mus.pos_ms;
        s_v.base_us = agora - (int64_t)s_mus.pos_ms * 1000;
        s_v.som_visto = true;
      }
      uint32_t pos = (uint32_t)((agora - s_v.base_us) / 1000);
      if (pos > s_mus.pos_ms + VIDEO_FOLGA_SOM_MS) { pos = s_mus.pos_ms + VIDEO_FOLGA_SOM_MS; }
      if (pos > s_v.pos_ms) { s_v.pos_ms = pos; }
      return s_v.pos_ms;
    }
    if (s_v.som_visto || agora - s_v.t_som > (int64_t)VIDEO_ESPERA_SOM_MS * 1000) {
      // The audio ended before the video (or never came): carry on alone from where it was.
      ESP_LOGI(TAG, "%s: own clock from %u ms", s_v.som_visto ? "audio ended" : "no audio", (unsigned)s_v.pos_ms);
      s_v.com_som = false;
      s_v.base_us = agora - (int64_t)s_v.pos_ms * 1000;
    } else {
      return s_v.pos_ms;   // the audio has not started yet
    }
  }
  s_v.pos_ms = (uint32_t)((agora - s_v.base_us) / 1000);
  return s_v.pos_ms;
}

// Turns the audio on (again): the music plays from the start and, if 'ms' > 0, seeks there.
static void ligar_som(uint32_t ms)
{
  s_v.com_som = false;
  if (!s_mp3[0]) { return; }
  // Switching videos, the previous one's music may still be stopping: keep trying
  // for up to 1 s before giving up on the audio.
  bool ligou = false;
  for (int i = 0; i < 20 && !(ligou = tab5_musica_tocar_um(s_mp3, "Vídeo")); ++i) { vTaskDelay(pdMS_TO_TICKS(50)); }
  if (!ligou) { ESP_LOGW(TAG, "the audio did not start"); return; }
  if (ms) { tab5_musica_ir_para(ms); }
  s_v.com_som = true;
  s_v.som_visto = false;
  s_v.som_pausado = false;
  s_v.t_som = esp_timer_get_time();
}

// The music only has "toggle": check whether it is already as wanted.
// 'som_pausado' remembers what WE requested: the music state takes a block
// to reflect the request, and a quick pause/play would read the stale state.
static void som_pausar(bool pausar)
{
  if (!s_v.com_som || !som_nosso() || s_v.som_pausado == pausar) { return; }
  if (pausar && !s_mus.tocando) { return; }   // it was already stopped from outside
  tab5_musica_tocar_pausar();
  s_v.som_pausado = pausar;
}

// Seeks to 'ms': the clock, the audio and the frame for that instant.
static void ir_para(uint32_t ms, bool tocando)
{
  if (ms >= s_v.dur_ms) { ms = s_v.dur_ms ? s_v.dur_ms - 1 : 0; }
  s_v.pos_ms = ms;
  s_v.base_us = esp_timer_get_time() - (int64_t)ms * 1000;
  s_v.pulo_livre = true;
  if (s_v.com_som && som_nosso()) {
    tab5_musica_ir_para(ms);
    // The position the music reports is still the one from BEFORE the jump: kept as
    // "already seen", the clock does not realign to it (see relogio_ms) and runs on
    // from the new point until the music reports the post-jump position.
    s_v.som_ultimo_ms = s_mus.pos_ms;
    if (tocando) { som_pausar(false); }
  } else if (s_mp3[0] && tocando) {
    ligar_som(ms);   // the audio had ended: play again from there
  }
}

static void laco(void)
{
  tab5_video_estado_t e = { };
  e.aberto = true; e.com_som = false;
  e.quadros = s_v.tab.n; e.dur_ms = s_v.dur_ms; e.l = s_ql; e.a = s_qa;

  bool tocando = true, fim = false;
  s_v.t_fps = esp_timer_get_time();
  s_v.base_us = s_v.t_fps;
  if (tem_mp3()) { ligar_som(0); } else { s_mp3[0] = '\0'; }   // the audio only counts if the .mp3 exists
  uint32_t pub_pos = UINT32_MAX;

  for (;;) {
    if (s_ped_fechar) { break; }
    bool mudou = false;

    // ---- requests ----
    if (s_ped_alternar.exchange(false)) {
      if (fim) {                                   // finished: play again from the start
        fim = false; tocando = true;
        ir_para(0, true);
      } else if (tocando) {
        tocando = false;
        relogio_ms();                              // keep the current position
        som_pausar(true);
      } else {
        tocando = true;
        s_v.base_us = esp_timer_get_time() - (int64_t)s_v.pos_ms * 1000;
        if (s_v.com_som && !som_nosso()) { ligar_som(s_v.pos_ms); }
        som_pausar(false);
      }
      mudou = true;
    }
    const int64_t ir = s_ped_ir.exchange(-1);
    if (ir >= 0) {
      if (fim) { fim = false; tocando = true; }
      ir_para((uint32_t)ir, tocando);
      s_v.ultimo = -1;                             // show this instant's frame, even when paused
      mudou = true;
    }

    // ---- the frame for this instant ----
    const uint32_t pos = tocando ? relogio_ms() : s_v.pos_ms;
    uint64_t alvo = (uint64_t)pos * 1000u / s_v.us_quadro;
    uint32_t espera = 0;
    bool atrasado = false;                         // showed a frame before this instant's one
    if (!fim && alvo >= s_v.tab.n) {                // past the last frame
      fim = true; tocando = false;
      ESP_LOGI(TAG, "end: %u shown, %u skipped (%u in the 1st second); worst frame: read %u ms, decode %u ms (average %u ms); %u slow frames",
               (unsigned)s_v.mostrados, (unsigned)s_v.perdidos, (unsigned)s_v.perdidos_1s,
               (unsigned)(s_v.pior_ler_us / 1000), (unsigned)(s_v.pior_dec_us / 1000),
               (unsigned)(s_v.n_dec ? s_v.soma_dec_us / s_v.n_dec / 1000 : 0), (unsigned)s_v.lentos);
      s_v.pos_ms = s_v.dur_ms;
      if (s_v.com_som && som_nosso()) { tab5_musica_parar(); }
      s_v.com_som = false;
      mudou = true;
    } else if (!fim && (tocando || s_v.ultimo < 0) && (int64_t)alvo != s_v.ultimo) {
      // Only one frame late and the missing one is already ready: show it now
      // (an instant late) instead of skipping, and this instant's one right after.
      if (tocando && s_v.adiantado >= 0 && (int64_t)alvo == s_v.adiantado + 1) {
        mostrar((uint32_t)s_v.adiantado);
        atrasado = true;
      } else {
        mostrar((uint32_t)alvo);
      }
      mudou = true;
    }
    if (s_v.falhas >= VIDEO_FALHAS_MAX) {
      tocando = false;
      e.erro = true;
      snprintf(e.erro_txt, sizeof(e.erro_txt), N_("não consegui decodificar os quadros"));
      s_v.falhas = 0;
      if (s_v.com_som && som_nosso()) { som_pausar(true); }
      mudou = true;
    }

    // ---- publish: on each new frame or change, and the position every 250 ms ----
    if (mudou || (tocando && pos / 250 != pub_pos / 250)) {
      e.tocando = tocando; e.fim = fim; e.com_som = s_v.com_som;
      e.pos_ms = s_v.pos_ms; e.mostrados = s_v.mostrados; e.perdidos = s_v.perdidos; e.fps = s_v.fps;
      pub_pos = pos;
      publicar_estado(e);
    }

    // ---- wait until the next frame (or until someone asks for something) ----
    if (tocando && !e.erro) {
      const int64_t falta = (int64_t)(alvo + 1) * s_v.us_quadro / 1000 - (int64_t)s_v.pos_ms;
      espera = (falta < 1 || atrasado) ? 1 : (falta > 30 ? 30 : (uint32_t)falta);
      esperar(espera);
    } else {
      esperar(1000);   // stopped: only wakes on a request (and checks for close now and then)
    }
  }
  if (s_v.com_som && som_nosso()) { tab5_musica_parar(); }   // closing the video silences the audio
}

// ---------------------------------------------------------------------------
// THE MP4 (H.264) - phone and internet video
// ---------------------------------------------------------------------------
// The MP4 reader (components/medf, via tab5_leitor) delivers the video packets
// in decode order; the decoder (services/tab5_h264) returns the frames in
// display order, each with its pts. The AUDIO comes from the file itself: the
// music player plays its AAC track (the same file, opened by it) and the clock
// is the same as for AVI + .mp3.
//
// THE CPU CANNOT KEEP UP WITH EVERYTHING (360p ~12 fps, 720p ~2 fps): the lateness
// policy decides what not to decode, so the video never falls behind the audio:
//   - more than VIDEO_ATRASO_PULA_MS late: frames nobody uses as a
//     reference (B frames, almost always) are not even decoded;
//   - more than VIDEO_ATRASO_SALTA_MS late: the following frames would not
//     be shown anyway, so it skips to the next keyframe (the rest of the
//     sequence depends on what was skipped) and restarts from it, on time.
//     A heavy video shows the start of each sequence (with a keyframe
//     every 1 s, like a phone's, it is choppy but smooth in bursts); too heavy,
//     it becomes a slideshow - but the audio is always right;
//   - a frame that comes out more than VIDEO_ATRASO_MOSTRA_MS late is not
//     shown (except the keyframe in catch-up mode: it is the only picture
//     there is). Up to that, show it: a slightly late picture beats none.
#define VIDEO_MP4_L_MAX        1920        // largest accepted H.264 frame (the decoder adapts)
#define VIDEO_MP4_A_MAX        1088
#define VIDEO_MP4_ARENA        (1024 * 1024)   // file reading: 768 KB read-ahead + a packet of up to 256 KB
#define VIDEO_ATRASO_PULA_MS   80
#define VIDEO_ATRASO_MOSTRA_MS 200
#define VIDEO_ATRASO_SALTA_MS  250   // just above MOSTRA: decoding what will not be shown is just waste
// Task priority for MP4: the same as the benchmark (tab5_h264). With 3 (the
// AVI one) decoding lost the core to the reading and network tasks.
#define VIDEO_MP4_PRIO         4
#define VIDEO_MP4_PILHA        8192        // task stack for MP4 (internal: the decoder uses the PIE)

static bool e_mp4(const char* caminho)
{
  const char* ponto = strrchr(caminho, '.');
  return ponto && (!strcasecmp(ponto, ".mp4") || !strcasecmp(ponto, ".m4v") || !strcasecmp(ponto, ".mov"));
}

// The PPA for the YUV -> RGB565 conversion (a separate client: the one in
// tab5_video_pintar belongs to the UI task). Blocking mode: the video task waits
// for the PPA, which takes a few ms, and only then carries on.
static ppa_client_handle_t s_ppa_conv = nullptr;

// THE CLOCK SPEED: H.264 is decoded by the CPU. In ESP-IDF the CPU only runs at
// full speed with an ESP_PM_CPU_FREQ_MAX lock; without it, it sits at the
// profile's floor and decoding took 4x longer (279 ms per 360p frame instead of 71).
// Playing: hold it. Paused or at the end: release it (battery).
static esp_pm_lock_handle_t s_pm_trava = nullptr;
static bool                 s_pm_segura = false;

static void clock_maximo(bool sim)
{
  if (!s_pm_trava && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "video", &s_pm_trava) != ESP_OK) { s_pm_trava = nullptr; return; }
  if (sim == s_pm_segura) { return; }
  if (sim) { esp_pm_lock_acquire(s_pm_trava); } else { esp_pm_lock_release(s_pm_trava); }
  s_pm_segura = sim;
}

static void mp4_ppa_soltar(void)
{
  if (s_ppa_conv) { ppa_unregister_client(s_ppa_conv); s_ppa_conv = nullptr; }
}

// The avcC (the H.264 header in the MP4) stores the SPS and PPS like this:
// [version][profile][compat][level][length size - 1][number of SPS] and each
// SPS with a 2-byte length in front; then [number of PPS] and each PPS the same.
// The decoder wants NALs: they become Annex-B (00 00 00 01 before each one) and
// go in all at once. false = broken avcC or no SPS/PPS.
static bool entregar_avcc(const uint8_t* c, size_t n)
{
  if (n < 7 || c[0] != 1) { return false; }
  uint8_t* nal = (uint8_t*)heap_caps_malloc(n * 2 + 64, MALLOC_CAP_SPIRAM);   // fits comfortably: each NAL gains 2 bytes
  if (!nal) { return false; }
  size_t k = 0, i = 5;
  int contados = 0;
  for (int parte = 0; parte < 2; ++parte) {   // 0 = SPS, 1 = PPS
    if (i >= n) { break; }
    const int quantos = parte == 0 ? (c[i] & 0x1F) : c[i];
    ++i;
    for (int j = 0; j < quantos && i + 2 <= n; ++j) {
      const size_t t = ((size_t)c[i] << 8) | c[i + 1];
      i += 2;
      if (i + t > n) { break; }
      nal[k++] = 0; nal[k++] = 0; nal[k++] = 0; nal[k++] = 1;
      memcpy(nal + k, c + i, t);
      k += t;
      i += t;
      ++contados;
    }
  }
  if (contados >= 2) { tab5_h264_decodificar(s_v.h264, nal, k, 0, 0); }
  heap_caps_free(nal);
  return contados >= 2;
}

// Opens the MP4, the decoder and the buffers. false = error already published.
static bool preparar_mp4(void)
{
  tab5_midia_preparar();
  if (!tab5_midia_arena(&s_v.arena, VIDEO_MP4_ARENA)) { falhar(N_("sem memória")); return false; }
  const char* erro = nullptr;
  s_v.mp4 = medf_mp4_demux_open(s_caminho, &s_v.arena, false, &erro);
  if (!s_v.mp4) {
    ESP_LOGW(TAG, "mp4: %s", erro ? erro : "?");
    falhar(N_("não é um MP4 que eu leia"));
    return false;
  }
  const medf_mp4_info_t* in = medf_mp4_demux_info(s_v.mp4);
  if (in->video.codec != MEDF_MP4_VIDEO_CODEC_H264) {
    falhar(in->video.codec == MEDF_MP4_VIDEO_CODEC_NONE ? N_("MP4 sem vídeo") : N_("só vídeo H.264"));
    return false;
  }
  const int l = (int)in->video.width, a = (int)in->video.height;
  if (l < 16 || a < 16 || l > VIDEO_MP4_L_MAX || a > VIDEO_MP4_L_MAX || (l > VIDEO_MP4_A_MAX && a > VIDEO_MP4_A_MAX)) {
    falhar(N_("quadro grande demais"));
    return false;
  }
  // avcC: the 5th byte gives the size of the length field in front of each NAL.
  const uint8_t* avcc = in->video.codec_private;
  if (!avcc || in->video.codec_private_size < 7 || avcc[0] != 1) { falhar(N_("MP4 sem avcC")); return false; }
  s_v.nal_tam = (uint8_t)((avcc[4] & 3) + 1);

  // presos = 1: the frame is converted to RGB and released at once (at 1080p each
  // extra held frame is 3 MB of PSRAM).
  // The limit is that of the CODED size (a multiple of 16): a width of 854 becomes 864.
  const int lado = ((l > a ? l : a) + 15) & ~15;
  const tab5_h264_cfg_t cfg = { lado, lado, TAB5_H264_AUTO, 1 };
  s_v.h264 = tab5_h264_abrir(&cfg);
  if (!s_v.h264) { falhar(N_("sem memória para o decodificador")); return false; }
  // The SPS and PPS come in the avcC: they go in before the first packet.
  if (!entregar_avcc(avcc, in->video.codec_private_size)) { falhar(N_("MP4 sem avcC")); return false; }

  // The ready frames: RGB565 in PSRAM, aligned for the PPA, never larger
  // than the screen (1280 x 720, landscape or portrait): the PPA shrinks during
  // conversion, in steps of 1/16. A full-size 1080p would be 4 MB per buffer.
  int e16 = 16;
  const int maior = l > a ? l : a, menor = l > a ? a : l;
  while (e16 > 1 && (maior * e16 / 16 > 1280 || menor * e16 / 16 > 720)) { --e16; }
  s_v.escala16 = e16;
  const int ol = l * e16 / 16, oa = a * e16 / 16;
  const int passo = (ol + 15) & ~15, alt = (oa + 15) & ~15;
  const size_t bytes = ((size_t)passo * alt * 2 + 127) & ~(size_t)127;
  uint8_t* b[VIDEO_BUFS] = { };
  bool tem_todos = true;
  for (int k = 0; k < VIDEO_BUFS; ++k) {
    b[k] = (uint8_t*)heap_caps_aligned_calloc(128, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!b[k]) { tem_todos = false; }
  }
  const ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
  if (!tem_todos || (!s_ppa_conv && ppa_register_client(&pc, &s_ppa_conv) != ESP_OK)) {
    s_ppa_conv = nullptr;
    for (int k = 0; k < VIDEO_BUFS; ++k) { heap_caps_free(b[k]); }
    falhar(N_("sem memória"));
    return false;
  }
  s_trava.travar();
  for (int k = 0; k < VIDEO_BUFS; ++k) { s_buf[k] = b[k]; }
  s_buf_bytes = bytes;
  s_passo = passo; s_ql = ol; s_qa = oa;
  s_trava.destravar();

  s_v.us_quadro = in->video.frame_interval_us > 1000 ? (uint32_t)in->video.frame_interval_us : 33333;
  s_v.dur_ms = in->duration_us > 0 ? (uint32_t)(in->duration_us / 1000) : 0;
  s_v.tab.n = s_v.dur_ms ? (uint32_t)((uint64_t)s_v.dur_ms * 1000 / s_v.us_quadro) : 0;   // only for the state
  s_v.chave_us = -1;
  s_v.mostrar_ja = true;   // the first frame appears at once, even when paused
  // The audio: the file's own AAC track (the music player plays it); without
  // it, a .mp3 of the same name next to it, as with AVI.
  if (in->audio.codec == MEDF_MP4_AUDIO_CODEC_AAC) { snprintf(s_mp3, sizeof(s_mp3), "%s", s_caminho); }
  else if (!tem_mp3()) { s_mp3[0] = '\0'; }
  ESP_LOGI(TAG, "%s: H.264 %dx%d (screen %dx%d), %.1f fps, %u s, audio %s, decoder arena %u KB", s_caminho, l, a, ol, oa,
           1e6f / (float)s_v.us_quadro, (unsigned)(s_v.dur_ms / 1000),
           in->audio.codec == MEDF_MP4_AUDIO_CODEC_AAC ? "AAC" : s_mp3[0] ? ".mp3 alongside" : "none",
           (unsigned)(tab5_h264_arena_bytes(s_v.h264) / 1024));
  return true;
}

// Converts the decoded frame (packed YUV420) to RGB565 in a free buffer
// and publishes it. false = the PPA refused (the frame is just skipped).
static bool publicar_quadro_mp4(const tab5_h264_quadro_t& q)
{
  const int b = buffer_livre(-1);
  const int e16 = s_v.escala16;
  if (b < 0 || q.l * e16 / 16 > s_ql || q.a * e16 / 16 > s_qa) { return false; }
  ppa_srm_oper_config_t c = { };
  c.in.buffer          = q.px;
  c.in.pic_w           = (uint32_t)q.l_cod;
  c.in.pic_h           = (uint32_t)q.a_cod;
  c.in.block_w         = (uint32_t)q.l;
  c.in.block_h         = (uint32_t)q.a;
  c.in.block_offset_x  = (uint32_t)q.x;
  c.in.block_offset_y  = (uint32_t)q.y;
  c.in.srm_cm          = PPA_SRM_COLOR_MODE_YUV420;
  c.in.yuv_range       = q.faixa_cheia ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT;
  c.in.yuv_std         = q.bt709 ? PPA_COLOR_CONV_STD_RGB_YUV_BT709 : PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  c.out.buffer         = s_buf[b];
  c.out.buffer_size    = (uint32_t)s_buf_bytes;
  c.out.pic_w          = (uint32_t)s_passo;
  c.out.pic_h          = (uint32_t)((s_qa + 15) & ~15);
  c.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
  c.rotation_angle     = PPA_SRM_ROTATION_ANGLE_0;
  c.scale_x = c.scale_y = (float)e16 / 16.0f;
  c.mode               = PPA_TRANS_MODE_BLOCKING;
  if (ppa_do_scale_rotate_mirror(s_ppa_conv, &c) != ESP_OK) { ESP_LOGW(TAG, "the PPA refused the frame"); return false; }

  s_trava.travar();
  s_ultimo = b;
  s_novo = true;
  s_trava.destravar();
  ++s_v.mostrados;
  ++s_v.n_fps;
  const int64_t agora = esp_timer_get_time();
  if (agora - s_v.t_fps >= 1000000) {
    s_v.fps = (float)s_v.n_fps * 1e6f / (float)(agora - s_v.t_fps);
    s_v.t_fps = agora;
    s_v.n_fps = 0;
  }
  return true;
}

// Decodes until the next frame comes out (kept in s_v.q), applying the lateness
// policy. 'pos' = the clock now (ms). false = no more frames.
static bool proximo_quadro_mp4(uint32_t pos)
{
  medf_buffer_t* leitura = medf_mp4_demux_buffer(s_v.mp4);
  for (int voltas = 0; voltas < 600 && !s_ped_fechar; ++voltas) {
    if (tab5_h264_quadro(s_v.h264, &s_v.q)) { s_v.tem_q = true; return true; }
    if (s_v.acabou_leitura) {
      if (s_v.drenado) { return false; }
      tab5_h264_fim(s_v.h264);        // releases those in the reorder queue
      s_v.drenado = true;
      continue;
    }
    medf_mp4_packet_t p;
    const int64_t t0 = esp_timer_get_time();
    if (!medf_mp4_demux_read(s_v.mp4, &p, false)) { s_v.acabou_leitura = true; continue; }
    const uint32_t ler_us = (uint32_t)(esp_timer_get_time() - t0);
    if (ler_us > s_v.pior_ler_us) { s_v.pior_ler_us = ler_us; }
    if (p.type != MEDF_MP4_PACKET_VIDEO) { medf_buffer_release(leitura, p.ref); continue; }

    const int64_t atraso_ms = (int64_t)pos - p.pts_us / 1000;
    // Catch up: far too late, keyframes only (the rest depends on what was skipped).
    if (!s_v.mostrar_ja && atraso_ms > VIDEO_ATRASO_SALTA_MS) { s_v.alcancar = true; }
    if (s_v.alcancar && !p.keyframe) { medf_buffer_release(leitura, p.ref); ++s_v.perdidos; continue; }
    if (s_v.alcancar && p.keyframe) {
      tab5_h264_recomecar(s_v.h264);   // forgets the skipped references
      s_v.chave_us = p.pts_us;
      if (atraso_ms <= VIDEO_ATRASO_PULA_MS) { s_v.alcancar = false; }   // caught up with the audio
    } else if (!s_v.mostrar_ja && atraso_ms > VIDEO_ATRASO_PULA_MS
               && tab5_h264_descartavel(p.data, p.size, s_v.nal_tam)) {
      medf_buffer_release(leitura, p.ref);   // nothing depends on it: do not even decode
      ++s_v.perdidos;
      continue;
    }
    if (p.keyframe) { s_v.chave_us = p.pts_us; }

    const int64_t t1 = esp_timer_get_time();
    const h264_dec_result_t r = tab5_h264_decodificar(s_v.h264, p.data, p.size, s_v.nal_tam, p.pts_us);
    const uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t1);
    medf_buffer_release(leitura, p.ref);
    if (dec_us > s_v.pior_dec_us) { s_v.pior_dec_us = dec_us; }
    if (dec_us > s_v.us_quadro) { ++s_v.lentos; }
    s_v.soma_dec_us += dec_us;
    s_v.n_dec++;
    if (r == H264_DEC_NO_MEMORY) {
      // The reference frames do not fit in PSRAM (1080p with many
      // references and B frames): no point insisting.
      s_v.sem_memoria = true;
      return false;
    }
    if (r != H264_DEC_OK && r != H264_DEC_NO_PICTURE) {
      if (s_v.falhas++ < 3) { ESP_LOGW(TAG, "packet at %lld ms: %s", (long long)(p.pts_us / 1000), tab5_h264_erro(s_v.h264)); }
    } else {
      s_v.falhas = 0;
    }
  }
  return false;   // only on close (or a file of nothing but bad packets: try again next round)
}

// Seeks to 'ms': the clock and the audio (ir_para) and the reader, at the keyframe
// before that point. The frames between the keyframe and 'ms' are decoded and
// skipped by the lateness policy, up to this instant's one.
static void saltar_mp4(uint32_t ms, bool tocando)
{
  if (s_v.tem_q) { tab5_h264_soltar(s_v.h264, s_v.q.id); s_v.tem_q = false; }
  tab5_h264_recomecar(s_v.h264);
  int64_t caiu = 0;
  medf_mp4_demux_seek(s_v.mp4, (int64_t)ms * 1000, &caiu);
  s_v.acabou_leitura = false;
  s_v.drenado = false;
  s_v.alcancar = false;
  s_v.mostrar_ja = true;
  ir_para(ms, tocando);
  s_v.pulo_livre = true;
}

static void laco_mp4(void)
{
  tab5_video_estado_t e = { };
  e.aberto = true; e.com_som = false;
  e.quadros = s_v.tab.n; e.dur_ms = s_v.dur_ms; e.l = s_ql; e.a = s_qa;

  bool tocando = true, fim = false;
  s_v.t_fps = esp_timer_get_time();
  s_v.base_us = s_v.t_fps;
  if (s_mp3[0]) { ligar_som(0); }
  uint32_t pub_pos = UINT32_MAX;

  for (;;) {
    if (s_ped_fechar) { break; }
    bool mudou = false;

    // ---- requests ----
    if (s_ped_alternar.exchange(false)) {
      if (fim) {                                   // finished: play again from the start
        fim = false; tocando = true;
        saltar_mp4(0, true);
      } else if (tocando) {
        tocando = false;
        relogio_ms();
        som_pausar(true);
      } else {
        tocando = true;
        s_v.base_us = esp_timer_get_time() - (int64_t)s_v.pos_ms * 1000;
        if (s_v.com_som && !som_nosso()) { ligar_som(s_v.pos_ms); }
        som_pausar(false);
      }
      mudou = true;
    }
    const int64_t ir = s_ped_ir.exchange(-1);
    if (ir >= 0) {
      if (fim) { fim = false; tocando = true; }
      saltar_mp4((uint32_t)ir, tocando);
      mudou = true;
    }

    const uint32_t pos = tocando ? relogio_ms() : s_v.pos_ms;

    // ---- the next frame: decode if there is not one waiting already ----
    if (!fim && !s_v.tem_q && (tocando || s_v.mostrar_ja)) {
      if (!proximo_quadro_mp4(pos) && s_v.drenado) {
        fim = true; tocando = false;
        ESP_LOGI(TAG, "end: %u shown, %u skipped; worst packet: read %u ms, decode %u ms (average %u ms); %u slow",
                 (unsigned)s_v.mostrados, (unsigned)s_v.perdidos, (unsigned)(s_v.pior_ler_us / 1000),
                 (unsigned)(s_v.pior_dec_us / 1000),
                 (unsigned)(s_v.n_dec ? s_v.soma_dec_us / s_v.n_dec / 1000 : 0), (unsigned)s_v.lentos);
        s_v.pos_ms = s_v.dur_ms;
        if (s_v.com_som && som_nosso()) { tab5_musica_parar(); }
        s_v.com_som = false;
        mudou = true;
      }
    }

    // ---- show it when its time comes (or at once, after a jump) ----
    const uint32_t agora = tocando ? relogio_ms() : s_v.pos_ms;
    if (s_v.tem_q) {
      const int64_t pts_ms = s_v.q.marca / 1000;
      if (s_v.mostrar_ja || pts_ms <= (int64_t)agora) {
        const bool chave = s_v.q.marca == s_v.chave_us;
        if (!s_v.mostrar_ja && !chave && pts_ms + VIDEO_ATRASO_MOSTRA_MS < (int64_t)agora) {
          ++s_v.perdidos;                          // came out too late: not shown
        } else {
          publicar_quadro_mp4(s_v.q);
          s_v.mostrar_ja = false;
          mudou = true;
        }
        tab5_h264_soltar(s_v.h264, s_v.q.id);
        s_v.tem_q = false;
      }
    }
    if (s_v.falhas >= VIDEO_FALHAS_MAX || s_v.sem_memoria) {
      tocando = false;
      e.erro = true;
      snprintf(e.erro_txt, sizeof(e.erro_txt), "%s",
               s_v.sem_memoria ? N_("vídeo pesado demais para a memória (converta para 720p)")
                               : N_("não consegui decodificar os quadros"));
      s_v.falhas = 0;
      if (s_v.sem_memoria) { fim = true; }   // do not retry
      if (s_v.com_som && som_nosso()) { tab5_musica_parar(); s_v.com_som = false; }
      mudou = true;
    }

    clock_maximo(tocando || s_v.mostrar_ja);   // decoding: CPU at full speed

    // ---- publish: on each new frame or change, and the position every 250 ms ----
    if (mudou || (tocando && agora / 250 != pub_pos / 250)) {
      e.tocando = tocando; e.fim = fim; e.com_som = s_v.com_som;
      e.pos_ms = s_v.pos_ms; e.mostrados = s_v.mostrados; e.perdidos = s_v.perdidos; e.fps = s_v.fps;
      pub_pos = agora;
      publicar_estado(e);
    }

    // ---- wait for the time of the ready frame (or a request) ----
    if (tocando && !e.erro) {
      int64_t falta = s_v.tem_q ? s_v.q.marca / 1000 - (int64_t)agora : 0;
      esperar(falta < 1 ? 1 : (falta > 30 ? 30 : (uint32_t)falta));
    } else {
      esperar(s_v.mostrar_ja ? 1 : 1000);
    }
  }
  if (s_v.com_som && som_nosso()) { tab5_musica_parar(); }
  clock_maximo(false);
}

static void tarefa_video(void*)
{
  s_v.e_mp4 = e_mp4(s_caminho);
  if (s_v.e_mp4 ? preparar_mp4() : preparar()) { s_v.e_mp4 ? laco_mp4() : laco(); }
  else { while (!s_ped_fechar) { esperar(1000); } }
  liberar_sessao();
  mp4_ppa_soltar();
  xSemaphoreGive(s_saiu);
  // The closer deletes the task. For MP4 it was created by tab5_h264_criar_tarefa
  // (internal stack): delete it the usual way (vTaskDeleteWithCaps).
  for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }
}

// ---------------------------------------------------------------------------
// PPA - scales the frame onto the surface
// ---------------------------------------------------------------------------
static Trava              s_ppa_trava;
static ppa_client_handle_t s_ppa = nullptr;
static SemaphoreHandle_t   s_ppa_fim = nullptr;
// The rectangle painted last: when it changes, the background is redone (black).
static struct { void* dst; int x, y, l, a, ql, qa; } s_geom;

// The same care as hal/tab5_camera: NON-blocking mode, with our own semaphore
// given by the PPA ISR - so an aborted wait does not let the task leave
// with the hardware still reading the buffer.
static bool IRAM_ATTR ao_fim_ppa(ppa_client_handle_t, ppa_event_data_t*, void* sem)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &acordou);
  return acordou == pdTRUE;
}

static bool ppa_pronto(void)
{
  if (s_ppa) { return true; }
  if (!s_ppa_fim) { s_ppa_fim = xSemaphoreCreateBinary(); }
  const ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
  if (!s_ppa_fim || ppa_register_client(&cfg, &s_ppa) != ESP_OK) { s_ppa = nullptr; return false; }
  const ppa_event_callbacks_t cbs = { .on_trans_done = ao_fim_ppa };
  if (ppa_client_register_event_callbacks(s_ppa, &cbs) != ESP_OK) {
    ppa_unregister_client(s_ppa);
    s_ppa = nullptr;
    return false;
  }
  return true;
}

static void ppa_soltar(void)
{
  Trava::Guarda g(s_ppa_trava);
  if (s_ppa) {
    if (ppa_unregister_client(s_ppa) != ESP_OK) { ESP_LOGW(TAG, "the PPA was still busy"); }
    s_ppa = nullptr;
  }
  memset(&s_geom, 0, sizeof(s_geom));
}

bool tab5_video_pintar(void* destino, size_t destino_bytes, int pic_l, int pic_a,
                       int x, int y, int l, int a, bool so_se_novo)
{
  if (!destino || l <= 0 || a <= 0 || x < 0 || y < 0 || x + l > pic_l || y + a > pic_a) { return false; }

  // The frame: the new one, or (so_se_novo = false) the last one out. The app holds
  // it only for the duration of this function.
  const uint16_t* px; int ql, qa, passo;
  if (so_se_novo) {
    if (!tab5_video_pegar_quadro(&px, &ql, &qa, &passo)) { return false; }
  } else {
    s_trava.travar();
    const bool tem = s_preso < 0 && s_ultimo >= 0 && s_buf[s_ultimo];
    if (tem) { s_preso = s_ultimo; s_novo = false; px = (const uint16_t*)s_buf[s_ultimo]; ql = s_ql; qa = s_qa; passo = s_passo; }
    s_trava.destravar();
    if (!tem) { return false; }
  }

  bool ok = false;
  {
    Trava::Guarda g(s_ppa_trava);
    if (ppa_pronto()) {
      // Uniform scale that fits, in steps of 1/16 (the PPA's precision).
      int s16 = (l * 16) / ql;
      const int s16a = (a * 16) / qa;
      if (s16a < s16) { s16 = s16a; }
      if (s16 < 1) { s16 = 1; }
      const int sl = ql * s16 / 16, sa = qa * s16 / 16;
      const int ox = x + (l - sl) / 2, oy = y + (a - sa) / 2;

      // THE RECTANGLE OR THE VIDEO CHANGED: black background again. The lines go to
      // memory BEFORE the PPA, which invalidates the destination cache.
      if (s_geom.dst != destino || s_geom.x != x || s_geom.y != y || s_geom.l != l || s_geom.a != a
       || s_geom.ql != ql || s_geom.qa != qa) {
        uint8_t* base = (uint8_t*)destino;
        for (int j = 0; j < a; ++j) { memset(base + ((size_t)(y + j) * pic_l + x) * 2, 0, (size_t)l * 2); }
        esp_cache_msync(base + (size_t)y * pic_l * 2, (size_t)a * pic_l * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        s_geom = { destino, x, y, l, a, ql, qa };
      }

      ppa_srm_oper_config_t c = { };
      c.in.buffer          = (void*)px;
      c.in.pic_w           = (uint32_t)passo;
      c.in.pic_h           = (uint32_t)((qa + 15) & ~15);
      c.in.block_w         = (uint32_t)ql;
      c.in.block_h         = (uint32_t)qa;
      c.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
      c.out.buffer         = destino;
      c.out.buffer_size    = (uint32_t)destino_bytes;
      c.out.pic_w          = (uint32_t)pic_l;
      c.out.pic_h          = (uint32_t)pic_a;
      c.out.block_offset_x = (uint32_t)ox;
      c.out.block_offset_y = (uint32_t)oy;
      c.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
      c.rotation_angle     = PPA_SRM_ROTATION_ANGLE_0;
      c.scale_x = c.scale_y = (float)s16 / 16.0f;
      c.mode               = PPA_TRANS_MODE_NON_BLOCKING;
      c.user_data          = s_ppa_fim;
      if (ppa_do_scale_rotate_mirror(s_ppa, &c) == ESP_OK) {
        // One operation takes a few ms; 1 s only runs out if the PPA has hung.
        for (int i = 0; i < 20 && !ok; ++i) { ok = xSemaphoreTake(s_ppa_fim, pdMS_TO_TICKS(50)) == pdTRUE; }
        if (!ok) { ESP_LOGW(TAG, "the PPA did not finish"); }
      } else {
        ESP_LOGW(TAG, "the PPA refused the frame");
      }
    }
  }
  tab5_video_soltar_quadro();
  return ok;
}

// ---------------------------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------------------------
bool tab5_video_suportado(const char* nome)
{
  const char* ponto = nome ? strrchr(nome, '.') : nullptr;
  return ponto && ponto != nome && (!strcasecmp(ponto, ".avi") || e_mp4(nome));
}

bool tab5_video_abrir(const char* caminho)
{
  if (!caminho || !tab5_video_suportado(caminho) || strlen(caminho) >= sizeof(s_caminho)) { return false; }
  tab5_video_fechar();   // one video at a time
  Trava::Guarda g(s_ctrl);
  if (!s_saiu) { s_saiu = xSemaphoreCreateBinary(); }
  if (!s_saiu) { return false; }
  xSemaphoreTake(s_saiu, 0);   // clears an old signal

  snprintf(s_caminho, sizeof(s_caminho), "%s", caminho);
  s_mp3[0] = '\0';
  s_ped_fechar = false; s_ped_alternar = false; s_ped_ir = -1;
  memset(&s_v, 0, sizeof(s_v));

  s_trava.travar();
  memset(&s_est, 0, sizeof(s_est));
  s_est.aberto = true;
  s_est.tocando = true;
  const char* nome = strrchr(caminho, '/');
  snprintf(s_est.nome, sizeof(s_est.nome), "%s", nome ? nome + 1 : caminho);
  char* ponto = strrchr(s_est.nome, '.');
  if (ponto) { *ponto = '\0' ; }
  s_trava.destravar();

  // MP4 decodes with the PIE (internal stack, see tab5_h264); AVI uses the
  // hardware JPEG and fits in PSRAM.
  const BaseType_t criou = e_mp4(caminho)
      ? tab5_h264_criar_tarefa(tarefa_video, "video", VIDEO_MP4_PILHA, nullptr, VIDEO_MP4_PRIO, 0, &s_tarefa)
      : xTaskCreatePinnedToCoreWithCaps(tarefa_video, "video", VIDEO_PILHA, nullptr, VIDEO_PRIO, &s_tarefa,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
  if (criou != pdPASS) {
    s_tarefa = nullptr;
    s_trava.travar(); memset(&s_est, 0, sizeof(s_est)); s_trava.destravar();
    ESP_LOGE(TAG, "the task did not start");
    return false;
  }
  sistema_avisar(AVISO_VIDEO);
  return true;
}

void tab5_video_fechar(void)
{
  Trava::Guarda g(s_ctrl);
  if (s_tarefa) {
    s_ped_fechar = true;
    xTaskNotifyGive(s_tarefa);
    // The task releases everything and signals; wait out a badly decoded frame, a long read...
    for (int i = 0; i < 100 && xSemaphoreTake(s_saiu, pdMS_TO_TICKS(50)) != pdTRUE; ++i) {}
    // Already blocked waiting to be deleted? (A task running on the other
    // core cannot be deleted.)
    for (int i = 0; i < 100 && eTaskGetState(s_tarefa) != eBlocked; ++i) { vTaskDelay(1); }
    vTaskDeleteWithCaps(s_tarefa);
    s_tarefa = nullptr;
  }
  ppa_soltar();
  s_trava.travar();
  const bool tinha = s_est.aberto;
  memset(&s_est, 0, sizeof(s_est));
  s_trava.destravar();
  if (tinha) { sistema_avisar(AVISO_VIDEO); }
}

void tab5_video_pausar_tocar(void)
{
  s_ped_alternar = true;
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}

void tab5_video_ir_ms(uint32_t ms)
{
  s_ped_ir = (int64_t)ms;
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}

void tab5_video_estado(tab5_video_estado_t* e)
{
  s_trava.travar();
  *e = s_est;
  s_trava.destravar();
}

bool tab5_video_pegar_quadro(const uint16_t** px, int* l, int* a, int* passo)
{
  Trava::Guarda g(s_trava);
  if (!s_novo || s_preso >= 0 || s_ultimo < 0 || !s_buf[s_ultimo]) { return false; }
  s_preso = s_ultimo;
  s_novo = false;
  *px = (const uint16_t*)s_buf[s_ultimo];
  *l = s_ql; *a = s_qa; *passo = s_passo;
  return true;
}

void tab5_video_soltar_quadro(void)
{
  Trava::Guarda g(s_trava);
  s_preso = -1;
}
