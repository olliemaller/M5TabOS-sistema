#include "tab5_usb.hpp"
#include "idioma.h"
#include "tab5_volumes.hpp"
#include "notificacao.h"
#include "ajustes.h"
#include "app.h"
#include "tab5_mutex.hpp"
#include "tab5_power.hpp"   // tab5_power_usb_a

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"   // ff_diskio_get_drive - the FatFs drive, for the label
#include "ff.h"

#include <atomic>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

static const char* TAG = "usb";

// How long readers have to let go of a flash drive that was yanked out.
#define SAIDA_ESPERA_MS  3000

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
static Trava s_trava;   // s_est
static tab5_usb_estado_t s_est = { };
static char              s_nome[40] = "";      // tab5_usb_nome (under the lock)

static std::atomic<bool> s_subiu{false};       // the USB stack has been installed
static std::atomic<bool> s_presente{false};    // /usb mounted and usable
static std::atomic<int>  s_refs{0};

static QueueHandle_t            s_fila = nullptr;
static usb_host_client_handle_t s_cliente = nullptr;
static usb_device_handle_t      s_aparelho = nullptr;   // opened by us (only to read)
static msc_host_device_handle_t s_disco = nullptr;
static msc_host_vfs_handle_t    s_vfs = nullptr;
static int                      s_notif = -1;

// EJECT: the requester waits for the "usb" task to do it (it owns the disk -
// the arrive and leave events go through it, and ejecting in the middle of one
// would unmount twice). One request at a time.
static SemaphoreHandle_t        s_ejetar_vez = nullptr;
static SemaphoreHandle_t        s_ejetar_fim = nullptr;
static uint32_t                 s_ejetar_espera_ms = 0;
static tab5_usb_ejetar_t        s_ejetar_res = USB_EJETAR_SEM_PENDRIVE;

// Events arrive through two doors (our USB Host Library client and the MSC
// driver); the "usb" task handles them, in order.
typedef enum { EV_APARELHO, EV_APARELHO_FORA, EV_DISCO, EV_DISCO_FORA, EV_EJETAR } ev_tipo_t;
typedef struct {
  uint8_t tipo;
  uint8_t endereco;
  void*   handle;   // usb_device_handle_t or msc_host_device_handle_t
} ev_t;

static void anotar_erro(const char* fmt, const char* detalhe)
{
  s_trava.travar();
  snprintf(s_est.erro, sizeof(s_est.erro), fmt, detalhe ? detalhe : "");
  s_trava.destravar();
}

// Text from USB descriptors (UTF-16) and from the MSC driver (wchar_t, 32 bits
// here) -> UTF-8. Trailing spaces (descriptors come padded) are removed.
static void utf8_por(uint32_t c, char* out, size_t tam, size_t* k)
{
  char b[3]; size_t m;
  if      (c < 0x80)    { b[0] = (char)c; m = 1; }
  else if (c < 0x800)   { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); m = 2; }
  else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                          b[2] = (char)(0x80 | (c & 0x3F)); m = 3; }
  else { return; }
  if (*k + m >= tam) { return; }
  memcpy(out + *k, b, m);
  *k += m;
}
static void utf8_fim(char* out, size_t k)
{
  out[k] = '\0';
  while (k && out[k - 1] == ' ') { out[--k] = '\0'; }
}
static void utf8_de_utf16(const uint16_t* s, int n, char* out, size_t tam)
{
  size_t k = 0;
  for (int i = 0; i < n && s[i]; ++i) { utf8_por(s[i], out, tam, &k); }
  utf8_fim(out, k);
}
static void utf8_de_wchar(const wchar_t* s, int n, char* out, size_t tam)
{
  size_t k = 0;
  for (int i = 0; i < n && s[i]; ++i) { utf8_por((uint32_t)s[i], out, tam, &k); }
  utf8_fim(out, k);
}

// ---------------------------------------------------------------------------
// THE EVENTS (they arrive on other tasks: they only queue)
// ---------------------------------------------------------------------------
static void enfileirar(ev_tipo_t t, uint8_t endereco, void* h)
{
  const ev_t ev = { (uint8_t)t, endereco, h };
  if (!s_fila || xQueueSend(s_fila, &ev, 0) != pdTRUE) { ESP_LOGE(TAG, "queue full: event %d lost", (int)t); }
  if (s_cliente) { usb_host_client_unblock(s_cliente); }   // wakes the "usb" task
}

// Our client: only to know there is a device and what it is (speed, name).
// It claims no interface at all - the MSC driver is the one that uses it.
static void cliente_evento(const usb_host_client_event_msg_t* m, void*)
{
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) { enfileirar(EV_APARELHO, m->new_dev.address, nullptr); }
  else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) { enfileirar(EV_APARELHO_FORA, 0, m->dev_gone.dev_hdl); }
}

static void msc_evento(const msc_host_event_t* e, void*)
{
  if (e->event == msc_host_event_t::MSC_DEVICE_CONNECTED) { enfileirar(EV_DISCO, e->device.address, nullptr); }
  else if (e->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) { enfileirar(EV_DISCO_FORA, 0, e->device.handle); }
}

// ---------------------------------------------------------------------------
// THE DEVICE (any device)
// ---------------------------------------------------------------------------
static void aparelho_chegou(uint8_t endereco)
{
  if (s_aparelho) { return; }   // no hub: only one at a time
  if (usb_host_device_open(s_cliente, endereco, &s_aparelho) != ESP_OK) {
    s_aparelho = nullptr;
    anotar_erro(tr("um aparelho foi visto e nao abriu%s"), "");
    return;
  }
  usb_device_info_t info = { };
  const usb_device_desc_t* desc = nullptr;
  usb_host_device_info(s_aparelho, &info);
  usb_host_get_device_descriptor(s_aparelho, &desc);
  s_trava.travar();
  s_est.aparelho = true;
  ++s_est.conexoes;
  s_est.velocidade = (uint8_t)info.speed;
  s_est.vid = desc ? desc->idVendor : 0;
  s_est.pid = desc ? desc->idProduct : 0;
  s_est.classe = desc ? desc->bDeviceClass : 0;
  s_est.produto[0] = '\0';
  // A malformed descriptor shorter than its own header (bLength < 2) gave a
  // negative character count.
  if (info.str_desc_product && info.str_desc_product->bLength >= 2) {
    const usb_str_desc_t* d = info.str_desc_product;
    utf8_de_utf16(d->wData, (d->bLength - 2) / 2, s_est.produto, sizeof(s_est.produto));
  }
  s_est.erro[0] = '\0';
  s_trava.destravar();
  // With a device on the connector, the bus cannot sleep: in light sleep the
  // controller stops sending the "I'm here" every millisecond and the
  // device switches itself off.
  sistema_recurso(RECURSO_USB_HOST, "USB-A", true);
  static const char* const VEL[] = { "baixa (1,5 Mb/s)", "cheia (12 Mb/s)", "ALTA (480 Mb/s)" };
  ESP_LOGI(TAG, "device %04x:%04x '%s', speed %s", s_est.vid, s_est.pid, s_est.produto,
           info.speed <= USB_SPEED_HIGH ? VEL[info.speed] : "?");
}

static void aparelho_saiu(usb_device_handle_t h)
{
  if (h && h == s_aparelho) {
    usb_host_device_close(s_cliente, s_aparelho);
    s_aparelho = nullptr;
  }
  s_trava.travar();
  s_est.aparelho = false;
  s_trava.destravar();
  sistema_recurso(RECURSO_USB_HOST, "USB-A", false);
  ESP_LOGI(TAG, "device removed");
}

// ---------------------------------------------------------------------------
// THE DISK (MSC class)
// ---------------------------------------------------------------------------
static void avisar_barra(notif_nivel_t nivel, const char* resumo)
{
  if (s_notif < 0) {
    // Tapping the notice opens Files: that is where the flash drive shows in full
    // (its music is still in Music, in the Folders list).
    s_notif = notif_registrar("USB-A");
    if (s_notif >= 0) { notif_associar_app(s_notif, "Arquivos"); }
  }
  if (s_notif < 0) { return; }
  if (resumo) { notif_publicar(s_notif, 1, nivel, resumo); }
  else        { notif_limpar(s_notif); }
}

static void disco_chegou(uint8_t endereco)
{
  if (s_disco) { return; }
  esp_err_t r = msc_host_install_device(endereco, &s_disco);
  if (r != ESP_OK) {
    s_disco = nullptr;
    anotar_erro(tr("o disco nao respondeu (%s)"), esp_err_to_name(r));
    ESP_LOGW(TAG, "msc_host_install_device: %s", esp_err_to_name(r));
    return;
  }
  msc_host_device_info_t info = { };
  msc_host_get_device_info(s_disco, &info);
  char produto[40];
  utf8_de_wchar(info.iProduct, MSC_STR_DESC_SIZE, produto, sizeof(produto));

  // The drive number FatFs will give it (the first free one - the same
  // calculation msc_host_vfs_register does inside): only to read the label.
  BYTE pdrv = 0xFF;
  ff_diskio_get_drive(&pdrv);
  esp_vfs_fat_mount_config_t mc = VFS_FAT_MOUNT_DEFAULT_CONFIG();
  mc.max_files = 8;
  r = msc_host_vfs_register(s_disco, TAB5_USB_RAIZ, &mc, &s_vfs);

  s_trava.travar();
  s_est.disco = true;
  s_est.setores = info.sector_count;
  s_est.setor_bytes = info.sector_size;
  s_est.montado = (r == ESP_OK);
  if (r != ESP_OK) {
    // NTFS (formatted on Windows), APFS/HFS+ (on the Mac), or FAT with a GPT
    // partition table (the Disk Utility default) - FatFs reads MBR.
    snprintf(s_est.erro, sizeof(s_est.erro), tr("nao montou (%s) - formate em FAT32 ou exFAT, MBR"), esp_err_to_name(r));
  }
  s_trava.destravar();

  if (r != ESP_OK) {
    s_vfs = nullptr;
    ESP_LOGW(TAG, "flash drive '%s' did not mount: %s (NTFS, Mac, or GPT partition?)", produto, esp_err_to_name(r));
    avisar_barra(NOTIF_ATENCAO, tr("Pendrive não lido: use FAT32/exFAT"));
    return;
  }

  char rotulo[40] = "";
  if (pdrv != 0xFF) {
    const char drv[3] = { (char)('0' + pdrv), ':', 0 };
    if (f_getlabel(drv, rotulo, nullptr) != FR_OK) { rotulo[0] = '\0'; }
  }
  s_trava.travar();
  snprintf(s_nome, sizeof(s_nome), "%s", rotulo[0] ? rotulo : (produto[0] ? produto : "Pendrive"));
  s_trava.destravar();
  s_presente = true;
  const uint64_t mb = (uint64_t)info.sector_count * info.sector_size / (1024 * 1024);
  ESP_LOGI(TAG, "flash drive '%s' mounted at " TAB5_USB_RAIZ " (%llu MB)", s_nome, (unsigned long long)mb);
  char resumo[NOTIF_RESUMO];
  snprintf(resumo, sizeof(resumo), "Pendrive %s", s_nome);
  avisar_barra(NOTIF_ESTADO, resumo);
  tab5_volumes_mudou();
}

static void disco_saiu(void)
{
  if (!s_disco) { return; }
  const bool estava = s_presente.exchange(false);
  if (estava) { tab5_volumes_mudou(); }   // readers see tab5_volume_disponivel() = false
  // Wait for whoever was reading to let go. Past the deadline: unmount anyway
  // (the device no longer exists - reads from anyone who insists just fail).
  int esperou = 0;
  while (s_refs.load() > 0 && esperou < SAIDA_ESPERA_MS) { vTaskDelay(pdMS_TO_TICKS(20)); esperou += 20; }
  if (s_refs.load() > 0) {
    // Whoever did not let go gets an error on the next read and lets go (it
    // checks tab5_usb_presente). The count is NOT forced to zero: if it were,
    // their late release would discount the reference of whoever holds the NEXT flash drive.
    ESP_LOGW(TAG, "%d still holding the flash drive: unmounting anyway", s_refs.load());
  }
  if (s_vfs) { msc_host_vfs_unregister(s_vfs); s_vfs = nullptr; }
  const esp_err_t r = msc_host_uninstall_device(s_disco);
  if (r != ESP_OK) { ESP_LOGW(TAG, "msc_host_uninstall_device: %s", esp_err_to_name(r)); }
  s_disco = nullptr;
  s_trava.travar();
  s_est.disco = s_est.montado = s_est.ejetado = false;
  s_est.setores = s_est.setor_bytes = 0;
  s_nome[0] = '\0';
  s_trava.destravar();
  avisar_barra(NOTIF_INFO, nullptr);
  ESP_LOGI(TAG, "flash drive unmounted");
}

// On the "usb" task. The same path as removal (disco_saiu), stopping before
// uninstalling the device: it stays plugged in, just without /usb.
static tab5_usb_ejetar_t disco_ejetar(uint32_t espera_ms)
{
  if (!s_disco || !s_vfs || !s_presente.load()) { return USB_EJETAR_SEM_PENDRIVE; }
  s_presente = false;
  tab5_volumes_mudou();   // readers see tab5_volume_disponivel() = false and let go
  uint32_t esperou = 0;
  while (s_refs.load() > 0 && esperou < espera_ms) { vTaskDelay(pdMS_TO_TICKS(20)); esperou += 20; }
  if (s_refs.load() > 0) {
    // Unlike removal, here the flash drive still exists: whoever holds it (a
    // copy, the music) carries on, and it is the eject request that gives up.
    ESP_LOGW(TAG, "eject: %d still holding the flash drive - stays mounted", s_refs.load());
    s_presente = true;
    tab5_volumes_mudou();
    return USB_EJETAR_EM_USO;
  }
  // Unmounting closes the FatFs volume: whatever was cached (FAT, directory)
  // goes to the flash drive here.
  msc_host_vfs_unregister(s_vfs);
  s_vfs = nullptr;
  s_trava.travar();
  s_est.montado = false;
  s_est.ejetado = true;
  s_nome[0] = '\0';
  s_trava.destravar();
  avisar_barra(NOTIF_INFO, "Pode tirar o pendrive");
  ESP_LOGI(TAG, "flash drive ejected: safe to remove");
  return USB_EJETADO;
}

// ---------------------------------------------------------------------------
// THE TASKS
// ---------------------------------------------------------------------------
static void tarefa_lib(void*)
{
  for (;;) {
    uint32_t f = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &f);
    if (f & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) { usb_host_device_free_all(); }
  }
}

static void tarefa_msc(void*)
{
  for (;;) { msc_host_handle_events(portMAX_DELAY); }
}

static void tarefa_usb(void*)
{
  for (;;) {
    usb_host_client_handle_events(s_cliente, portMAX_DELAY);
    ev_t ev;
    while (xQueueReceive(s_fila, &ev, 0) == pdTRUE) {
      switch (ev.tipo) {
        case EV_APARELHO:      aparelho_chegou(ev.endereco); break;
        case EV_APARELHO_FORA: aparelho_saiu((usb_device_handle_t)ev.handle); break;
        case EV_DISCO:         disco_chegou(ev.endereco); break;
        case EV_DISCO_FORA:    if (ev.handle == s_disco) { disco_saiu(); } break;
        case EV_EJETAR:
          s_ejetar_res = disco_ejetar(s_ejetar_espera_ms);
          xSemaphoreGive(s_ejetar_fim);
          break;
      }
    }
  }
}

static bool criar(TaskFunction_t fn, const char* nome, uint32_t pilha, UBaseType_t prio)
{
  if (xTaskCreatePinnedToCoreWithCaps(fn, nome, pilha, nullptr, prio, nullptr, tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM) == pdPASS) { return true; }
  ESP_LOGE(TAG, "task %s did not start", nome);
  return false;
}

// The whole USB stack. Once per boot: turning the setting off only cuts the 5V.
static void subir(void)
{
  if (s_subiu.exchange(true)) { return; }
  s_fila = xQueueCreate(8, sizeof(ev_t));
  s_ejetar_vez = xSemaphoreCreateMutex();
  s_ejetar_fim = xSemaphoreCreateBinary();
  usb_host_config_t hc = { };
  hc.intr_flags = ESP_INTR_FLAG_LOWMED;
  hc.peripheral_map = 0;   // the P4 default is the HIGH-speed controller - the USB-A one
  esp_err_t r = usb_host_install(&hc);
  if (r != ESP_OK) {
    anotar_erro(tr("a pilha USB nao subiu (%s)"), esp_err_to_name(r));
    ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(r));
    return;
  }
  // THE CLIENTS BEFORE THE LIBRARY TASK: it is the one that enumerates what is
  // already on the connector (the 5V comes up on at boot). A client registered
  // later never learns about a device that was already there.
  usb_host_client_config_t cc = { };
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = cliente_evento;
  cc.async.callback_arg = nullptr;
  r = usb_host_client_register(&cc, &s_cliente);
  msc_host_driver_config_t mc = { };
  mc.create_backround_task = false;   // the task is ours (stack in PSRAM)
  mc.callback = msc_evento;
  if (r == ESP_OK) { r = msc_host_install(&mc); }
  if (r != ESP_OK) {
    anotar_erro(tr("o driver de pendrive nao subiu (%s)"), esp_err_to_name(r));
    ESP_LOGE(TAG, "cliente/msc_host_install: %s", esp_err_to_name(r));
    return;
  }
  // Priorities: above "cartao" (2) and file readers (background jobs, 1) -
  // flash-drive reads finish in "usb_msc"; below the UI (4), the
  // mounting one.
  if (!criar(tarefa_usb, "usb", 4096, 3) || !criar(tarefa_msc, "usb_msc", 4096, 5)
      || !criar(tarefa_lib, "usb_lib", 3072, 5)) { return; }
  s_trava.travar();
  s_est.ligado = true;
  s_trava.destravar();
  ESP_LOGI(TAG, "USB-A ready (high-speed host)");
}

// ---------------------------------------------------------------------------
// THE API
// ---------------------------------------------------------------------------
void tab5_usb_iniciar(void)
{
  if (!ajustes_usb_a()) { ESP_LOGI(TAG, "USB-A disabled in settings"); return; }
  subir();
}

void tab5_usb_ligar(bool ligar)
{
  if (ligar != ajustes_usb_a()) { ajustes_definir_usb_a(ligar); }
  if (ligar) { subir(); }
  s_trava.travar();
  s_est.ligado = ligar && s_subiu.load();
  s_trava.destravar();
  sistema_perfil_mudou();   // the connector's 5V follows the setting (kernel/core.cpp)
}

bool tab5_usb_montar(void)
{
  if (!s_presente.load()) { return false; }
  ++s_refs;
  if (!s_presente.load()) { --s_refs; return false; }   // removed in the meantime
  return true;
}

void tab5_usb_desmontar(void)
{
  // Only decrement if there is something to decrement - atomically (two releasing
  // together do not take the count below zero).
  int n = s_refs.load();
  while (n > 0 && !s_refs.compare_exchange_weak(n, n - 1)) { }
}

bool tab5_usb_presente(void) { return s_presente.load(); }

tab5_usb_ejetar_t tab5_usb_ejetar(uint32_t espera_ms)
{
  if (!s_subiu.load() || !s_ejetar_vez || !s_presente.load()) { return USB_EJETAR_SEM_PENDRIVE; }
  xSemaphoreTake(s_ejetar_vez, portMAX_DELAY);
  xSemaphoreTake(s_ejetar_fim, 0);   // no stale reply
  s_ejetar_espera_ms = espera_ms;
  s_ejetar_res = USB_EJETAR_SEM_PENDRIVE;
  enfileirar(EV_EJETAR, 0, nullptr);
  // The "usb" task may be in the middle of a removal (up to 3 s) before getting
  // to the request: the deadline covers both waits.
  if (xSemaphoreTake(s_ejetar_fim, pdMS_TO_TICKS(espera_ms + SAIDA_ESPERA_MS + 2000)) != pdTRUE) {
    ESP_LOGW(TAG, "eject: the usb task did not respond");
  }
  const tab5_usb_ejetar_t r = s_ejetar_res;
  xSemaphoreGive(s_ejetar_vez);
  return r;
}

static std::atomic<bool> s_cortado{false};

bool tab5_usb_cortado(void) { return s_cortado.load(); }

void tab5_usb_repouso(bool repouso)
{
  if (repouso) {
    // Only cut what is on and unused. Music from the flash drive holds
    // the reference while it plays; when it stops, the next cycle cuts it.
    if (s_cortado.load() || !ajustes_usb_a() || s_refs.load() > 0) { return; }
    // A device that is NOT a disk (headphones, a phone) is probably
    // charging from the connector: that one keeps power.
    s_trava.travar();
    const bool carregando = s_est.aparelho && !s_est.disco;
    s_trava.destravar();
    if (carregando) { return; }
    s_cortado = true;
    tab5_power_usb_a(false);
    ESP_LOGI(TAG, "idle: USB-A 5V cut (nobody using it)");
  } else if (s_cortado.exchange(false)) {
    if (ajustes_usb_a()) { tab5_power_usb_a(true); }
  }
}

void tab5_usb_nome(char* out, size_t tam)
{
  if (!tam) { return; }
  s_trava.travar();
  snprintf(out, tam, "%s", s_presente.load() ? s_nome : "");
  s_trava.destravar();
}

bool tab5_usb_espaco(uint64_t* total, uint64_t* livre)
{
  if (!tab5_usb_montar()) { return false; }
  const bool ok = esp_vfs_fat_info(TAB5_USB_RAIZ, total, livre) == ESP_OK;
  tab5_usb_desmontar();
  return ok;
}

void tab5_usb_estado(tab5_usb_estado_t* out)
{
  s_trava.travar();
  *out = s_est;
  s_trava.destravar();
  out->refs = s_refs.load();
}
