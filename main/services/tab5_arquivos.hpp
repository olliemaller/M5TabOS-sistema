// ARQUIVOS (FILES) - the system's file engine: list, copy, move, delete.
//
// WHY IT EXISTS. Each app that needed files did its own opendir
// (Camera, Azimutal, Trilha, Exemplo, Arquivos), and copying didn't exist
// anywhere - someone who plugged in a USB flash drive full of music had no way to move
// it onto the SD card. The engine lives here, once; the Arquivos app (and Berry)
// only display and request. House rule: services in the system, apps are just UI.
//
// EVERYTHING BY ABSOLUTE PATH, on any disk (services/tab5_volumes):
// "/sdcard/Música/Rock", "/usb/Fotos/x.jpg". The service holds the disk
// (tab5_volume_montar) while working on it - the caller mounts nothing.
//
// WHAT SHOWS AND WHAT CAN BE TOUCHED (the same for every app):
//   - HIDDEN: names starting with '.' (the .Lixeira, the folders the Mac
//     leaves), Windows' "System Volume Information" and "$RECYCLE.BIN", and the
//     system's password files (wifi.txt, wifi_no.txt in apps/).
//     Hidden doesn't show, can't be read and isn't copied.
//   - READ ONLY: /sdcard/apps/ (app and system data) - except
//     /sdcard/apps/Berry/, where the Berry apps live (copying a .be there
//     installs it). It can be read and copied OUT; not deleted, renamed or pasted into.
//   - The rest of the SD card and the whole USB flash drive: unrestricted.
//
// DELETING DOESN'T VANISH: it goes to the disk's own recycle bin (<root>/.Lixeira),
// which is emptied as a separate action (tab5_arq_esvaziar_lixeira).
//
// COPY AND MOVE are a BACKGROUND JOB: a pool slot, low priority
// (below the player - music playing from the SD card carries on), 64 KB blocks
// with POSIX read()/write() (stdio is ~10x slower on FAT). The job
// carries on with the app closed and the screen off (the USB drive's 5V doesn't drop:
// it holds the disk), shows progress in the bar and notifies at the end. A USB drive
// yanked out or an SD card lent to the computer become a clean error; the half-written
// file is deleted. One job at a time.
//
// CALLERS: the functions that touch the disk block (mounting the SD card,
// FAT) - from the worker (sistema_trabalho), never from the UI task. The
// state ones (tab5_arq_trabalho, the clipboard) are safe from
// any task.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TAB5_ARQ_NOME_MAX     96    // name of an item, UTF-8, including the '\0'
#define TAB5_ARQ_CAMINHO_MAX  256   // absolute path, including the '\0'
#define TAB5_ARQ_LIXEIRA      ".Lixeira"


// ---------------------------------------------------------------------------
// LISTING
// ---------------------------------------------------------------------------
// The type, from the extension - for the icon and "open with".
typedef enum {
  TAB5_ARQ_OUTRO = 0,
  TAB5_ARQ_PASTA,
  TAB5_ARQ_MUSICA,     // .mp3 .wav
  TAB5_ARQ_IMAGEM,     // .bmp .jpg .jpeg .png
  TAB5_ARQ_TEXTO,      // .txt .csv .log .json .md .m3u .gpx .nmea
  TAB5_ARQ_BERRY,      // .be
} tab5_arq_tipo_t;
tab5_arq_tipo_t tab5_arq_tipo(const char* nome);
// Does the name end in 'ext' (with the dot, e.g. ".csv"; upper or lower case)?
bool tab5_arq_extensao_e(const char* nome, const char* ext);

typedef struct {
  char            nome[TAB5_ARQ_NOME_MAX];   // as it is on disk (UTF-8)
  tab5_arq_tipo_t tipo;
  uint64_t        bytes;   // file: size; folder: 0 (summing is expensive)
  int64_t         data;    // last change (epoch), 0 = unknown
} tab5_arq_item_t;

typedef enum {
  TAB5_ARQ_ORDEM_NOME = 0,      // A-Z
  TAB5_ARQ_ORDEM_NOME_INVERSA,  // Z-A (today's log first: log_2026-10-21.txt)
  TAB5_ARQ_ORDEM_DATA,          // newest first
} tab5_arq_ordem_t;

// The VISIBLE entries of 'pasta', folders first, in the requested order.
// 'filtro': accepted extensions, ";" between them (".mp3;.wav"), or nullptr =
// all; folders always pass. Returns how many it copied (up to 'max'); 'total'
// (may be nullptr) receives how many the folder has - may exceed 'max'.
// -1 = the disk isn't there or the folder didn't open.
int tab5_arq_listar(const char* pasta, tab5_arq_item_t* out, int max, int* total,
                    tab5_arq_ordem_t ordem, const char* filtro);

// WALK - each visible entry of 'pasta', with no table and no limit (a
// folder with thousands of photos), in disk order. Fast: doesn't read size or
// date (the item gets 0) - whoever needs that uses tab5_arq_listar. 'visita'
// returns false to stop. Returns how many it visited; -1 = the folder didn't open.
typedef bool (*tab5_arq_visita_t)(const tab5_arq_item_t* item, void* ctx);
int tab5_arq_percorrer(const char* pasta, tab5_arq_visita_t visita, void* ctx);

// The LARGEST NUMBER among names following 'formato' (an sscanf with ONE %lu:
// "foto_%04lu.bmp"). 0 = none. It is the "next file" for whoever writes
// numbered files (Camera, Trilha): the numbering comes from the disk, not from a variable
// that is born zeroed on every boot.
uint32_t tab5_arq_maior_numero(const char* pasta, const char* formato);

// ---------------------------------------------------------------------------
// PERMISSIONS (the rules at the top; they don't touch the disk)
// ---------------------------------------------------------------------------
bool tab5_arq_visivel(const char* caminho);      // shows, can be read and copied
bool tab5_arq_gravavel(const char* pasta);       // can create and paste INSIDE it
bool tab5_arq_mexivel(const char* caminho);      // can delete, rename, cut
bool tab5_arq_nome_valido(const char* nome);     // acceptable new name on FAT

// ---------------------------------------------------------------------------
// QUICK OPERATIONS (block briefly - but touch the disk: outside the UI)
// ---------------------------------------------------------------------------
typedef enum {
  TAB5_ARQ_OK = 0,
  TAB5_ARQ_ERRO_PROTEGIDO,   // read-only or hidden area
  TAB5_ARQ_ERRO_NOME,        // invalid name
  TAB5_ARQ_ERRO_EXISTE,      // there is already an item with that name (or it is already there)
  TAB5_ARQ_ERRO_DISCO,       // the disk isn't there (no SD card, lent out, USB drive removed)
  TAB5_ARQ_ERRO_OCUPADO,     // a copy job is already running
  TAB5_ARQ_ERRO_ESPACO,      // doesn't fit on the destination
  TAB5_ARQ_ERRO_DENTRO,      // folder into itself
  TAB5_ARQ_ERRO_FALHOU,      // the disk refused (see the log)
} tab5_arq_erro_t;
const char* tab5_arq_erro_texto(tab5_arq_erro_t e);   // for display; never NULL

tab5_arq_erro_t tab5_arq_criar_pasta(const char* pasta, const char* nome);

// NEW FILE IN PIECES - what comes from outside (download from the internet, receive):
// creates '<pasta>/<nome>' (a repeated name becomes "x (2).ext"), holds the disk until
// closed and, if it didn't finish well (manter = false, write error, disk removed),
// DELETES the piece: there's never a half-written file. POSIX read/write (fast).
typedef struct { int fd; char caminho[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX]; char pasta[TAB5_ARQ_CAMINHO_MAX]; } tab5_arq_novo_t;
tab5_arq_erro_t tab5_arq_novo_abrir(tab5_arq_novo_t* a, const char* pasta, const char* nome);
bool            tab5_arq_novo_escrever(tab5_arq_novo_t* a, const void* dados, size_t n);
tab5_arq_erro_t tab5_arq_novo_fechar(tab5_arq_novo_t* a, bool manter);
tab5_arq_erro_t tab5_arq_renomear(const char* caminho, const char* novo_nome);
// To the disk's recycle bin (fast: just changes folder).
tab5_arq_erro_t tab5_arq_apagar(const char* caminho);

// ---------------------------------------------------------------------------
// COPY, MOVE, EMPTY - background job
// ---------------------------------------------------------------------------
// Checks everything immediately (permissions, disks, folder into itself, one
// job at a time) and, if accepted, starts in the background: TAB5_ARQ_OK =
// started, not "finished". The end and progress come in tab5_arq_trabalho() and
// via AVISO_ARQUIVOS (app.h). The paths are copied: the caller can
// free its own. A repeated name at the destination becomes "Nome (2).mp3".
#define TAB5_ARQ_ITENS_MAX  64
tab5_arq_erro_t tab5_arq_copiar(const char* const* origens, int n, const char* destino, bool mover);

// How many items (loose, without going into folders) the recycle bin of 'raiz''s disk
// has. 0 = empty or no disk. Touches the disk: outside the UI.
int tab5_arq_lixeira_itens(const char* raiz);

// Deletes FOR GOOD what is in the recycle bin of 'raiz''s disk ("/sdcard", "/usb"),
// along with the index.
tab5_arq_erro_t tab5_arq_esvaziar_lixeira(const char* raiz);

// RECYCLE BIN PER ITEM - each delete stores the origin and time in <root>/.Lixeira/.indice.
typedef struct {
  char     nome[TAB5_ARQ_NOME_MAX];       // as it is in the bin (may be "x (2)")
  char     origem[TAB5_ARQ_CAMINHO_MAX];  // path it came from; "" = unknown
  uint32_t quando;                        // epoch of when it was deleted; 0 = unknown
  uint64_t bytes;                         // file: size; folder: sum of everything inside
  bool     pasta;
} tab5_arq_lixo_t;

// Lists the recycle bin, from most recent to oldest (undated at the end).
// Returns how many it copied (up to 'max'); 'total_bytes' (may be nullptr) receives
// the size of EVERYTHING. Sums whole folders: may be slow, call from a background
// job (never from the UI).
int tab5_arq_lixeira_listar(const char* raiz, tab5_arq_lixo_t* out, int max, uint64_t* total_bytes);

// Returns an item to its origin (recreates missing folders; an occupied name becomes
// "x (2)"; unknown origin = the disk's root). Fast (only renames), but touches
// the disk: outside the UI.
tab5_arq_erro_t tab5_arq_lixeira_restaurar(const char* raiz, const char* nome);

// Deletes ONE item for good (a whole folder). A big folder takes time: call from a background
// job. ERRO_OCUPADO = a copy/empty is running.
tab5_arq_erro_t tab5_arq_lixeira_excluir(const char* raiz, const char* nome);

// Deletes for good the items deleted more than 'dias' days ago (only those with a date).
// Returns how many. May take time: background job. 0 if the clock has no time.
int tab5_arq_lixeira_limpar_antigos(const char* raiz, int dias);

// How much the recycle bin of 'raiz''s disk may weigh before the notice: 8% of the
// disk's capacity, at least 256 MB and at most 10 GB. Touches the disk: outside the UI.
uint64_t tab5_arq_lixeira_limite(const char* raiz);

// Maintenance of 'raiz''s recycle bin (background job): if the "clean up automatically" setting
// is on, deletes what is older than N days; then checks the size and publishes
// (or removes) the "Lixeira" notice in the notification centre. May take time.
void tab5_arq_lixeira_manter(const char* raiz);

// Stops the current job (the half-written file is deleted; what has already been
// copied stays).
void tab5_arq_cancelar(void);

typedef enum {
  TAB5_ARQ_PARADO = 0,   // there has never been a job (or its end has already been seen)
  TAB5_ARQ_CONTANDO,     // measuring what will be copied
  TAB5_ARQ_COPIANDO,
  TAB5_ARQ_APAGANDO,     // the source of a move between disks, or the recycle bin
  TAB5_ARQ_FEITO,
  TAB5_ARQ_FALHOU,
  TAB5_ARQ_CANCELADO,
} tab5_arq_fase_t;

typedef struct {
  tab5_arq_fase_t fase;
  bool     mover;
  int      itens, itens_feitos;      // files
  uint64_t bytes, bytes_feitos;
  uint32_t kbps;                     // average rate
  char     atual[TAB5_ARQ_NOME_MAX]; // the current file
  char     destino[TAB5_ARQ_CAMINHO_MAX];
  char     erro[96];                 // FALHOU: what happened
  uint32_t versao;                   // changes on every step (for redrawing)
} tab5_arq_trabalho_t;
void tab5_arq_trabalho(tab5_arq_trabalho_t* out);
bool tab5_arq_ocupado(void);
// Whoever showed the end to the user (Arquivos on opening): the bar notice goes
// and the stage returns to PARADO.
void tab5_arq_trabalho_visto(void);

// ---------------------------------------------------------------------------
// CLIPBOARD - the system's, not the app's: survives closing the app
// ---------------------------------------------------------------------------
// Copy / Cut store the paths; paste is tab5_arq_copiar of them.
// Cut and pasted successfully, the clipboard empties; copied, it stays (paste
// again into another folder).
bool tab5_arq_area_por(const char* const* caminhos, int n, bool recortar);
int  tab5_arq_area(bool* recortar);                    // how many items (0 = empty)
bool tab5_arq_area_item(int i, char* out, size_t tam);
void tab5_arq_area_limpar(void);
tab5_arq_erro_t tab5_arq_colar(const char* destino);
