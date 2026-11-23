#include "tab5_sd.hpp"
#include "idioma.h"
#include "tab5_marcos.hpp"
#include "tab5_reserva.hpp"
#include "tab5_power.hpp"   // tab5_power_em_repouso (the pace of the "cartao" task)
#include <errno.h>

#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"   // ff_diskio_get_pdrv_card - the FatFs drive, for the label
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

static const char *TAG = "sd";

// ---- wiring (M5Unified, _pin_table_sd, board_M5Tab5 row) ------------------
static const int SD_CLK = 43;
static const int SD_CMD = 44;
static const int SD_D0  = 39;
static const int SD_D1  = 40;
static const int SD_D2  = 41;
static const int SD_D3  = 42;

// Channel of the internal LDO that powers the SDMMC IO.
//
// The P4 declares SOC_SDMMC_IO_POWER_EXTERNAL: the card's IO supply does NOT
// come from a fixed rail, it comes from an on-chip LDO that has to be
// switched on first. Channel 4 (LDO_VO4) is the one the P4 reference boards use;
// M5GFX already uses channel 3 for the DSI PHY, which supports 4 being free
// for this. If mounting fails with a timeout error, this number is the first
// suspect.
static const int SD_LDO_CANAL = 4;

// ---- state -----------------------------------------------------------------
static sdmmc_card_t*       s_card = nullptr;
static sd_pwr_ctrl_handle_t s_ldo = nullptr;
static int                 s_refs = 0;

// The reference count is touched by different tasks: the UI (when
// opening and closing apps) and the energy log (on each flush). Without a lock, two
// simultaneous montar() calls could mount twice, or a desmontar() could pull the
// rug out from under whoever is writing. Mutex created on demand, on the first call.
static SemaphoreHandle_t   s_mutex = nullptr;

// The mutex must NOT be created on demand inside trava(): two tasks
// arriving together the first time would create two different mutexes and each
// would lock its own - no protection at all, and one leaks too. Created once in
// tab5_sd_iniciar(), before any concurrency exists.
static void trava(void)
{
  if (s_mutex) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
}
static void destrava(void)
{
  if (s_mutex) { xSemaphoreGive(s_mutex); }
}

// THE SDMMC CONTROLLER LOCK (see tab5_sd.hpp). Created together with s_mutex,
// in tab5_sd_iniciar, before any task (including Wi-Fi bring-up).
static SemaphoreHandle_t s_controlador = nullptr;
void tab5_sd_controlador_travar(void)
{
  if (s_controlador) { xSemaphoreTake(s_controlador, portMAX_DELAY); }
}
void tab5_sd_controlador_soltar(void)
{
  if (s_controlador) { xSemaphoreGive(s_controlador); }
}
// ---------------------------------------------------------------------------
// THE CARD STATE IS FOUR THINGS, NOT TWO
// ---------------------------------------------------------------------------
// The description said "no card" every time the card was UNMOUNTED - which is
// the normal state when no app is using it, since mounting is on
// demand and reference-counted. The `servicos` command then announced "no
// card" with the card inserted and working.
//
// Mixing "not mounted right now" with "doesn't exist" is the same mistake as Wi-Fi
// saying "failed" when the coprocessor was silent: two different causes,
// a single message, and the user sent to investigate the wrong place.
//
//   nao verificado      - nobody has tried to mount yet this boot
//   SD 14.5GB           - mounted now
//   SD 14.5GB (livre)   - mounted before, now unmounted, the card is still there
//   ausente ou ilegivel - the last mount attempt failed
static char s_ident[32]      = "";      // identity, once we know it
static bool s_ja_montou      = false;   // some mount succeeded this boot
// THE CARD NAME (FAT volume label), read on every real mount:
// renamed on the computer during USB flash drive mode, the return remounts and reads it again.
// Stays valid with the card unmounted. Empty = no name.
static char s_rotulo[36]     = "";
static void arrumar_sistema(void);
static bool s_anunciou = false;   // the boot's first mount has already been logged

// DEFERRED UNMOUNT. When the last user hands the card back,
// it stays mounted for another SD_OCIOSO_MS: the log, the monitor, the energy log
// and the track write at nearby moments, and each mount powers the regulator and
// reinitialises the card. With the slack, neighbouring writes share ONE mount;
// if nobody asks again, it unmounts and the regulator powers off. It is the same
// "idle timeout" pattern as the speaker (docs/ARQUITETURA.md, 1.1).
static const uint64_t   SD_OCIOSO_US = 10ULL * 1000 * 1000;
static esp_timer_handle_t s_ocioso = nullptr;
static void desmontar_de_verdade(void);

// When the card last REALLY mounted/unmounted (us of uptime).
// Diagnostics: the monitor correlates it with the panel's late frame (the card is
// powered by an LDO of the P4 itself, neighbour of the DSI LDO).
static volatile int64_t s_montou_us = 0, s_desmontou_us = 0;
void tab5_sd_ultimas_transicoes(int64_t* montou_us, int64_t* desmontou_us)
{
  if (montou_us)    { *montou_us = s_montou_us; }
  if (desmontou_us) { *desmontou_us = s_desmontou_us; }
}
static void ao_ocioso(void*);
static bool s_ultima_falhou  = false;   // the last attempt failed
// NO CARD, GROWING BACKOFF. Each attempt on an empty slot takes ~1 s of
// timeout with the lock held, and everyone who writes (log, Wi-Fi, notifications)
// requests a mount: unchecked it was 5 error lines per second and a second of
// queueing for each. After a failure, the next real attempt only
// happens after SD_ESPERA_MIN_MS, doubling with each new failure up to SD_ESPERA_MAX_MS;
// in between, montar returns failure immediately. A card inserted with the
// device on is found within SD_ESPERA_MAX_MS.
#define SD_ESPERA_MIN_MS  2000
#define SD_ESPERA_MAX_MS 10000
static int64_t  s_proxima_us = 0;
static uint32_t s_espera_ms  = 0;
static char s_desc[64]       = "nao verificado";

static void gravador_iniciar(void);

// LENT TO THE COMPUTER (USB flash drive mode, see tab5_sd_emprestar in the .hpp).
// While in effect, the card belongs to the computer: montar() refuses at once, as
// if there were no card, and the writer holds on to whatever arrives. s_cru is the same
// card, initialised WITHOUT a file system - sectors only.
static volatile bool s_emprestado = false;
static sdmmc_card_t* s_cru = nullptr;
static tab5_sd_resumo_t s_resumo = { };   // the journal of this loan (tab5_sd_resumo)

// The raw card's OWN lock: the sector ports (tab5_sd_cru_*) and the hand-back
// (which releases s_cru) go through it, so the card is never released with an
// operation in progress. Created in tab5_sd_iniciar, like s_mutex.
static SemaphoreHandle_t s_cru_mutex = nullptr;
static volatile uint32_t s_cru_recuperacoes = 0, s_cru_falhas = 0, s_cru_max_ms = 0;

void tab5_sd_iniciar(void)
{
  if (!s_mutex) { s_mutex = xSemaphoreCreateMutex(); }
  if (!s_controlador) { s_controlador = xSemaphoreCreateMutex(); }
  if (!s_cru_mutex) { s_cru_mutex = xSemaphoreCreateMutex(); }
  // The journal BEFORE the writer: whatever was left from a flash drive session cut off by
  // a restart must be visible when the first write arrives.
  tab5_reserva_iniciar();
  gravador_iniciar();
  if (!s_ocioso) {
    const esp_timer_create_args_t a = { .callback = ao_ocioso, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "sd_ocioso",
                                        .skip_unhandled_events = true };
    esp_timer_create(&a, &s_ocioso);
  }

  // SILENCED FOREVER, not just for a window - the warning flooded the console.
  // sd_pwr_ctrl_new_on_chip_ldo()
  // (called in montar_de_verdade() just below) always creates the LDO
  // controller with voltage 0 before programming the real voltage, and the IDF driver warns
  // about it at WARN every time - harmless (the card mounts and reads normally, the real voltage
  // is set right afterwards), but the card was mounted/unmounted every
  // ~15s FOREVER (it was the log itself feeding back - fixed in
  // see montar_de_verdade and the deferred unmount) - and not just in a rare
  // window like "ledc" during wake-up (tab5_power.cpp), so there is no
  // "afterwards" to restore the level: it is the same logic as "eh_sdio"/"sdmmc_io"
  // in tab5_wifi_iniciar(), silenced once at boot.
  esp_log_level_set("ldo", ESP_LOG_ERROR);
  // The lower layers repeat, on every attempt, the error ours already reports
  // (and only on a state change - see montar_de_verdade).
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
}

bool tab5_sd_montado(void) { return s_card != nullptr; }
const char* tab5_sd_nome(void) { return s_rotulo; }

// THE VOLUME NAME. FAT32 keeps the label in TWO places: a special entry
// in the root folder (the one FatFs reads, f_getlabel) and a field in the volume's
// boot sector (offset 0x47, 11 bytes). Whoever renames doesn't always write
// both - so, without the root entry, the boot sector one wins. "NO NAME" there
// is the formatter's "no name". Both in uppercase (that's what FAT stores).
static void ler_rotulo(void)
{
  s_rotulo[0] = 0;
  const char drv[3] = { (char)('0' + ff_diskio_get_pdrv_card(s_card)), ':', 0 };
  char r[sizeof(s_rotulo)] = "";
  if (f_getlabel(drv, r, nullptr) == FR_OK && r[0]) {
    snprintf(s_rotulo, sizeof(s_rotulo), "%s", r);
    return;
  }
  // The volume boot sector: sector 0 of the card, or the start of the 1st partition
  // (card with a partition table - the normal case, formatted on a computer).
  // Read raw and not via f_getfree, which on a large card may scan the FAT.
  uint8_t* setor = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!setor) { return; }
  bool ok = sdmmc_read_sectors(s_card, setor, 0, 1) == ESP_OK && setor[510] == 0x55 && setor[511] == 0xAA;
  if (ok && memcmp(setor + 0x52, "FAT32", 5) != 0) {
    const uint32_t inicio = setor[0x1C6] | (setor[0x1C7] << 8) | (setor[0x1C8] << 16) | ((uint32_t)setor[0x1C9] << 24);
    ok = inicio && sdmmc_read_sectors(s_card, setor, inicio, 1) == ESP_OK && setor[510] == 0x55
         && setor[511] == 0xAA && memcmp(setor + 0x52, "FAT32", 5) == 0;
  }
  if (ok) {
    char b[12];
    memcpy(b, setor + 0x47, 11);
    b[11] = 0;
    for (int i = 10; i >= 0 && b[i] == ' '; --i) { b[i] = 0; }   // the field is padded with spaces
    if (b[0] && strcmp(b, "NO NAME") != 0) { snprintf(s_rotulo, sizeof(s_rotulo), "%s", b); }
  }
  heap_caps_free(setor);
}

const char* tab5_sd_descricao(void)
{
  if (s_emprestado)         { snprintf(s_desc, sizeof(s_desc), N_("no computador (USB)")); }
  else if (s_card)          { snprintf(s_desc, sizeof(s_desc), "%s", s_ident); }
  else if (s_ultima_falhou) { snprintf(s_desc, sizeof(s_desc), N_("ausente ou ilegivel")); }
  else if (s_ja_montou)     { snprintf(s_desc, sizeof(s_desc), "%s (livre)", s_ident); }
  else                      { snprintf(s_desc, sizeof(s_desc), N_("nao verificado")); }
  return s_desc;
}

// SDMMC host and card slot, with the LDO (s_ldo) already on and tied to the host.
// Used by the normal mount and by the flash drive raw access - both MUST
// talk to the card the same way (slot 0, see the warning below).
static void configurar_host(sdmmc_host_t* host, sdmmc_slot_config_t* slot)
{
  *host = SDMMC_HOST_DEFAULT();

  // SLOT 0, AND THIS IS NOT A DETAIL.
  //
  // SDMMC_HOST_DEFAULT() comes with .slot = SDMMC_HOST_SLOT_1
  // (sdmmc_default_configs.h:31), and esp_hosted uses SLOT 1 to talk to the
  // Wi-Fi ESP32-C6 (CONFIG_ESP_HOSTED_HOST_SDIO_SLOT=1). With both on the same
  // slot, mounting the card reprograms the slot's pins and RIPS THE C6 OFF THE
  // BUS. That is exactly what happened:
  //
  //   wifi: C6 no ar, firmware 1.4.1            <- link to the C6 up
  //   sdmmc_periph: SDMMC host already initialized, skipping initflow
  //   sdmmc_io: sdmmc_send_cmd returned 0x107   <- C6 dead, 1ms later
  //   eh_sdio: Unrecoverable host sdio state    -> board restart
  //
  // The P4 has two slots (SOC_SDMMC_NUM_SLOTS=2) and routes via the GPIO matrix
  // (SOC_SDMMC_USE_GPIO_MATRIX=1), so any pin works on any slot.
  // We changed OUR side and not esp_hosted's, so as not to touch the board
  // preset Espressif maintains.
  host->slot = SDMMC_HOST_SLOT_0;
  host->pwr_ctrl_handle = s_ldo;

  // 4-bit slot, with the Tab5 pins. The P4 has a GPIO matrix for
  // SDMMC, so they can be remapped freely.
  *slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot->width = 4;
  slot->clk = (gpio_num_t)SD_CLK;
  slot->cmd = (gpio_num_t)SD_CMD;
  slot->d0  = (gpio_num_t)SD_D0;
  slot->d1  = (gpio_num_t)SD_D1;
  slot->d2  = (gpio_num_t)SD_D2;
  slot->d3  = (gpio_num_t)SD_D3;
  slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static bool montar_de_verdade(void)
{
  // 1. Switch on the LDO that powers the card IO.
  sd_pwr_ctrl_ldo_config_t ldo_cfg = { };
  ldo_cfg.ldo_chan_id = SD_LDO_CANAL;
  esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "card LDO (channel %d) failed: %s", SD_LDO_CANAL, esp_err_to_name(err));
    return false;
  }

  // 2. SDMMC host and slot - the same configuration as raw access (flash drive).
  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  configurar_host(&host, &slot);

  esp_vfs_fat_sdmmc_mount_config_t mnt = { };
  // NEVER format on its own. A card that won't mount may just have the
  // wrong file system, and formatting would destroy what the user has on it.
  // If formatting is ever needed, it must be an explicit button in an app.
  mnt.format_if_mount_failed = false;
  mnt.max_files              = 8;
  mnt.allocation_unit_size   = 16 * 1024;

  tab5_marco("sd: mounting", 0);
  tab5_sd_controlador_travar();   // host/slot 0 coming up: slot 1 (C6) must not touch it meanwhile
  err = esp_vfs_fat_sdmmc_mount(TAB5_SD_RAIZ, &host, &slot, &mnt, &s_card);
  tab5_sd_controlador_soltar();
  if (err != ESP_OK) {
    // Only the FIRST failure of a run goes to the log; repeats of the
    // same state are debug.
    if (!s_ultima_falhou) {
      if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "card present but no readable FAT file system");
      } else {
        ESP_LOGE(TAG, "mount failed: %s (card missing or badly seated?) - retrying within %d s",
                 esp_err_to_name(err), SD_ESPERA_MAX_MS / 1000);
      }
    } else {
      ESP_LOGD(TAG, "mount failed again: %s", esp_err_to_name(err));
    }
    sd_pwr_ctrl_del_on_chip_ldo(s_ldo);
    s_ldo  = nullptr;
    s_card = nullptr;
    s_ultima_falhou = true;
    return false;
  }

  if (s_ultima_falhou) { ESP_LOGI(TAG, "card found"); }
  s_ultima_falhou = false;
  s_ja_montou     = true;
  snprintf(s_ident, sizeof(s_ident), "%s %.1fGB",
           s_card->is_sdio ? "SDIO" : (s_card->is_mmc ? "MMC" : "SD"),
           ((double)s_card->csd.capacity * s_card->csd.sector_size) / (1024.0 * 1024.0 * 1024.0));

  ler_rotulo();

  // Ensures the apps root exists. mkdir returning EEXIST is not an error.
  mkdir(TAB5_SD_APPS, 0777);

  // MOUNTING IS NOT NEWS. These lines used to go to the log, the log is
  // written TO THE CARD, and writing requires mounting: each log flush produced
  // the two lines that forced the next flush - the card mounted and
  // unmounted every 15s forever, logging that it had logged. Only the
  // boot's first mount stays visible; the rest is debug.
  if (!s_anunciou) {
    ESP_LOGI(TAG, "mounted at %s: %s, name '%s' (%d kHz, %d bit(s), DDR %d)", TAB5_SD_RAIZ, s_ident, s_rotulo,
             s_card->real_freq_khz, s_card->log_bus_width ? (1 << s_card->log_bus_width) : 1, s_card->is_ddr);
    s_anunciou = true;
    arrumar_sistema();
  }
  else             { ESP_LOGD(TAG, "mounted"); }
  s_montou_us = esp_timer_get_time();
  return true;
}

// THE SISTEMA FOLDER IN SUBFOLDERS. The daily logs and the energy
// files all used to sit loose in apps/Sistema and drowned out the rest (wifi.txt,
// C6 firmware). Now each has its own: TAB5_SD_LOGS and TAB5_SD_ENERGIA.
// Whatever was already on the card the old way is moved ONCE per boot, on the
// first mount - with nothing to move, it only costs listing the folder.
static bool tem_prefixo(const char* s, const char* p) { return strncasecmp(s, p, strlen(p)) == 0; }

static void mover_para(const char* base, const char* nome, const char* sub, int* n)
{
  char dest_pasta[96];
  if (!tab5_sd_pasta(sub, dest_pasta, sizeof(dest_pasta))) { return; }
  char de[200], para[200];
  snprintf(de, sizeof(de), "%s/%s", base, nome);
  snprintf(para, sizeof(para), "%s/%s", dest_pasta, nome);
  struct stat st;
  if (stat(para, &st) == 0) {
    ESP_LOGW(TAG, "%s already exists in %s - left the old one where it was", nome, sub);
    return;
  }
  if (rename(de, para) == 0) { ++*n; }
  else { ESP_LOGW(TAG, "could not move %s: %s", nome, strerror(errno)); }
}

static void arrumar_sistema(void)
{
  char base[96];
  snprintf(base, sizeof(base), "%s/Sistema", TAB5_SD_APPS);
  DIR* d = opendir(base);
  if (!d) { return; }
  // Collect the names before moving: renaming during readdir of the same folder
  // can skip or repeat entries in FAT.
  // In PSRAM and only during the tidy-up: runs once per boot.
  char (*nomes)[64] = (char (*)[64])heap_caps_malloc(48 * 64, MALLOC_CAP_SPIRAM);
  if (!nomes) { closedir(d); return; }
  int total = 0;
  while (struct dirent* e = readdir(d)) {
    if (e->d_type == DT_DIR || total >= 48) { continue; }
    const char* nm = e->d_name;
    const bool eh_log = tem_prefixo(nm, "log_");
    const bool eh_ene = tem_prefixo(nm, "energia");
    if ((eh_log || eh_ene) && strlen(nm) < 64) {
      snprintf(nomes[total++], 64, "%s", nm);
    }
  }
  closedir(d);

  int n = 0;
  for (int i = 0; i < total; ++i) {
    mover_para(base, nomes[i], tem_prefixo(nomes[i], "log_") ? TAB5_SD_LOGS : TAB5_SD_ENERGIA, &n);
  }
  heap_caps_free(nomes);
  if (n) { ESP_LOGI(TAG, "Sistema tidied: %d file(s) moved to logs/ and energia/", n); }
}

bool tab5_sd_montar(void)
{
  trava();
  bool ok;
  if (s_emprestado) {
    // The computer has the card: refuse quietly, without the growing "no card"
    // backoff - it comes back whole when the flash drive session ends.
    ok = false;
  } else if (s_card) {
    // Already mounted - perhaps just waiting out the idle timeout: cancel the timeout
    // and count. This is where the mount gets reused.
    if (s_refs == 0 && s_ocioso) { esp_timer_stop(s_ocioso); }
    s_refs++;
    ok = true;
  } else if (s_ultima_falhou && esp_timer_get_time() < s_proxima_us) {
    ok = false;                               // backing off after a failure: don't even try
  } else if (montar_de_verdade()) {
    s_refs = 1;
    s_espera_ms = 0;
    ok = true;
  } else {
    ok = false;                               // failed: do NOT increment
    s_espera_ms = s_espera_ms ? s_espera_ms * 2 : SD_ESPERA_MIN_MS;
    if (s_espera_ms > SD_ESPERA_MAX_MS) { s_espera_ms = SD_ESPERA_MAX_MS; }
    s_proxima_us = esp_timer_get_time() + (int64_t)s_espera_ms * 1000;
  }
  destrava();
  return ok;
}

// Called with the lock held.
static void desmontar_de_verdade(void)
{
  tab5_marco("sd: unmounting", 0);
  tab5_sd_controlador_travar();   // deinit of slot 0 (or the whole host, if it is the last one)
  esp_vfs_fat_sdcard_unmount(TAB5_SD_RAIZ, s_card);
  tab5_sd_controlador_soltar();
  s_card = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
  // The identity is NOT erased: the card is still inserted, just not
  // mounted. Erasing it here is what made `servicos` announce "no card"
  // with the card in place.
  ESP_LOGD(TAG, "unmounted");
  s_desmontou_us = esp_timer_get_time();
}

// The idle timeout expired (esp_timer task). If nobody took the
// card back in the meantime, unmount now.
static void ao_ocioso(void*)
{
  trava();
  if (s_refs == 0 && s_card) { desmontar_de_verdade(); }
  destrava();
}

void tab5_sd_desmontar(void)
{
  trava();
  if (s_refs <= 0 || --s_refs > 0) {          // nobody mounted, or someone still uses it
    destrava();
    return;
  }
  // The last one handed it back: don't unmount yet - arm the timeout (or restart it).
  if (s_ocioso) {
    esp_timer_stop(s_ocioso);
    esp_timer_start_once(s_ocioso, SD_OCIOSO_US);
  } else {
    desmontar_de_verdade();
  }
  destrava();
}

// ===========================================================================
// LENDING TO THE COMPUTER (USB flash drive mode) - see the .hpp
// ===========================================================================
static void tarefa_cartao_acordar(void);
static void gravacao_travar(bool travar);
static void esvaziar_buffer(void);   // with the write lock held
static void aplicar_diario(void);    // ditto

// ---------------------------------------------------------------------------
// THE CARD SNAPSHOT AT LENDING TIME
// ---------------------------------------------------------------------------
// Size and date of every file in apps/ at the instant the card leaves. A
// file REPLACED during flash drive mode takes its snapshot along into the journal
// (the snapshot itself stays only in RAM: it is the record that must survive a
// restart). On return, a file differing from the snapshot = the computer touched it.
typedef struct { uint32_t hash, tam, data; } foto_t;
#define FOTO_MAX 4096
static foto_t* s_foto = nullptr;
static int     s_foto_n = 0;

static uint32_t hash_texto(const char* s)
{
  uint32_t h = 2166136261u;   // FNV-1a
  while (*s) { h = (h ^ (uint8_t)*s++) * 16777619u; }
  return h;
}

static void fotografar_pasta(const char* pasta, int nivel)
{
  DIR* d = opendir(pasta);
  if (!d) { return; }
  while (struct dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') { continue; }
    char caminho[192];
    if (snprintf(caminho, sizeof(caminho), "%s/%s", pasta, e->d_name) >= (int)sizeof(caminho)) { continue; }
    if (e->d_type == DT_DIR) {
      if (nivel < 4) { fotografar_pasta(caminho, nivel + 1); }
      continue;
    }
    if (s_foto_n >= FOTO_MAX) { continue; }   // beyond that: no snapshot, the return treats it as a conflict
    struct stat st;
    if (stat(caminho, &st) != 0) { continue; }
    s_foto[s_foto_n++] = { hash_texto(caminho), (uint32_t)st.st_size, (uint32_t)st.st_mtime };
  }
  closedir(d);
}

// With the card mounted and the lock held.
static void fotografar(void)
{
  if (!s_foto) { s_foto = (foto_t*)heap_caps_malloc(FOTO_MAX * sizeof(foto_t), MALLOC_CAP_SPIRAM); }
  s_foto_n = 0;
  if (!s_foto) { return; }
  fotografar_pasta(TAB5_SD_APPS, 0);
}

static const foto_t* foto_de(const char* caminho)
{
  const uint32_t h = hash_texto(caminho);
  for (int i = 0; i < s_foto_n; ++i) { if (s_foto[i].hash == h) { return &s_foto[i]; } }
  return nullptr;
}

// THE SNAPSHOT IS ONLY USEFUL WHILE THE CARD IS AWAY: each REPLACE record in the
// journal already carries its own base. Freed at the end of the loan (or when it
// is abandoned) - 48 KB of PSRAM that used to stay held until restart. With the write
// lock held: that is what the replace uses to read the snapshot.
static void foto_soltar(void)
{
  heap_caps_free(s_foto);
  s_foto = nullptr;
  s_foto_n = 0;
}

static void cru_soltar(void)
{
  if (!s_cru) { return; }
  // The same undo as esp_vfs_fat_sdcard_unmount: only SLOT 0 (slot 1 is
  // the C6's SDIO, and a deinit of the whole host would bring it down).
  tab5_sd_controlador_travar();
  if (s_cru->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) { s_cru->host.deinit_p(s_cru->host.slot); }
  else                                                { s_cru->host.deinit(); }
  tab5_sd_controlador_soltar();
  free(s_cru);
  s_cru = nullptr;
  if (s_ldo) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; }
}

// The card without a file system: LDO, host, slot and card
// initialisation - what esp_vfs_fat_sdmmc_mount does before f_mount. With the lock.
static bool cru_preparar(void)
{
  sd_pwr_ctrl_ldo_config_t ldo_cfg = { };
  ldo_cfg.ldo_chan_id = SD_LDO_CANAL;
  if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo) != ESP_OK) { s_ldo = nullptr; return false; }

  sdmmc_host_t host;
  sdmmc_slot_config_t slot;
  configurar_host(&host, &slot);

  s_cru = (sdmmc_card_t*)calloc(1, sizeof(sdmmc_card_t));
  if (!s_cru) { sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr; return false; }
  tab5_sd_controlador_travar();   // host, slot 0 and card_init: see tab5_sd.hpp
  esp_err_t err = host.init();
  if (err != ESP_OK) {
    tab5_sd_controlador_soltar();
    ESP_LOGE(TAG, "card for flash drive mode: host %s", esp_err_to_name(err));
    free(s_cru); s_cru = nullptr;
    sd_pwr_ctrl_del_on_chip_ldo(s_ldo); s_ldo = nullptr;
    return false;
  }
  err = sdmmc_host_init_slot(host.slot, &slot);
  if (err == ESP_OK) { err = sdmmc_card_init(&host, s_cru); }
  tab5_sd_controlador_soltar();   // before cru_soltar, which locks again (not recursive)
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "card for flash drive mode: %s", esp_err_to_name(err));
    s_cru->host = host;   // the host was initialised: cru_soltar undoes it
    cru_soltar();
    return false;
  }
  s_cru_recuperacoes = 0; s_cru_falhas = 0; s_cru_max_ms = 0;
  return true;
}

tab5_sd_emprestimo_t tab5_sd_emprestar(uint32_t espera_ms)
{
  if (s_emprestado) { return s_cru ? SD_EMPRESTADO : SD_EMPRESTIMO_SEM_CARTAO; }
  // Without the journal, whatever the system wrote during flash drive mode would be lost.
  uint32_t total = 0;
  tab5_reserva_uso(nullptr, &total, nullptr);
  if (total == 0) { return SD_EMPRESTIMO_SEM_DIARIO; }
  s_resumo = { };   // the summary belongs to this loan

  // 1. With the write lock held from start to end of this step, nothing is
  //    written in the middle of it:
  //    - whatever was already handed in (and an old journal) goes to the card NOW,
  //      while it still belongs to the Tab5;
  //    - the card SNAPSHOT (size and date of every file) is taken - it is
  //      how the return recognises what the computer changed;
  //    - montar() starts refusing, and the ports start writing to the journal.
  gravacao_travar(true);
  esvaziar_buffer();
  aplicar_diario();
  trava();
  if (!s_card) { montar_de_verdade(); }   // only for the snapshot (no reference)
  if (s_card) { fotografar(); } else { s_foto_n = 0; }
  s_emprestado = true;
  destrava();
  gravacao_travar(false);

  // 2. Wait for whoever ALREADY had the card to hand it back. The services hold it for
  //    milliseconds (open, write, close); anyone past the timeout is
  //    really using it, and lending now would pull the card out from under them.
  const int64_t limite = esp_timer_get_time() + (int64_t)espera_ms * 1000;
  for (;;) {
    trava();
    const int refs = s_refs;
    destrava();
    if (refs == 0) { break; }
    if (esp_timer_get_time() > limite) {
      ESP_LOGW(TAG, "flash drive: card in use (%d), not lent", refs);
      gravacao_travar(true);
      trava();
      s_emprestado = false;
      foto_soltar();
      // The snapshot mount has no owner: back to the idle timeout.
      if (s_refs == 0 && s_card && s_ocioso) { esp_timer_start_once(s_ocioso, SD_OCIOSO_US); }
      destrava();
      gravacao_travar(false);
      tarefa_cartao_acordar();   // whatever fell into the journal while waiting goes to the card
      return SD_EMPRESTIMO_EM_USO;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // 3. Really unmount - the idle mount too: the Tab5's FAT must
  //    have NOTHING cached when the computer starts writing. Then
  //    the raw card.
  gravacao_travar(true);
  trava();
  if (s_ocioso) { esp_timer_stop(s_ocioso); }
  if (s_card) { desmontar_de_verdade(); }
  const bool ok = cru_preparar();
  if (!ok) { s_emprestado = false; foto_soltar(); }
  destrava();
  gravacao_travar(false);
  if (!ok) { tarefa_cartao_acordar(); return SD_EMPRESTIMO_SEM_CARTAO; }
  ESP_LOGI(TAG, "card lent to the computer: %s (%d file(s) snapshotted)",
           s_ident[0] ? s_ident : "SD", s_foto_n);
  return SD_EMPRESTADO;
}

// ---------------------------------------------------------------------------
// SECTORS OF THE LENT CARD, WITH RECOVERY
// ---------------------------------------------------------------------------
// The IDF function that resets the SDMMC controller. It is private (only in
// sdmmc_internal.h, which is not public), but the symbol is exported.
//
// WHY USE IT: when a data command times out (card too
// slow), the IDF driver just gives up waiting - it does NOT abort the transfer
// in the controller. The controller is left with the previous command stuck (HLE, "hardware locked
// error": "handle_idle_state_events unhandled: 00001000") and from then on every
// command fails with 0x107, forever. As the SAME controller serves slot 1
// (the ESP32-C6's SDIO, the Wi-Fi), Wi-Fi goes down with it. It has happened: flash drive mode
// writing a large file brought down the card and Wi-Fi until restart.
//
// WHAT IT RESETS: only the reset bits of the CTRL register - the controller's
// state machine, internal DMA and FIFO (and clears the stuck start_cmd). It does NOT touch the
// clock divider, the pins or the slot configuration: sdmmc_host_init
// itself calls the reset AFTER setting the divider and BEFORE
// configuring slot and pins, and each transfer re-enables DMA and rewrites the
// size/descriptors (sdmmc_host_dma_prepare). That is why it is safe for slot 1.
// Known limit: it doesn't take the driver's internal lock, so a Wi-Fi
// transfer at exactly the same instant would be hit by the reset (the
// SDIO protocol retries the command); it only runs after a failure, which is rare.
extern "C" esp_err_t sdmmc_host_reset(void);

// How many times an operation recovers before giving up, and how long to wait for the
// card to leave "busy" (programming the write) after the reset.
#define CRU_RECUPERACOES   2
#define CRU_OCUPADO_MAX_MS 10000
#define CRU_OCUPADO_PASSO  20

// Leaves the card ready for the operation to be retried: controller reset,
// card out of busy and, if it doesn't even respond, initialised again.
// With the raw lock held. NO LOGGING: runs inside the USB callbacks.
//
// THE CONTROLLER LOCK IS PER OPERATION, not for the whole loop: the controller is
// shared with the C6's SDIO (Wi-Fi/BLE, see tab5_sd.hpp), and waiting for the card
// to leave busy takes up to CRU_OCUPADO_MAX_MS. Holding the lock for that long
// stalled the C6 for up to 10 s. Between iterations (in vTaskDelay) it is released.
static void cru_recuperar(void)
{
  // The reset and card_init touch the whole controller (see tab5_sd.hpp).
  tab5_sd_controlador_travar();
  sdmmc_host_reset();
  tab5_sd_controlador_soltar();

  const int64_t limite_us = esp_timer_get_time() + (int64_t)CRU_OCUPADO_MAX_MS * 1000;
  for (;;) {
    tab5_sd_controlador_travar();
    const esp_err_t st = sdmmc_get_status(s_cru);
    tab5_sd_controlador_soltar();
    if (st == ESP_OK) { break; }

    if (esp_timer_get_time() >= limite_us) {
      // Didn't respond: the card is reinitialised (same host, already initialised).
      const sdmmc_host_t host = s_cru->host;
      tab5_sd_controlador_travar();
      sdmmc_card_init(&host, s_cru);
      tab5_sd_controlador_soltar();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(CRU_OCUPADO_PASSO));
  }
}

static esp_err_t cru_operar(bool gravar, uint32_t setor, void* buf, uint32_t n)
{
  if (!s_cru_mutex) { return ESP_ERR_INVALID_STATE; }
  xSemaphoreTake(s_cru_mutex, portMAX_DELAY);
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (s_emprestado && s_cru) {
    const int64_t t0 = esp_timer_get_time();
    for (int tentativa = 0;; ++tentativa) {
      err = gravar ? sdmmc_write_sectors(s_cru, buf, setor, n)
                   : sdmmc_read_sectors(s_cru, buf, setor, n);
      if (err == ESP_OK || tentativa >= CRU_RECUPERACOES) { break; }
      ++s_cru_recuperacoes;
      cru_recuperar();
    }
    if (err != ESP_OK) { ++s_cru_falhas; }
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ms > s_cru_max_ms) { s_cru_max_ms = ms; }
  }
  xSemaphoreGive(s_cru_mutex);
  return err;
}

bool tab5_sd_cru_info(uint32_t* setores, uint32_t* tam_setor)
{
  if (!s_cru_mutex) { return false; }
  xSemaphoreTake(s_cru_mutex, portMAX_DELAY);
  const bool ok = s_emprestado && s_cru;
  if (ok) {
    if (setores)   { *setores = (uint32_t)s_cru->csd.capacity; }
    if (tam_setor) { *tam_setor = (uint32_t)s_cru->csd.sector_size; }
  }
  xSemaphoreGive(s_cru_mutex);
  return ok;
}

esp_err_t tab5_sd_cru_ler(uint32_t setor, void* buf, uint32_t n)
{
  return cru_operar(false, setor, buf, n);
}

esp_err_t tab5_sd_cru_gravar(uint32_t setor, const void* buf, uint32_t n)
{
  return cru_operar(true, setor, const_cast<void*>(buf), n);
}

void tab5_sd_cru_estat(tab5_sd_cru_estat_t* e)
{
  e->recuperacoes = s_cru_recuperacoes;
  e->falhas = s_cru_falhas;
  e->max_ms = s_cru_max_ms;
}

void tab5_sd_devolver(void)
{
  // The write lock BEFORE the card one (the order for all ports): the
  // replace reads the snapshot with it held, and the snapshot goes away here together with the
  // loan.
  gravacao_travar(true);
  trava();
  if (!s_emprestado) { destrava(); gravacao_travar(false); return; }
  // The raw lock: never release the card with a sector operation in progress.
  xSemaphoreTake(s_cru_mutex, portMAX_DELAY);
  cru_soltar();
  s_emprestado = false;
  xSemaphoreGive(s_cru_mutex);
  // The next mount is normal: no inherited "no card" backoff.
  s_ultima_falhou = false;
  s_espera_ms = 0;
  foto_soltar();
  destrava();
  gravacao_travar(false);
  ESP_LOGI(TAG, "card returned to the Tab5");
  tarefa_cartao_acordar();   // applies the journal (vTaskCartao)
}

bool tab5_sd_emprestado(void) { return s_emprestado; }

// App name -> FAT-safe folder name. Replaces anything troublesome with
// '_' instead of refusing: better to write to "Mapa_Azimutal" than not to write.
static void nome_seguro(const char* origem, char* destino, size_t tam)
{
  size_t j = 0;
  for (size_t i = 0; origem[i] && j + 1 < tam; ++i) {
    const char c = origem[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9') || c == '-' || c == '_';
    destino[j++] = ok ? c : '_';
  }
  destino[j] = '\0';
  if (j == 0) { snprintf(destino, tam, "app"); }
}

static bool pasta_de(const char* app, char* out, size_t out_tam, bool criar)
{
  if (!app || !out) { return false; }

  // SUBFOLDERS: "Sistema/logs" becomes apps/Sistema/logs. Each segment
  // between slashes goes through nome_seguro and each level is created if missing.
  size_t n = (size_t)snprintf(out, out_tam, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p) {
    const char* fim = strchr(p, '/');
    const size_t tam = fim ? (size_t)(fim - p) : strlen(p);
    if (tam > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)tam, p);
      nome_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < out_tam ? out_tam - n : 0, "/%s", seguro);
      if (n >= out_tam) { return false; }

      struct stat st;
      if (!criar) {
        // just the path
      } else if (stat(out, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) { return false; }
      } else if (mkdir(out, 0777) != 0) {
        ESP_LOGE(TAG, "could not create %s", out);
        return false;
      }
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  return true;
}

bool tab5_sd_pasta(const char* app, char* out, size_t out_tam)
{
  return s_card && pasta_de(app, out, out_tam, true);
}

bool tab5_sd_pasta_caminho(const char* app, char* out, size_t out_tam)
{
  return pasta_de(app, out, out_tam, false);
}

static FILE* abrir_cru(const char* app, const char* arquivo, const char* modo);

FILE* tab5_sd_abrir(const char* app, const char* arquivo, const char* modo)
{
  // Reading (or writing outside the ports) must see everything already
  // handed in: the writer's pending data and a flash drive session's journal.
  tab5_sd_gravar_pendentes();
  return abrir_cru(app, arquivo, modo);
}

// INTERRUPTED SWAP. Replacing a file on FAT means: write "<nome>.tmp"
// in full (and fsync), delete the old one, rename the .tmp - FAT doesn't replace
// in place. A power cut between delete and rename leaves the file MISSING and
// the new content, complete, in the .tmp. Whoever reads next finds this and completes the
// swap. (A .tmp with the file still there is a swap that hadn't started deleting:
// it stays, and the next swap overwrites it.)
static void completar_troca(const char* caminho)
{
  struct stat st;
  if (stat(caminho, &st) == 0) { return; }
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return; }
  if (stat(tmp, &st) != 0) { return; }
  if (rename(tmp, caminho) == 0) { ESP_LOGW(TAG, "%s: swap interrupted by a power cut - completed", caminho); }
}

static FILE* abrir_cru(const char* app, const char* arquivo, const char* modo)
{
  char pasta[96];
  if (!tab5_sd_pasta(app, pasta, sizeof(pasta))) { return nullptr; }

  char caminho[160];
  snprintf(caminho, sizeof(caminho), "%s/%s", pasta, arquivo);

  completar_troca(caminho);
  FILE* f = fopen(caminho, modo);
  if (!f)
  {
    // NOT EVERY FAILURE IS AN ERROR, and the service is the one that must know the difference.
    //
    // Opening for READ a file that doesn't exist yet is a legitimate and
    // predictable result - it happens every time an app reads something for the first time (the
    // history of a new Meshtastic conversation, for example). Logging
    // that as an ERROR fills the log with false alarms, and a log that cries wolf teaches
    // people to ignore logs.
    //
    // A failure to OPEN FOR WRITE, though, or any other errno on read
    // (permission, corrupted card, invalid path), is a real problem and
    // stays at ERROR.
    const bool so_leitura = (modo[0] == 'r');
    if (so_leitura && errno == ENOENT) {
      ESP_LOGD(TAG, "'%s' does not exist yet", caminho);
    } else {
      ESP_LOGE(TAG, "fopen('%s','%s') failed: %s", caminho, modo, strerror(errno));
    }
  }
  return f;
}


bool tab5_sd_espaco(uint64_t* total, uint64_t* livre)
{
  if (!s_card) { return false; }
  // ESP's newlib has no statvfs; the IDF exposes this through the FAT VFS itself.
  return esp_vfs_fat_info(TAB5_SD_RAIZ, total, livre) == ESP_OK;
}

long tab5_sd_tamanho(FILE* f)
{
  if (fseek(f, 0, SEEK_END) != 0) { return -1; }
  const long n = ftell(f);
  if (fseek(f, 0, SEEK_SET) != 0) { return -1; }
  return n;
}

// ===========================================================================
// THE WRITER (see the .hpp)
// ===========================================================================
#include "freertos/task.h"
#include "tab5_donos.hpp"
#include <atomic>

#define ANEXO_BUF      (16 * 1024)   // each of the two buffers, in PSRAM
#define ANEXO_PRAZO_MS 5000          // anything non-urgent waits at most this long
#define ANEXO_APP      32
#define ANEXO_ARQ      40

typedef struct {
  uint16_t tam;          // whole record, header included
  uint16_t n_cab, n_txt;
  char     app[ANEXO_APP];
  char     arq[ANEXO_ARQ];
  // followed by n_cab bytes of header and n_txt of text
} anexo_t;

// TWO buffers: whoever hands in writes to the active one; the write swaps and writes the
// other WITHOUT the lock - handing in never waits for the card.
static uint8_t*          s_buf[2]   = { nullptr, nullptr };
static int               s_ativo    = 0;
static size_t            s_usado    = 0;
static int64_t           s_desde_us = 0;      // when the oldest pending one arrived
static SemaphoreHandle_t s_anexo    = nullptr; // the active buffer
static SemaphoreHandle_t s_gravando = nullptr; // one write at a time
static TaskHandle_t      s_tarefa   = nullptr;
static std::atomic<uint32_t> s_gravadas = 0, s_perdidas = 0;

bool tab5_sd_anexar(const char* app, const char* arquivo, const char* texto,
                    const char* cabecalho, bool urgente)
{
  if (!s_anexo || !app || !arquivo || !texto) { return false; }
  const size_t nc = cabecalho ? strlen(cabecalho) : 0;
  const size_t nt = strlen(texto);
  const size_t tam = sizeof(anexo_t) + nc + nt;
  if (tam > ANEXO_BUF / 2) { ++s_perdidas; return false; }

  xSemaphoreTake(s_anexo, portMAX_DELAY);   // only memcpy in here
  const bool cabe = (s_usado + tam <= ANEXO_BUF);
  if (cabe) {
    anexo_t* a = (anexo_t*)(s_buf[s_ativo] + s_usado);
    a->tam = (uint16_t)tam; a->n_cab = (uint16_t)nc; a->n_txt = (uint16_t)nt;
    snprintf(a->app, sizeof(a->app), "%s", app);
    snprintf(a->arq, sizeof(a->arq), "%s", arquivo);
    uint8_t* p = (uint8_t*)(a + 1);
    if (nc) { memcpy(p, cabecalho, nc); }
    memcpy(p + nc, texto, nt);
    if (s_usado == 0) { s_desde_us = esp_timer_get_time(); }
    s_usado += tam;
  }
  const bool metade = (s_usado > ANEXO_BUF / 2);
  xSemaphoreGive(s_anexo);

  if (!cabe) { ++s_perdidas; }
  if ((urgente || metade) && s_tarefa) { xTaskNotifyGive(s_tarefa); }
  return cabe;
}

// ---------------------------------------------------------------------------
// THE DESTINATION OF EACH WRITE
// ---------------------------------------------------------------------------
// Card, or the journal in flash (hal/tab5_reserva) - ALWAYS decided with the
// write lock held, the same one used by the loan and by applying the journal.
// While the journal has anything pending, EVERYTHING goes to it, even with the
// card already back: writing straight to the card would jump ahead of what
// happened earlier and is waiting.
static bool para_o_diario(void) { return s_emprestado || tab5_reserva_pendente(); }

static std::atomic<uint32_t> s_diario_perdidas = 0;   // did not fit in the journal

static bool diario_acrescentar(const char* app, const char* arq,
                               const void* cab, size_t n_cab, const void* dados, size_t n)
{
  reserva_reg_t r = { };
  r.tipo = RESERVA_ACRESCENTAR;
  r.app = app; r.arquivo = arq;
  r.cabecalho = cab; r.n_cabecalho = (uint16_t)n_cab;
  r.dados = dados; r.n_dados = (uint32_t)n;
  if (tab5_reserva_gravar(&r)) { return true; }
  ++s_diario_perdidas;
  return false;
}

// The path on the card for app/file, WITHOUT creating the folder (for the snapshot and hash).
static void caminho_de(const char* app, const char* arq, char* out, size_t tam)
{
  size_t n = (size_t)snprintf(out, tam, "%s", TAB5_SD_APPS);
  const char* p = app;
  while (*p && n < tam) {
    const char* fim = strchr(p, '/');
    const size_t k = fim ? (size_t)(fim - p) : strlen(p);
    if (k > 0) {
      char trecho[32], seguro[32];
      snprintf(trecho, sizeof(trecho), "%.*s", (int)k, p);
      nome_seguro(trecho, seguro, sizeof(seguro));
      n += (size_t)snprintf(out + n, n < tam ? tam - n : 0, "/%s", seguro);
    }
    if (!fim) { break; }
    p = fim + 1;
  }
  if (n < tam) { snprintf(out + n, tam - n, "/%s", arq); }
}

// Writes an already-swapped buffer: each file opened ONCE, its lines in
// arrival order. Called with s_gravando held.
static void gravar_buffer(uint8_t* b, size_t n)
{
  if (n == 0) { return; }
  if (para_o_diario()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) {
      anexo_t* a = (anexo_t*)(b + o);
      const uint8_t* p = (const uint8_t*)(a + 1);
      if (diario_acrescentar(a->app, a->arq, a->n_cab ? p : nullptr, a->n_cab, p + a->n_cab, a->n_txt)) {
        ++s_gravadas;
      } else {
        ++s_perdidas;
      }
    }
    return;
  }
  if (!tab5_sd_montar()) {
    for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) { ++s_perdidas; }
    return;
  }
  // Marks the written ones by zeroing 'app' (the record stays in place, it just
  // stops counting) - scans again for the next distinct file.
  for (size_t o = 0; o < n; o += ((anexo_t*)(b + o))->tam) {
    anexo_t* a = (anexo_t*)(b + o);
    if (!a->app[0]) { continue; }
    char app[ANEXO_APP], arq[ANEXO_ARQ];
    memcpy(app, a->app, sizeof(app));
    memcpy(arq, a->arq, sizeof(arq));
    FILE* f = abrir_cru(app, arq, "a");
    bool cabecalho_feito = false;
    for (size_t k = o; k < n; k += ((anexo_t*)(b + k))->tam) {
      anexo_t* r = (anexo_t*)(b + k);
      if (!r->app[0] || strcmp(r->app, app) != 0 || strcmp(r->arq, arq) != 0) { continue; }
      const uint8_t* p = (const uint8_t*)(r + 1);
      if (f) {
        if (!cabecalho_feito) {
          cabecalho_feito = true;
          if (r->n_cab && ftell(f) == 0) { fwrite(p, 1, r->n_cab, f); }
        }
        fwrite(p + r->n_cab, 1, r->n_txt, f);
        ++s_gravadas;
      } else {
        ++s_perdidas;
      }
      r->app[0] = 0;
    }
    if (f) { fclose(f); }
  }
  tab5_sd_desmontar();
}

// Swaps the buffer and writes the full one. With s_gravando held.
static void esvaziar_buffer(void)
{
  if (!s_anexo) { return; }
  xSemaphoreTake(s_anexo, portMAX_DELAY);
  uint8_t* cheio = s_buf[s_ativo];
  const size_t n = s_usado;
  s_ativo = 1 - s_ativo;
  s_usado = 0;
  xSemaphoreGive(s_anexo);
  gravar_buffer(cheio, n);
}

static void gravar_tudo(void)
{
  if (!s_gravando) { return; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  xSemaphoreGive(s_gravando);
}

// ---------------------------------------------------------------------------
// APPLYING THE JOURNAL TO THE CARD
// ---------------------------------------------------------------------------
// Files THIS application pass has already written: they changed because of it, not the
// computer - a second REPLACE of the same file is not a conflict.
#define TOCADOS_MAX 64
static uint32_t s_tocados[TOCADOS_MAX];
static int      s_n_tocados = 0;
static bool tocado(uint32_t h)
{
  for (int i = 0; i < s_n_tocados; ++i) { if (s_tocados[i] == h) { return true; } }
  return false;
}
static void tocar(uint32_t h) { if (!tocado(h) && s_n_tocados < TOCADOS_MAX) { s_tocados[s_n_tocados++] = h; } }

// Replaces the whole file on the card, never leaving half a file: writes
// "<nome>.tmp", and only then swaps it for the old one. SIGILOSO zeroes the old one before
// deleting - on FAT deleting only marks the sectors as free, and the old content
// would be readable by any recovery program. Card mounted.
static bool substituir_no_cartao(const char* caminho, const void* dados, size_t n, uint8_t flags)
{
  char tmp[200];
  if (snprintf(tmp, sizeof(tmp), "%s.tmp", caminho) >= (int)sizeof(tmp)) { return false; }
  FILE* f = fopen(tmp, "wb");
  if (!f) { ESP_LOGE(TAG, "fopen('%s') failed: %s", tmp, strerror(errno)); return false; }
  bool ok = (n == 0) || fwrite(dados, 1, n, f) == n;
  ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (fclose(f) != 0 || !ok) { unlink(tmp); return false; }
  if (flags & TAB5_SD_SIGILOSO) {
    FILE* velho = fopen(caminho, "r+");
    if (velho) {
      long falta = tab5_sd_tamanho(velho);
      static const char zeros[64] = { };
      while (falta > 0) {
        const size_t k = falta > (long)sizeof(zeros) ? sizeof(zeros) : (size_t)falta;
        if (fwrite(zeros, 1, k, velho) != k) { break; }
        falta -= (long)k;
      }
      fflush(velho);
      fsync(fileno(velho));
      fclose(velho);
    }
  }
  unlink(caminho);                          // FAT doesn't replace in place: remove the old one...
  if (rename(tmp, caminho) != 0) {          // ...and put the new one in its place
    ESP_LOGE(TAG, "could not replace %s with the new one", caminho);
    return false;
  }
  return true;
}

// One journal record onto the card (mounted). true = it is already there
// durably (the journal only marks it applied after this).
static bool aplicar_um(const reserva_reg_t* r, void*)
{
  char pasta[96];
  if (!tab5_sd_pasta(r->app, pasta, sizeof(pasta))) { return false; }
  char caminho[160];
  if (snprintf(caminho, sizeof(caminho), "%s/%s", pasta, r->arquivo) >= (int)sizeof(caminho)) {
    s_resumo.descartadas++;   // a name that doesn't fit never will: don't block the rest
    return true;
  }
  const uint32_t h = hash_texto(caminho);

  if (r->tipo == RESERVA_ACRESCENTAR) {
    FILE* f = fopen(caminho, "a");
    if (!f) { return false; }
    // Not applied = nothing written: if it fails midway (card full), whatever
    // went in is cut back - otherwise the retry would repeat the chunk.
    const long antes = ftell(f);
    bool ok = antes >= 0;
    if (ok && r->cabecalho && r->n_cabecalho && antes == 0) { ok = fwrite(r->cabecalho, 1, r->n_cabecalho, f) == r->n_cabecalho; }
    ok = ok && fwrite(r->dados, 1, r->n_dados, f) == r->n_dados;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (!ok && antes >= 0) { fflush(f); ftruncate(fileno(f), antes); }
    fclose(f);
    if (!ok) { return false; }
    tocar(h);
    s_resumo.aplicadas++;
    return true;
  }

  // REPLACE. Did the computer touch the file while it had the
  // card (doesn't match the snapshot)? Then its version stays, and the Tab5's goes alongside
  // as "<nome>.tab5" - nothing is lost silently, and the person decides.
  completar_troca(caminho);
  struct stat st;
  const bool existe = stat(caminho, &st) == 0;
  const bool igual = tocado(h)
                  || (existe == r->base_existia
                      && (!existe || ((uint32_t)st.st_size == r->base_tamanho
                                      && (uint32_t)st.st_mtime == r->base_data)));
  if (igual) {
    if (!substituir_no_cartao(caminho, r->dados, r->n_dados, r->flags)) { return false; }
    tocar(h);
    s_resumo.aplicadas++;
    return true;
  }
  char lado[176];
  snprintf(lado, sizeof(lado), "%s.tab5", caminho);
  if (!substituir_no_cartao(lado, r->dados, r->n_dados, r->flags)) { return false; }
  ESP_LOGW(TAG, "%s changed on the computer: the Tab5's version was saved as %s.tab5", caminho, r->arquivo);
  s_resumo.conflitos++;
  return true;
}

// With s_gravando held and the card belonging to the Tab5.
static void aplicar_diario(void)
{
  if (s_emprestado || !tab5_reserva_pendente()) { return; }
  if (!tab5_sd_montar()) { return; }   // no card: left for the next round
  s_n_tocados = 0;
  const int n = tab5_reserva_aplicar(aplicar_um, nullptr);
  tab5_sd_desmontar();
  if (n < 0) { ESP_LOGE(TAG, "journal unreadable"); return; }
  if (!tab5_reserva_pendente()) {
    s_resumo.completo = true;
    ESP_LOGI(TAG, "journal applied: %u write(s), %u conflict(s)",
             (unsigned)s_resumo.aplicadas, (unsigned)s_resumo.conflitos);
  }
}

void tab5_sd_gravar_pendentes(void)
{
  if (!s_anexo || !s_gravando) { return; }
  // BARRIER: waits for the write ALREADY IN PROGRESS (the "cartao" task
  // swaps the buffer BEFORE writing to the file - without waiting, anyone calling
  // here in that interval would read without the line being written), writes the
  // pending data and applies a flash drive session's journal, in that order: whoever reads
  // next sees everything already handed in.
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  aplicar_diario();
  xSemaphoreGive(s_gravando);
}

bool tab5_sd_acrescentar(const char* app, const char* arquivo, const void* dados, size_t n)
{
  if (!s_gravando || !app || !arquivo || (!dados && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  bool ok;
  if (para_o_diario()) {
    ok = diario_acrescentar(app, arquivo, nullptr, 0, dados, n);
  } else if (tab5_sd_montar()) {
    FILE* f = abrir_cru(app, arquivo, "a");
    ok = f && (n == 0 || fwrite(dados, 1, n, f) == n);
    if (f) { fclose(f); }
    tab5_sd_desmontar();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

bool tab5_sd_substituir(const char* app, const char* arquivo, const void* dados, size_t n, uint8_t flags)
{
  if (!s_gravando || !app || !arquivo || (!dados && n)) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  // Whatever was handed in to APPEND before this swap goes before it.
  esvaziar_buffer();
  bool ok;
  if (para_o_diario()) {
    char caminho[160];
    caminho_de(app, arquivo, caminho, sizeof(caminho));
    const foto_t* f = s_emprestado ? foto_de(caminho) : nullptr;
    reserva_reg_t r = { };
    r.tipo = RESERVA_SUBSTITUIR; r.flags = flags;
    r.app = app; r.arquivo = arquivo;
    r.dados = dados; r.n_dados = (uint32_t)n;
    r.base_existia = f != nullptr;
    r.base_tamanho = f ? f->tam : 0;
    r.base_data    = f ? f->data : 0;
    ok = tab5_reserva_gravar(&r);
    if (!ok) { ++s_diario_perdidas; }
  } else if (tab5_sd_montar()) {
    char pasta[96], caminho[160];
    ok = tab5_sd_pasta(app, pasta, sizeof(pasta))
      && snprintf(caminho, sizeof(caminho), "%s/%s", pasta, arquivo) < (int)sizeof(caminho)
      && substituir_no_cartao(caminho, dados, n, flags);
    tab5_sd_desmontar();
  } else {
    ok = false;
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

bool tab5_sd_apagar(const char* app, const char* arquivo)
{
  if (!s_gravando || !app || !arquivo) { return false; }
  xSemaphoreTake(s_gravando, portMAX_DELAY);
  esvaziar_buffer();
  bool ok = false;
  if (!para_o_diario() && tab5_sd_montar()) {
    char caminho[160];
    caminho_de(app, arquivo, caminho, sizeof(caminho));
    ok = remove(caminho) == 0;
    tab5_sd_desmontar();
  }
  xSemaphoreGive(s_gravando);
  return ok;
}

void tab5_sd_resumo(tab5_sd_resumo_t* r)
{
  *r = s_resumo;
  r->no_diario  = 0;
  tab5_reserva_uso(&r->diario_usado, &r->diario_total, &r->no_diario);
  r->perdidas = s_diario_perdidas;
}

void tab5_sd_anexos_estado(uint32_t* pendentes, uint32_t* gravadas, uint32_t* perdidas)
{
  if (pendentes) { *pendentes = (uint32_t)s_usado; }
  if (gravadas)  { *gravadas  = s_gravadas; }
  if (perdidas)  { *perdidas  = s_perdidas; }
}

// 8, not 4: with the journal and the Mesh node's networks, there are already FIVE
// riders (log, registro, mesh, mesh_diario, mesh_wifi_no). With 4, the fifth to
// register was left out - "riders exhausted" at boot, and what it only does
// as a rider (writing the node's network list, for example) never happened.
#define CARONAS_MAX 8
static tab5_sd_carona_t s_caronas[CARONAS_MAX] = { };

bool tab5_sd_carona(tab5_sd_carona_t fn)
{
  for (int i = 0; i < CARONAS_MAX; ++i) {
    if (s_caronas[i] == fn) { return true; }
    if (!s_caronas[i]) { s_caronas[i] = fn; return true; }
  }
  ESP_LOGE(TAG, "riders exhausted (%d) - increase CARONAS_MAX", CARONAS_MAX);
  return false;
}

static void vTaskCartao(void*)
{
  for (;;) {
    // THE ROUND: 1 s when awake, 5 s when idle. The urgent stuff (conversation
    // message) and the buffer passing half full wake it at once (notification);
    // the rest already waited up to ANEXO_PRAZO_MS. When idle each round pulls the P4
    // out of light sleep, and the riders (log every 60 s, Mesh, registro) don't
    // need more than this.
    const uint32_t volta_ms = tab5_power_em_repouso() ? 5000 : 1000;
    const bool pedido = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(volta_ms)) > 0;
    xSemaphoreTake(s_anexo, portMAX_DELAY);
    const bool ha = (s_usado > 0);
    const bool venceu = ha && (esp_timer_get_time() - s_desde_us) >= (int64_t)ANEXO_PRAZO_MS * 1000;
    xSemaphoreGive(s_anexo);
    if (ha && (pedido || venceu)) { gravar_tudo(); }
    // A flash drive session's journal goes to the card as soon as it is
    // back (or after a restart, when the card appears).
    // Failed without making any progress (card full, no card): wait before the
    // next attempt, instead of hammering the card every second.
    static int64_t proxima_us = 0;
    if (tab5_reserva_pendente() && !s_emprestado && (pedido || esp_timer_get_time() >= proxima_us)) {
      xSemaphoreTake(s_gravando, portMAX_DELAY);
      esvaziar_buffer();
      const uint32_t antes = s_resumo.aplicadas + s_resumo.conflitos;
      aplicar_diario();
      const bool andou = s_resumo.aplicadas + s_resumo.conflitos != antes;
      xSemaphoreGive(s_gravando);
      proxima_us = (andou || !tab5_reserva_pendente()) ? 0 : esp_timer_get_time() + 30LL * 1000 * 1000;
    }
    // The riders write through the ports (append/replace): they keep
    // working with the card lent - the destination is decided inside.
    for (int i = 0; i < CARONAS_MAX && s_caronas[i]; ++i) { s_caronas[i](); }
  }
}

static void gravacao_travar(bool travar)
{
  if (!s_gravando) { return; }
  if (travar) { xSemaphoreTake(s_gravando, portMAX_DELAY); }
  else        { xSemaphoreGive(s_gravando); }
}

static void tarefa_cartao_acordar(void)
{
  if (s_tarefa) { xTaskNotifyGive(s_tarefa); }
}

// Called by tab5_sd_iniciar (same moment: before there is concurrency).
static void gravador_iniciar(void)
{
  if (s_anexo) { return; }
  s_buf[0] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_buf[1] = (uint8_t*)heap_caps_malloc(ANEXO_BUF, MALLOC_CAP_SPIRAM);
  s_gravando = xSemaphoreCreateMutex();
  SemaphoreHandle_t anexo = xSemaphoreCreateMutex();
  if (!s_buf[0] || !s_buf[1] || !s_gravando || !anexo) {
    ESP_LOGE(TAG, "writer out of memory - attachments disabled");
    return;
  }
  // 6 KB: the task MOUNTS the card, and f_mount goes deep into the stack.
  xTaskCreatePinnedToCore(vTaskCartao, "cartao", 6144, nullptr, 2, &s_tarefa, tskNO_AFFINITY);
  tab5_dono_registrar(s_tarefa, DONO_SERVICO, "Cartao");
  s_anexo = anexo;   // last: tab5_sd_anexar only accepts once everything is ready
}
