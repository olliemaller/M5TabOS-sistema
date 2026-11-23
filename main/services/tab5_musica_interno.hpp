// What the music player's files share: the state used by more than one
// of them (declared here, defined in the file indicated) and the internal
// functions that cross files. Only for them: users of the service include
// tab5_musica.hpp.
//
// The service was split by subject (files only moved, no logic changed):
//   tab5_musica.cpp          state, controls, queue, playback loop, background part
//   tab5_musica_nomes.cpp    names: UTF-8, old CP437, title from the file name
//   tab5_musica_decod.cpp    open file (MP3/WAV/MP4), ID3, position and decoding
//   tab5_musica_espectro.cpp FFT and the spectrum frame ring
//   tab5_musica_pasta.cpp    music folder, listing, .m3u playlists, play folder/playlist
//   tab5_musica_duracao.cpp  duration without decoding
//   tab5_musica_capa.cpp     cover: ID3, folder, cache, own task
//   tab5_musica_rede.cpp     cover from the internet (iTunes search)
//
// All the .cpp files above are in linker.lf (m5tabos_extram_bss): when creating
// a new player file, add its line there.
#pragma once

#include "tab5_musica.hpp"
#include "tab5_mutex.hpp"
#include "tab5_hash.hpp"
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// MEMORY AND SD CARD READING (see the comments in tab5_musica.cpp)
// ---------------------------------------------------------------------------
#define BLOCO_QUADROS  (1152 * 16)
#define LER_PEDACO     (32 * 1024)    // one read from the card
#define ARQ_BUF        (128 * 1024)   // what the decoder has ahead of it
#define SETOR          512
#define MP4_ARENA      (512 * 1024)   // MP4 reader arena (playing and measuring)
#define LATENCIA_US    60000          // from queueing to the speaker: I2S DMA + mixer
#define CAPA_MAX_BYTES (2 * 1024 * 1024)   // a cover bigger than this is not a cover

typedef enum { FMT_NADA = 0, FMT_MP3, FMT_WAV, FMT_MP4 } formato_t;

// The queue: the paths (relative to the music folder), copied from the requester.
typedef char caminho_t[TAB5_MUSICA_CAMINHO_MAX];

// The cover request (the track playing) and the result of an internet search.
typedef struct {
  char caminho[TAB5_MUSICA_CAMINHO_MAX];
  char titulo[64], artista[48], album[48];
} capa_ped_t;
typedef enum { NET_ACHOU, NET_NAO_TEM, NET_FALHOU } net_t;

// ---------------------------------------------------------------------------
// State used by more than one file
// ---------------------------------------------------------------------------
// tab5_musica.cpp: queue + state (all under the lock)
extern Trava                  g_musica_trava;
extern tab5_musica_estado_t   g_musica_est;
extern caminho_t*             g_musica_fila;
extern int16_t*               g_musica_ordem;   // the play order (indices into g_musica_fila)
extern int                    g_musica_fila_n;
extern char                   g_musica_fila_raiz[160];   // the music folder, absolute path

// tab5_musica_decod.cpp: buffers and position of the open file
extern uint8_t*  g_musica_arq_buf;
extern uint8_t*  g_musica_ler_buf;       // aligned, DMA-capable (PSRAM)
extern mp3dec_t* g_musica_mp3;
extern int16_t*  g_musica_quadro;        // 1152 x 2 samples (PSRAM)
extern formato_t g_musica_fmt;
extern long      g_musica_dados_ini, g_musica_dados_fim;   // audio bytes [ini, fim)
extern uint32_t  g_musica_hz;
extern uint64_t  g_musica_quadros;       // already queued (position)
extern uint32_t  g_musica_ult_q;         // frames in the last queued block
extern uint32_t  g_musica_ler_us;        // time spent reading the card in the current block
extern uint64_t  g_musica_total_quadros; // 0 = unknown
extern uint32_t  g_musica_mp3_kbps_soma, g_musica_mp3_quadros;   // average bitrate (VBR without Xing)
extern bool      g_musica_dur_exata;     // the duration came from Xing (or it is WAV)

// tab5_musica_espectro.cpp:
extern int64_t   g_musica_fim_prev_us;   // when the already-queued sound ends (0 = empty queue)

// ---------------------------------------------------------------------------
// Small pure functions (inline: they only read bytes and text)
// ---------------------------------------------------------------------------
static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint32_t syncsafe(const uint8_t* p) { return ((p[0] & 0x7F) << 21) | ((p[1] & 0x7F) << 14) | ((p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

static inline bool termina_com(const char* s, const char* ext)
{
  const size_t a = strlen(s), b = strlen(ext);
  return a >= b && strcasecmp(s + a - b, ext) == 0;
}

// MP4 container (.mp4, .m4a, .m4v, .mov): the sound comes out of here.
static inline bool e_mp4(const char* nome)
{
  return termina_com(nome, ".mp4") || termina_com(nome, ".m4a") || termina_com(nome, ".m4v")
      || termina_com(nome, ".mov");
}

// A MUSIC PATH is relative to the card's music folder ("Rock/x.mp3")
// or ABSOLUTE on a disk ("/usb/Rock/x.mp3" - the USB flash drive). Both coexist
// in the same queue and the same playlist.
static inline bool absoluto(const char* c) { return c && c[0] == '/'; }

// Case-insensitive text hash ("Queen" and "queen" are the same
// cover): the system FNV-1a (tab5_hash.hpp), with 'sem_caixa'.
static inline uint32_t fnv(const char* s, uint32_t h = TAB5_FNV1A_INICIO) { return tab5_fnv1a_txt(s, h, true); }

// ---------------------------------------------------------------------------
// Functions that cross files
// ---------------------------------------------------------------------------
// tab5_musica_nomes.cpp
bool musica_utf8_por(char* out, size_t tam, size_t* k, uint32_t c);   // false = did not fit
bool musica_utf8_valido(const char* s);

// tab5_musica_decod.cpp
bool     musica_arquivo_aberto(void);
void     musica_fechar_arquivo(void);
bool     musica_abrir_item(int pos);     // false = bad file (the loop skips to the next)
void     musica_ir_para_ms(uint32_t ms);
size_t   musica_decodificar_bloco(int16_t* saida, uint32_t* hz, bool* estereo);

// tab5_musica_espectro.cpp
bool musica_espectro_alocar(void);
void musica_espectro_soltar(void);
void musica_espectro_esquecer(void);
void musica_espectro_bloco(const int16_t* pcm, size_t q, uint32_t hz, bool estereo, int64_t ini_us);

// tab5_musica_pasta.cpp
void musica_raiz_montado(char* out, size_t tam);   // the music folder (remembered for 5 s), with the card mounted
bool musica_montar_rel(const char* rel);           // mounts the disk where 'rel' lives
void musica_desmontar_rel(const char* rel);

// tab5_musica.cpp
bool musica_fila_tocar(const caminho_t* itens, int n, int inicio, const char* raiz,
                       const char* pasta_rel, const char* playlist, const char* app);

// tab5_musica_capa.cpp
void musica_capa_pedir(const char* caminho, const char* titulo, const char* artista, const char* album);

// tab5_musica_rede.cpp
net_t musica_capa_internet(const capa_ped_t* p, uint8_t** img, size_t* n);
