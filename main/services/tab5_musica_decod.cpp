// MUSIC PLAYER - the open file: MP3/WAV/MP4, the ID3 tag, reading
// the card, seeking to a point and decoding a block of PCM.
// See tab5_musica_interno.hpp.
#include "tab5_musica_interno.hpp"
#include "idioma.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "minimp3_externo.h"

// MP4/M4A: the container reader (components/medf) and AAC (components/audf).
#include "medf_mp4.h"
#include "audf_aac.h"
#include "tab5_midia.hpp"

static const char* TAG = "musica";

// Just the file name from a path.
static const char* base_de(const char* caminho)
{
  const char* b = strrchr(caminho, '/');
  return b ? b + 1 : caminho;
}

// The buffers (allocated in ao_iniciar in tab5_musica.cpp) and the MP3 decoder.
uint8_t*  g_musica_arq_buf = nullptr;
uint8_t*  g_musica_ler_buf = nullptr;   // aligned, DMA-capable (PSRAM)
mp3dec_t* g_musica_mp3 = nullptr;

// ---------------------------------------------------------------------------
// THE OPEN FILE
// ---------------------------------------------------------------------------
// MP4/M4A (the sound of a phone video, or .m4a music): the MP4 reader has
// its own card reading (via tab5_leitor, with read-ahead) and
// delivers only the AAC packets; the audf decoder turns them into PCM.
// The 512 KB arena: 384 KB of read-ahead + 128 KB for the largest packet.
static medf_arena_t       s_mp4_arena = { };
static medf_mp4_demux_t*  s_mp4 = nullptr;
static audf_decoder_t*    s_aac = nullptr;

// POSIX (open/read), NOT stdio. Measured on the device, reading 256 KB from the card:
// unbuffered fread 52 KB/s (newlib reads ONE BYTE per call to FAT), buffered
// fread 572 KB/s, read() 4593 KB/s - the whole request goes to FatFs, which
// DMAs several sectors straight into the aligned buffer.
static int       s_fd = -1;

// Reads exactly n bytes (fewer only at end of file).
static size_t ler(void* dst, size_t n)
{
  size_t feito = 0;
  while (feito < n) {
    const ssize_t r = read(s_fd, (uint8_t*)dst + feito, n - feito);
    if (r <= 0) { break; }
    feito += (size_t)r;
  }
  return feito;
}
formato_t g_musica_fmt = FMT_NADA;
long      g_musica_dados_ini = 0, g_musica_dados_fim = 0;   // audio bytes [ini, fim)
static size_t    s_buf_n = 0, s_buf_pos = 0;         // what is in g_musica_arq_buf
static long      s_lido_ate = 0;                     // file position after the buffer
static size_t    s_pular = 0;                        // bytes to discard from the next read (alignment)
uint32_t  g_musica_hz = 0;
static uint8_t   s_canais = 0;
static uint8_t   s_wav_bits = 16;
uint64_t  g_musica_quadros = 0;                      // already queued (position)
uint32_t  g_musica_ult_q = 0;                        // frames in the last queued block
uint32_t  g_musica_ler_us = 0;                       // time reading the card in the current block
uint64_t  g_musica_total_quadros = 0;                // 0 = unknown
uint32_t  g_musica_mp3_kbps_soma = 0, g_musica_mp3_quadros = 0;   // average bitrate (VBR without Xing)
bool      g_musica_dur_exata = false;                // the duration came from Xing (or it is WAV)
// A decoded MP3 frame that did not fit in the block (the rate or channels changed
// midway): it opens the next block. Decoding again would spoil minimp3's
// bit reservoir.
int16_t*  g_musica_quadro = nullptr;                 // 1152 x 2 samples (PSRAM)
static int       s_pend_n = 0, s_pend_canais = 0;
static uint32_t  s_pend_hz = 0;

// Is a track open? (MP3/WAV via s_fd; MP4 via the MP4 reader.)
bool musica_arquivo_aberto(void) { return s_fd >= 0 || s_mp4 != nullptr; }

void musica_fechar_arquivo(void)
{
  if (s_fd >= 0) { close(s_fd); s_fd = -1; }
  if (s_aac) { audf_decoder_destroy(s_aac); s_aac = nullptr; }
  if (s_mp4) { medf_mp4_demux_close(s_mp4); s_mp4 = nullptr; }
  tab5_midia_arena_soltar(&s_mp4_arena);
  g_musica_fmt = FMT_NADA;
  s_buf_n = s_buf_pos = 0;
  s_pend_n = 0;
}


// Text of an ID3v2 frame (with the encoding byte in front) -> UTF-8.
static void id3_texto(const uint8_t* p, size_t n, char* out, size_t tam)
{
  out[0] = '\0';
  if (n < 2 || !tam) { return; }
  const uint8_t enc = p[0];
  ++p; --n;
  size_t k = 0;
  if (enc == 0) {   // ISO-8859-1
    for (size_t i = 0; i < n && p[i]; ++i) { if (!musica_utf8_por(out, tam, &k, p[i])) { break; } }
  } else if (enc == 3) {   // UTF-8
    for (size_t i = 0; i < n && p[i] && k + 1 < tam; ++i) { out[k++] = (char)p[i]; }
  } else {   // 1 = UTF-16 with BOM, 2 = UTF-16BE
    bool be = (enc == 2);
    size_t i = 0;
    if (enc == 1 && n >= 2) {
      if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
      else if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
    }
    for (; i + 1 < n; i += 2) {
      const uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i + 1] << 8 | p[i]);
      if (!c) { break; }
      if (c >= 0xD800 && c < 0xE000) { continue; }   // outside the basic plane: skip
      if (!musica_utf8_por(out, tam, &k, c)) { break; }
    }
  }
  out[k] = '\0';
  while (k && (out[k - 1] == ' ')) { out[--k] = '\0'; }
}

// ID3v2 at the start: reads title, artist and album and returns where the audio starts.
static long id3v2_pular(char* titulo, size_t tt, char* artista, size_t ta, char* album, size_t tal)
{
  uint8_t h[10];
  if (ler(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) { return 0; }
  const uint8_t ver = h[3];
  const uint32_t tam = syncsafe(h + 6);
  const long fim = 10 + (long)tam + ((h[5] & 0x10) ? 10 : 0);
  // Only the start of the tag: title and artist come before the cover, almost always.
  const size_t n_tag = tam < 16 * 1024 ? tam : 16 * 1024;
  if (ver < 3 || ver > 4 || (h[5] & 0x40) || ler(g_musica_arq_buf, n_tag) != n_tag) { return fim; }
  size_t i = 0;
  while (i + 10 <= n_tag) {
    const uint8_t* q = g_musica_arq_buf + i;
    if (!q[0]) { break; }   // padding
    const uint32_t n = (ver == 4) ? syncsafe(q + 4) : be32(q + 4);
    if (n == 0 || i + 10 + n > n_tag) { break; }
    if      (!memcmp(q, "TIT2", 4)) { id3_texto(q + 10, n, titulo, tt); }
    else if (!memcmp(q, "TPE1", 4)) { id3_texto(q + 10, n, artista, ta); }
    else if (!memcmp(q, "TALB", 4)) { id3_texto(q + 10, n, album, tal); }
    i += 10 + n;
  }
  return fim;
}

// Reads ONE chunk from the card onto the end of the buffer, if it fits. false = nothing to read.
static bool recarregar(void)
{
  if (s_buf_pos > 0) {
    memmove(g_musica_arq_buf, g_musica_arq_buf + s_buf_pos, s_buf_n - s_buf_pos);
    s_buf_n -= s_buf_pos;
    s_buf_pos = 0;
  }
  if (s_lido_ate >= g_musica_dados_fim || ARQ_BUF - s_buf_n < LER_PEDACO) { return false; }
  const int64_t t0 = esp_timer_get_time();
  const size_t n = ler(g_musica_ler_buf, LER_PEDACO);   // from a sector start, into an aligned buffer: direct DMA
  g_musica_ler_us += (uint32_t)(esp_timer_get_time() - t0);
  if (n == 0) { s_lido_ate = g_musica_dados_fim; return false; }
  s_lido_ate += (long)n;
  size_t uteis = n > s_pular ? n - s_pular : 0;
  if (s_lido_ate > g_musica_dados_fim) {   // went past the audio (ID3v1 tag at the end)
    const long sobra = s_lido_ate - g_musica_dados_fim;
    uteis = (long)uteis > sobra ? uteis - (size_t)sobra : 0;
    s_lido_ate = g_musica_dados_fim;
  }
  memcpy(g_musica_arq_buf + s_buf_n, g_musica_ler_buf + s_pular, uteis);
  s_buf_n += uteis;
  s_pular = 0;
  return uteis > 0;
}

// Goes to byte 'b' of the file: reads from its sector, and discards the
// start (see SD CARD READING, up above).
static void ir_para_byte(long b)
{
  if (b < g_musica_dados_ini) { b = g_musica_dados_ini; }
  if (b > g_musica_dados_fim) { b = g_musica_dados_fim; }
  const long setor = b & ~(long)(SETOR - 1);
  lseek(s_fd, setor, SEEK_SET);
  s_lido_ate = setor;
  s_pular = (size_t)(b - setor);
  s_buf_n = s_buf_pos = 0;
}

// The Xing/Info header (MP3 VBR) says how many frames the track has.
static uint32_t xing_quadros(const uint8_t* q, size_t n)
{
  if (n < 4) { return 0; }
  const bool mpeg1 = (q[1] & 0x08) != 0;
  const bool mono  = ((q[3] >> 6) & 3) == 3;
  const size_t o = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
  if (n < o + 12) { return 0; }
  if (memcmp(q + o, "Xing", 4) && memcmp(q + o, "Info", 4)) { return 0; }
  return (be32(q + o + 4) & 1) ? be32(q + o + 8) : 0;
}

static bool abrir_mp3(void)
{
  mp3dec_init(g_musica_mp3);
  // First frame: rate, channels and, if present, Xing.
  recarregar();
  mp3dec_frame_info_t info;
  for (int tentativas = 0; tentativas < 8; ++tentativas) {
    const int amostras = mp3dec_decode_frame(g_musica_mp3, g_musica_arq_buf + s_buf_pos, (int)(s_buf_n - s_buf_pos), nullptr, &info);
    if (info.frame_bytes == 0) { if (!recarregar()) { return false; } continue; }
    if (amostras > 0 || info.hz) {
      g_musica_hz = (uint32_t)info.hz;
      s_canais = (uint8_t)info.channels;
      const uint32_t xq = xing_quadros(g_musica_arq_buf + s_buf_pos + (info.frame_offset), s_buf_n - s_buf_pos - info.frame_offset);
      const uint32_t por_quadro = (info.layer == 1) ? 384 : (g_musica_hz < 32000 && info.layer == 3) ? 576 : 1152;
      g_musica_dur_exata = xq != 0;
      if (xq) {
        g_musica_total_quadros = (uint64_t)xq * por_quadro;
      } else if (info.bitrate_kbps > 0) {
        g_musica_total_quadros = (uint64_t)(g_musica_dados_fim - g_musica_dados_ini) * 8u * g_musica_hz / ((uint64_t)info.bitrate_kbps * 1000u);
      }
      g_musica_mp3_kbps_soma = 0; g_musica_mp3_quadros = 0;
      // Back to the start: the first frame is audio too (or the Xing, which
      // minimp3 decodes as silence - harmless).
      ir_para_byte(g_musica_dados_ini);
      mp3dec_init(g_musica_mp3);
      return g_musica_hz > 0 && s_canais > 0;
    }
    s_buf_pos += (size_t)info.frame_bytes;
  }
  return false;
}

static bool abrir_wav(void)
{
  uint8_t h[12];
  lseek(s_fd, 0, SEEK_SET);
  if (ler(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { return false; }
  bool tem_fmt = false;
  for (int k = 0; k < 32; ++k) {
    uint8_t c[8];
    if (ler(c, 8) != 8) { return false; }
    const uint32_t n = le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t f[16];
      if (n < 16 || ler(f, 16) != 16) { return false; }
      const uint16_t fmt = le16(f);
      s_canais   = (uint8_t)le16(f + 2);
      g_musica_hz       = le32(f + 4);
      s_wav_bits = (uint8_t)le16(f + 14);
      if ((fmt != 1 && fmt != 0xFFFE) || s_canais < 1 || s_canais > 2 || (s_wav_bits != 16 && s_wav_bits != 8)) {
        return false;
      }
      lseek(s_fd, (off_t)(n - 16 + (n & 1)), SEEK_CUR);
      tem_fmt = true;
    } else if (!memcmp(c, "data", 4)) {
      if (!tem_fmt) { return false; }
      g_musica_dados_ini = (long)lseek(s_fd, 0, SEEK_CUR);
      if ((long)n < g_musica_dados_fim - g_musica_dados_ini) { g_musica_dados_fim = g_musica_dados_ini + (long)n; }
      g_musica_total_quadros = (uint64_t)(g_musica_dados_fim - g_musica_dados_ini) / (s_canais * (s_wav_bits / 8));
      g_musica_dur_exata = true;
      ir_para_byte(g_musica_dados_ini);
      return g_musica_hz > 0;
    } else {
      lseek(s_fd, (off_t)(n + (n & 1)), SEEK_CUR);
    }
  }
  return false;
}

// Opens an MP4's sound track. AAC only (the sound of nearly every phone and internet
// video). false = no sound, another format or out of memory.
static bool abrir_mp4(const char* caminho)
{
  tab5_midia_preparar();
  if (!tab5_midia_arena(&s_mp4_arena, MP4_ARENA)) { return false; }
  const char* erro = nullptr;
  s_mp4 = medf_mp4_demux_open(caminho, &s_mp4_arena, false, &erro);
  if (!s_mp4) { ESP_LOGW(TAG, "mp4: %s", erro ? erro : "did not open"); return false; }
  medf_mp4_demux_drop_video(s_mp4);   // the video (if any) belongs to whoever plays the video
  const medf_mp4_info_t* in = medf_mp4_demux_info(s_mp4);
  if (in->audio.codec != MEDF_MP4_AUDIO_CODEC_AAC) {
    ESP_LOGW(TAG, "mp4: sound that is not AAC (code %d)", (int)in->audio.codec);
    return false;
  }
  audf_aac_config_t cfg = { };
  cfg.asc          = in->audio.codec_private;
  cfg.asc_len      = in->audio.codec_private_size;
  cfg.adts         = false;
  cfg.he           = AUDF_AAC_HE_V2;           // HE-AAC (SBR and parametric stereo) too
  cfg.alloc_caps   = MALLOC_CAP_SPIRAM;        // internal SRAM is kept for Wi-Fi
  cfg.scratch_caps = MALLOC_CAP_SPIRAM;
  if (audf_aac_decoder_create(&cfg, &s_aac) != ESP_OK) { s_aac = nullptr; return false; }
  g_musica_hz = audf_aac_decoder_rate(s_aac);
  s_canais = audf_decoder_channels(s_aac);
  if (!g_musica_hz || s_canais < 1 || s_canais > 2 || audf_decoder_max_frames(s_aac) > BLOCO_QUADROS) { return false; }
  g_musica_total_quadros = (uint64_t)(in->duration_us > 0 ? in->duration_us : 0) * g_musica_hz / 1000000u;
  g_musica_dur_exata = true;
  return true;
}


// Opens the track at position 'pos' of the ORDER. false = bad file (the loop skips
// to the next).
bool musica_abrir_item(int pos)
{
  musica_fechar_arquivo();
  char caminho[160 + TAB5_MUSICA_CAMINHO_MAX + 2];
  char rel[TAB5_MUSICA_CAMINHO_MAX];
  g_musica_trava.travar();
  const bool valido = pos >= 0 && pos < g_musica_fila_n && g_musica_ordem;
  if (valido) {
    snprintf(rel, sizeof(rel), "%s", g_musica_fila[g_musica_ordem[pos]]);
    if (absoluto(rel)) { snprintf(caminho, sizeof(caminho), "%s", rel); }
    else               { snprintf(caminho, sizeof(caminho), "%s/%s", g_musica_fila_raiz, rel); }
  }
  g_musica_trava.destravar();
  if (!valido) { return false; }
  const char* nome = base_de(rel);

  char titulo[64] = "", artista[48] = "", album[48] = "";
  g_musica_quadros = 0; g_musica_total_quadros = 0; g_musica_hz = 0; s_canais = 0;
  bool ok = false;
  if (e_mp4(nome)) {
    g_musica_fmt = FMT_MP4;
    ok = abrir_mp4(caminho);
  } else if ((s_fd = open(caminho, O_RDONLY)) >= 0) {
    g_musica_dados_fim = (long)lseek(s_fd, 0, SEEK_END);
    lseek(s_fd, 0, SEEK_SET);
    if (termina_com(nome, ".wav")) {
      g_musica_fmt = FMT_WAV;
      ok = abrir_wav();
    } else {
      g_musica_fmt = FMT_MP3;
      g_musica_dados_ini = id3v2_pular(titulo, sizeof(titulo), artista, sizeof(artista), album, sizeof(album));
      // ID3v1 at the end: 128 bytes that are not audio.
      if (g_musica_dados_fim >= 128) {
        uint8_t tag[3];
        lseek(s_fd, g_musica_dados_fim - 128, SEEK_SET);
        if (ler(tag, 3) == 3 && !memcmp(tag, "TAG", 3)) { g_musica_dados_fim -= 128; }
      }
      ir_para_byte(g_musica_dados_ini);
      ok = abrir_mp3();
    }
  }
  if (!titulo[0]) { tab5_musica_titulo_do_arquivo(nome, titulo, sizeof(titulo), artista[0] ? nullptr : artista, sizeof(artista)); }

  g_musica_trava.travar();
  g_musica_est.idx = pos;
  snprintf(g_musica_est.caminho, sizeof(g_musica_est.caminho), "%s", rel);
  snprintf(g_musica_est.arquivo, sizeof(g_musica_est.arquivo), "%s", nome);
  snprintf(g_musica_est.titulo, sizeof(g_musica_est.titulo), "%s", titulo);
  snprintf(g_musica_est.artista, sizeof(g_musica_est.artista), "%s", artista);
  snprintf(g_musica_est.album, sizeof(g_musica_est.album), "%s", album);
  g_musica_est.hz = g_musica_hz;
  g_musica_est.canais = s_canais;
  g_musica_est.kbps = 0;
  g_musica_est.pos_ms = 0;
  g_musica_est.dur_ms = g_musica_hz ? (uint32_t)(g_musica_total_quadros * 1000u / g_musica_hz) : 0;
  if (!ok) { snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), tr("não toca: %s"), titulo); }
  else     { g_musica_est.erro[0] = '\0'; }
  g_musica_trava.destravar();

  if (!ok) {
    ESP_LOGW(TAG, "did not open %s", caminho);
    musica_fechar_arquivo();
    return false;
  }
  musica_capa_pedir(rel, titulo, artista, album);
  ESP_LOGI(TAG, "%s: %s, %lu Hz, %u channel(s), %lu s", nome, g_musica_fmt == FMT_MP3 ? "MP3" : g_musica_fmt == FMT_MP4 ? "AAC" : "WAV",
           (unsigned long)g_musica_hz, (unsigned)s_canais, (unsigned long)(g_musica_est.dur_ms / 1000));
  return true;
}

void musica_ir_para_ms(uint32_t ms)
{
  if (!g_musica_hz) { return; }
  uint64_t q = (uint64_t)ms * g_musica_hz / 1000u;
  if (g_musica_fmt == FMT_MP4) {
    // MP4 has a table of every packet: the seek lands on the right packet.
    int64_t caiu_us = 0;
    if (!s_mp4 || !medf_mp4_demux_seek(s_mp4, (int64_t)ms * 1000, &caiu_us)) { return; }
    audf_decoder_reset(s_aac);
    q = (uint64_t)caiu_us * g_musica_hz / 1000000u;
  } else if (s_fd < 0) {
    return;
  } else if (g_musica_fmt == FMT_WAV) {
    const uint32_t por = s_canais * (s_wav_bits / 8);
    ir_para_byte(g_musica_dados_ini + (long)(q * por));
  } else {
    // MP3 without an index: proportional to the size. minimp3 resyncs on the
    // next frame. (VBR without a table is a little off - that is the best without reading everything.)
    if (!g_musica_total_quadros) { return; }
    const long b = g_musica_dados_ini + (long)((uint64_t)(g_musica_dados_fim - g_musica_dados_ini) * q / g_musica_total_quadros);
    ir_para_byte(b);
    mp3dec_init(g_musica_mp3);
  }
  s_pend_n = 0;
  g_musica_quadros = q;
}

// Decodes up to one block into 'saida'. Returns the frames (0 = the track
// ended). 'hz'/'estereo' are the block's: a block never mixes rates.
size_t musica_decodificar_bloco(int16_t* saida, uint32_t* hz, bool* estereo)
{
  size_t q = 0;
  if (g_musica_fmt == FMT_MP4) {
    // Packet by packet (1024 or 2048 frames each), straight into the block, while
    // the largest possible packet fits. AAC has a fixed rate and channels.
    const size_t maior = audf_decoder_max_frames(s_aac);
    medf_buffer_t* leitura = medf_mp4_demux_buffer(s_mp4);
    while (q + maior <= BLOCO_QUADROS) {
      medf_mp4_packet_t p;
      const int64_t t0 = esp_timer_get_time();
      const bool tem = medf_mp4_demux_read(s_mp4, &p, true);
      g_musica_ler_us += (uint32_t)(esp_timer_get_time() - t0);
      if (!tem) {                                              // end of the track
        if (q == 0) { ESP_LOGI(TAG, "mp4: end of the sound packets"); }
        break;
      }
      if (p.type != MEDF_MP4_PACKET_AUDIO) { medf_buffer_release(leitura, p.ref); continue; }
      size_t n = 0;
      const esp_err_t err = audf_decoder_decode(s_aac, p.data, p.size, saida + q * s_canais, &n);
      medf_buffer_release(leitura, p.ref);
      if (err != ESP_OK) {                                     // bad packet: short silence, carry on
        static uint32_t avisos = 0;
        if (avisos++ < 5) { ESP_LOGW(TAG, "aac: packet of %u bytes at %lld ms: %s", (unsigned)p.size, (long long)(p.pts_us / 1000), esp_err_to_name(err)); }
        continue;
      }
      q += n;
    }
    *hz = g_musica_hz;
    *estereo = s_canais == 2;
    return q;
  }
  if (g_musica_fmt == FMT_WAV) {
    // Through the same buffer as MP3 (the fast read is its): a stereo WAV block
    // is 72 KB, so it fills until it fits or the file ends.
    const uint32_t por = s_canais * (s_wav_bits / 8);
    while (s_buf_n - s_buf_pos < (size_t)BLOCO_QUADROS * por && recarregar()) { }
    q = (s_buf_n - s_buf_pos) / por;
    if (q > BLOCO_QUADROS) { q = BLOCO_QUADROS; }
    if (q == 0) { return 0; }
    const uint8_t* src = g_musica_arq_buf + s_buf_pos;
    if (s_wav_bits == 16) {
      memcpy(saida, src, q * por);
    } else {   // 8-bit unsigned -> 16-bit signed
      for (size_t i = 0; i < q * s_canais; ++i) { saida[i] = (int16_t)(((int)src[i] - 128) << 8); }
    }
    s_buf_pos += q * por;
    *hz = g_musica_hz;
    *estereo = s_canais == 2;
    return q;
  }

  // MP3: frame by frame, while a whole frame fits in the block.
  int canais_bloco = 0;
  uint32_t hz_bloco = 0;
  auto aceitar = [&](int n, int ch, uint32_t hz) -> bool {
    if (!canais_bloco) { canais_bloco = ch; hz_bloco = hz; }
    else if (ch != canais_bloco || hz != hz_bloco) { return false; }
    memcpy(saida + q * ch, g_musica_quadro, (size_t)n * ch * sizeof(int16_t));
    q += (size_t)n;
    return true;
  };
  if (s_pend_n) { aceitar(s_pend_n, s_pend_canais, s_pend_hz); s_pend_n = 0; }
  while (q + 1152 <= BLOCO_QUADROS) {
    if (s_buf_n - s_buf_pos < 16 * 1024) { recarregar(); }
    if (s_buf_n <= s_buf_pos) { break; }   // end of file
    mp3dec_frame_info_t info;
    const int n = mp3dec_decode_frame(g_musica_mp3, g_musica_arq_buf + s_buf_pos, (int)(s_buf_n - s_buf_pos), g_musica_quadro, &info);
    if (info.frame_bytes == 0) {
      // No frame in what is left of the buffer: discard and reload.
      s_buf_pos = s_buf_n;
      if (s_lido_ate >= g_musica_dados_fim) { break; }
      continue;
    }
    s_buf_pos += (size_t)info.frame_bytes;
    if (n <= 0) { continue; }
    g_musica_mp3_kbps_soma += (uint32_t)info.bitrate_kbps;
    ++g_musica_mp3_quadros;
    g_musica_hz = (uint32_t)info.hz;
    if (!aceitar(n, info.channels, (uint32_t)info.hz)) {
      s_pend_n = n; s_pend_canais = info.channels; s_pend_hz = (uint32_t)info.hz;
      break;
    }
  }
  *hz = hz_bloco ? hz_bloco : g_musica_hz;
  *estereo = canais_bloco != 1;
  return q;
}

