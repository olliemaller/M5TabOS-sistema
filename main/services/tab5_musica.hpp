// SYSTEM PLAYER - the music service. Reads the files in the SD card's music
// folder, decodes them (MP3 via the minimp3 component, WAV PCM directly) and
// feeds the speaker in blocks (sistema_som_fluxo_*), with any app on
// screen - or none.
//
// WHOEVER USES IT IS JUST INTERFACE. The Music app (main/app_musica) and the Berry
// module 'musica' (services/berry_apps) are two faces of this same service: they list,
// ask to play, show the state and the spectrum. None of them decodes,
// reads the card or touches the speaker. One player per device: whoever asks
// to play replaces the queue of whoever was playing.
//
// WHAT THE SYSTEM GUARANTEES WITHOUT ANYONE ASKING (the service runs as a kernel
// background part, kernel/fundo.h, named "Música"):
//   - plays with the requester's window closed;
//   - the status bar shows what is playing, and the notification centre has the miniplayer and
//     the X that stops it (kernel/midia.h) - tapping the title opens the app that asked;
//   - paused for TAB5_MUSICA_PAUSA_MAX_S (or with nothing to play for
//     TAB5_MUSICA_VAZIO_MAX_S), it ends by itself;
//   - the card going to the computer (USB drive mode) stops the music immediately;
//   - each track's cover (tag, folder or internet) is looked up automatically.
// Shuffle/repeat, playlists (.m3u) and each file's duration also live
// here - the interface only asks and shows.
//
// WHERE IT PLAYS FROM: the card's music folder (paths RELATIVE to it,
// "Rock/x.mp3") or any folder on a disk, by ABSOLUTE path
// ("/usb/Rock/x.mp3" - the USB flash drive in USB-A, services/tab5_volumes). Every
// function taking 'rel' or 'caminho' accepts both; the queue, playlists
// and Favourites mix the two without trouble (a track from a USB drive that
// is not plugged in is skipped). The whole USB drive is its "music folder":
// listar("/usb") shows the USB drive's root.
//
// TASKS: everything here may be called from any task (interface, an app's
// task, Berry, console). The controls only flag a request; the service loop
// serves it, between one sound block and the next.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TAB5_MUSICA_NOME_MAX   128           // file name as on disk (bytes, UTF-8)
#define TAB5_MUSICA_CAMINHO_MAX 256          // relative to the music folder ("Rock/x.mp3") or absolute ("/usb/...")
#define TAB5_MUSICA_FILA_MAX   400           // files in a queue (a folder or a playlist)
#define TAB5_MUSICA_LISTA_MAX  400           // items listar() returns from a folder
#define TAB5_MUSICA_PAUSA_MAX_S (5 * 60)     // paused (with music loaded) longer than this, the player shuts down
#define TAB5_MUSICA_VAZIO_MAX_S 30           // nothing to play (the list ended, stopped): shuts down sooner
#define TAB5_MUSICA_FAIXAS     32            // spectrum bands
// Playlists are .m3u files in this subfolder of the music folder - the same
// format any computer player opens (lines "../Rock/x.mp3").
// The folder does not appear in listar(): the interface shows playlists separately.
#define TAB5_MUSICA_PLAYLISTS  "Playlists"
#define TAB5_MUSICA_FAVORITAS  "Favoritas"   // the heart playlist

// ---- the music folder -------------------------------------------------------
// Any folder in the card's ROOT whose name, ignoring accents and
// case, starts with "music" (Music, Música, Musicas, MUSICA~1); with more than
// one, the one with the most in it. None: creates "Music". The empty "Music" the
// system created earlier is deleted when there is another with music.
// out receives the absolute path ("/sdcard/Música"). false = no card.
bool tab5_musica_pasta(char* out, size_t tam);

typedef struct {
  char nome[TAB5_MUSICA_NOME_MAX];   // as on disk (UTF-8)
  bool pasta;
} tab5_musica_item_t;

// The contents of a subfolder ('rel' relative to the music folder, "" = the root;
// or absolute, "/usb" = the USB drive's root):
// folders first, then .mp3/.wav, in alphabetical order (the playlists
// folder and hidden ones are left out). Returns how many
// (up to 'max'); -1 = no card or the folder does not exist. 'cheia' (optional) =
// there were more than 'max'.
int tab5_musica_listar(const char* rel, tab5_musica_item_t* out, int max, bool* cheia);

// A name from the disk, for display. FAT already returns UTF-8 (and this only copies);
// text that is not valid UTF-8 came from before that - an old playlist,
// written in CP437 - and is converted.
void tab5_musica_nome_utf8(const char* nome, char* out, size_t out_tam);

// ---- playing ------------------------------------------------------------------
// PLAYS the folder 'rel' from item 'idx' of tab5_musica_listar(rel) (it has
// to be a file): the queue is the folder's tracks, in order. 'app' = the
// name of the app that asked (tapping the title in the miniplayer opens it; nullptr = the
// Music app). false = did not start (no card, item is not music, out of memory).
bool tab5_musica_tocar(const char* rel, int idx, const char* app);

// PLAYS A single file ('caminho' absolute, e.g. "/sdcard/Videos/filme.mp3"): the
// queue holds only it, so at the end the player stops (or repeats, if repeat
// is on) instead of moving to the next in the folder. It is a video's
// sound (services/tab5_video). 'app' as in tab5_musica_tocar.
bool tab5_musica_tocar_um(const char* caminho, const char* app);

// PLAYS a playlist (the name as on the card, without .m3u) from track
// 'idx' of tab5_musica_playlist_itens(); idx < 0 = from the start (with
// shuffle on, any one). Tracks that vanished from the card are
// skipped. false = empty playlist or did not open.
bool tab5_musica_tocar_playlist(const char* nome, int idx, const char* app);

// ---- how the queue moves ------------------------------------------------------
// SHUFFLE: the queue is shuffled (the current track keeps playing and becomes the
// first); turning it off restores the folder/playlist order without cutting the sound.
// REPEAT: NAO (off) = plays to the end and stops; TUDO (all) = at the end goes back to the
// start (with shuffle, reshuffles); UMA (one) = the same track again when it ends (the
// next/previous buttons still move along the queue). Stored in
// settings: they apply to the next queue and on restarting.
typedef enum {
  TAB5_MUSICA_REPETIR_NAO = 0,
  TAB5_MUSICA_REPETIR_TUDO,
  TAB5_MUSICA_REPETIR_UMA,
} tab5_musica_repetir_t;
void tab5_musica_definir_aleatorio(bool sim);
bool tab5_musica_aleatorio(void);
void tab5_musica_definir_repetir(tab5_musica_repetir_t r);
tab5_musica_repetir_t tab5_musica_repetir(void);

void tab5_musica_tocar_pausar(void);
void tab5_musica_proxima(void);
void tab5_musica_anterior(void);   // after 3 s of a track, goes back to its start
void tab5_musica_ir_para(uint32_t ms);
void tab5_musica_parar(void);      // asks and carries on: the service exits soon after

typedef struct {
  bool     ativo;          // there is a queue (the service is running)
  bool     tocando;        // false = paused (or loading)
  int      idx, n;         // position in the queue's play order (0 = the first)
  char     pasta[160];     // the queue's folder, absolute path ("" = playlist queue)
  char     rel[160];       // the same, relative to the music folder ("" = the root)
  char     playlist[TAB5_MUSICA_NOME_MAX];   // the playlist playing (as on the card); "" = a folder
  char     caminho[TAB5_MUSICA_CAMINHO_MAX]; // what is playing, relative to the music folder
  char     arquivo[TAB5_MUSICA_NOME_MAX];    // just the file name
  char     titulo[64];     // from the ID3 tag, or from the file name ("Artista - Titulo (Video)" -> "Titulo")
  char     artista[48];    // from the ID3 tag, or the part before " - " in the name ("" = none)
  char     album[48];      // from the ID3 tag ("" = none)
  bool     aleatorio;
  uint8_t  repetir;        // tab5_musica_repetir_t
  uint32_t pos_ms, dur_ms; // the position of what is COMING OUT of the speaker; dur 0 = unknown
  uint32_t hz;
  uint8_t  canais;
  uint16_t kbps;           // 0 = WAV
  char     erro[48];       // the last problem ("" = none)
  // DIAGNOSTICS (console: musica estado): the sound stutters when the speaker
  // queue runs dry. 'secas' counts blocks queued with the queue ALREADY
  // empty (except the first after opening, seeking or resuming).
  uint32_t blocos, secas;
  uint32_t dec_max_us, dec_media_us;   // decoding one block
  uint32_t bloco_ms;                   // how much sound fits in a block
  uint32_t ler_no_pior_us;             // of the worst block, how much was reading the card
  uint32_t ler_soma_ms;                // total time reading the card
  uint32_t esp_max_us;                 // worst spectrum time in a block
} tab5_musica_estado_t;

void tab5_musica_estado(tab5_musica_estado_t* out);
void tab5_musica_zerar_diag(void);

// ---- the spectrum -------------------------------------------------------------
// The bands (0..255, bass first) of the sound coming out NOW. Each
// frame (1024 samples, ~23 ms) carries the time it leaves the speaker: the
// bar matches what you hear. The service only computes while SOMEONE READS: the
// first read switches it on, and a second without reads switches it off - with the window
// closed or the screen off, it costs nothing. false = nothing playing (pause,
// end, or the first read, which only switches the computation on).
bool tab5_musica_espectro(uint8_t* faixas);

// ---- the colour ---------------------------------------------------------------
// The player's accent colour (RGB565; 0 = the system's): the status bar pill
// and the miniplayer use it. Stored in settings: applies on restarting.
void tab5_musica_definir_cor(uint16_t cor565);

// ---- playlists ------------------------------------------------------------------
// Everything here reads or writes the card: call OUTSIDE the interface task
// (sistema_trabalho), except where stated otherwise. Playlist names as
// they are on the card (without .m3u, UTF-8).
typedef struct {
  char caminho[TAB5_MUSICA_CAMINHO_MAX];   // relative to the music folder
} tab5_musica_faixa_t;

// The playlists, in alphabetical order (Favoritas first). 'out[i].pasta' =
// false; -1 = no card.
int  tab5_musica_playlists(tab5_musica_item_t* out, int max);
// Creates an empty one. The name comes from the person (UTF-8); letters the card
// cannot store become the closest match. 'nome_cartao' (optional) receives the final name.
// false = already exists, empty name or no card.
bool tab5_musica_playlist_criar(const char* nome_utf8, char* nome_cartao, size_t tam);
bool tab5_musica_playlist_apagar(const char* nome);
// The tracks, in file order. -1 = does not exist.
int  tab5_musica_playlist_itens(const char* nome, tab5_musica_faixa_t* out, int max);
// Appends at the end (the playlist is created if missing) / removes. false = already there (add)
// or was not there (remove).
bool tab5_musica_playlist_por(const char* nome, const char* caminho);
bool tab5_musica_playlist_tirar(const char* nome, const char* caminho);
bool tab5_musica_playlist_tem(const char* nome, const char* caminho);

// ---- duration ---------------------------------------------------------------------
// How long a file lasts (path relative to the music folder), in ms, without
// decoding: reads the header (MP3: Xing/VBRI, or the bitrate and size; WAV:
// the header). A few ms of card access - outside the interface. 0 = could not.
// Remembers what it has measured (the same file is not read twice).
uint32_t tab5_musica_duracao(const char* caminho);
// Only what has already been measured, without touching the card (may be from the interface).
// false = not measured yet.
bool tab5_musica_duracao_lembrada(const char* caminho, uint32_t* ms);

// ---- cover ------------------------------------------------------------------------
// THE COVER OF WHAT IS PLAYING. When a track opens the service looks, on its own
// task and in this order: the image inside the ID3 tag (APIC), a
// cover/folder/capa.jpg in the track's folder, and - with Wi-Fi connected - the
// iTunes search by artist and title. Whatever it finds goes into the cache
// <music folder>/.capas/ (next time it does not search again; the internet is only
// queried once per track).
typedef enum {
  TAB5_MUSICA_CAPA_NENHUMA = 0,   // none (or nothing playing)
  TAB5_MUSICA_CAPA_BUSCANDO,      // searching (may still appear)
  TAB5_MUSICA_CAPA_PRONTA,        // 'arquivo' = JPEG or PNG on the card, absolute path
} tab5_musica_capa_t;
// May be from the interface (only copies the state). 'versao' changes on every new cover
// (track change, search finished): whoever draws reloads when it changes.
tab5_musica_capa_t tab5_musica_capa(char* arquivo, size_t tam, uint32_t* versao);
// Reads the cover file ('arquivo' from tab5_musica_capa) whole into PSRAM
// (JPEG or PNG; up to 3 MB). Reads the card: call OUTSIDE the interface. The
// caller frees with heap_caps_free. nullptr = did not open or too big.
uint8_t* tab5_musica_capa_ler(const char* arquivo, size_t* n);

// ---- name for display -----------------------------------------------------------
// The title (and the artist, if the name has one) from a file name:
// "Artista - Titulo (Video).mp3" -> "Titulo" and "Artista". Without " - ", the whole
// name is the title; '_' becomes a space; no extension and no bits in
// parentheses/brackets. 'artista' may be nullptr (title only).
void tab5_musica_titulo_do_arquivo(const char* nome, char* titulo, size_t tt, char* artista, size_t ta);
