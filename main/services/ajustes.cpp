#include "ajustes.h"

#include <M5Unified.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "tab5_wifi.hpp"   // tab5_wifi_cifrar_texto (MQTT server passwords)
#include "esp_attr.h"       // EXT_RAM_BSS_ATTR
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"   // esp_ptr_external_ram
#include <cstring>
#include <cstddef>   // offsetof
#include <cstdio>
#include <cmath>

static const char *TAG = "ajustes";
static const char *NAMESPACE = "sistema";

static uint8_t s_brilho = 80;
static uint8_t s_volume = 128;   // middle of the scale - neither mute nor flat out the first time
static bool    s_wifi   = false;
static bool    s_grove   = false;
static bool    s_usb_a   = true;    // the flash drive on USB-A works without configuring anything
static uint8_t s_tela_s  = 30;   // the system's old default, in seconds
static uint8_t s_relogio_painel = 80;  // M5GFX default for the ILI9881C; a real 60 caused underrun
static bool    s_pronto = false;

// Degrees x 1,000,000 (microdegrees), integer: fits in an NVS i32 without relying
// on a blob, and gives ~11cm of precision at the equator - plenty for marking an
// address. 0,0 (Gulf of Guinea) is the factory value and nobody lives there -
// it serves as a "not configured" sentinel without needing a separate flag.
static int32_t s_casa_lat_ue6 = 0;
static int32_t s_casa_lon_ue6 = 0;

static bool s_beep_trilha = true;   // default PLAY - see the big note in ajustes.h
static uint8_t s_tema_espectro = 0;   // 0 = rainbow
static uint16_t s_cor_tocador = 0;    // 0 = the system's
static uint8_t s_modo_tocador = 0;    // bit 0 shuffle, bits 1-2 repeat (services/tab5_musica)
static bool s_beep_mesh   = true;   // likewise, Mesh's counterpart
static uint8_t  s_espnow_canal = 0;
static uint8_t  s_espnow_letra = 0;
static uint8_t  s_bateria_soc = AJUSTES_SOC_NENHUM;
static bool     s_c6_sono  = false;       // the C6 sleeps while associated during idle (hal/tab5_c6_sono)
static bool     s_capa_net = true;        // music cover art fetched from the internet (title/artist go to iTunes)
static bool     s_wifi_cochilo = false;   // connected from the factory: the nap is the MAXIMUM saving, optional
static bool     s_sono_leve = true;
static uint8_t  s_mesh_via = AJUSTES_MESH_SERIAL;
static uint32_t s_mesh_pin = 123456;
static char     s_mesh_end[18] = "";
static bool     s_mesh_pin_tela = false;
static uint8_t  s_mesh_pos_min = 0;
static bool     s_mesh_internet = true;
static bool     s_bluetooth = false;
static uint8_t  s_parede    = AJUSTES_PAREDE_SORTEIO;
static bool     s_bloqueio  = false;   // lock screen on waking (off from the factory)
static char     s_idioma[8] = "en";    // interface language code (services/idioma.h); English out of the box
static bool     s_pendrive  = false;   // open Pendrive when plugged into a computer (off from the factory)
static uint8_t  s_lixeira_dias = 0;    // empty the bin automatically: 0 = never (default), N = items older than N days
// Gyroscope scale in parts per million (1000000 = 1.0), per axis: an NVS
// i32, like the home position, no blob.
static int32_t  s_giro_esc_ppm[3] = { 1000000, 1000000, 1000000 };
static bool     s_config_ini = false;   // see ajustes_config_inicial (read at boot)
static int16_t  s_fuso_min   = -180;    // UTC-3
static bool     s_relogio_12h = false;  // 12-hour clock (9:05pm) instead of 24-hour
static uint8_t  s_fuso_zona  = 0;       // time-zone region with daylight saving (services/tab5_fuso); 0 = manual
static uint8_t  s_mqtt_srv   = AJUSTES_MQTT_AUTOMATICO;
#define GIRO_ESC_MIN_PPM  900000
#define GIRO_ESC_MAX_PPM 1100000

// WRITING TO FLASH DISABLES THE CACHE, AND WITHOUT CACHE THE PSRAM DISAPPEARS. A task with its
// STACK in PSRAM (Berry's, for example) that writes NVS brings the device down
// (assert in spi_flash_disable_interrupts_caches_and_other_cpu). Only the blobs
// (MQTT list, power test) still write directly and check this; the settings in the
// table below never write from the caller.
static bool pilha_na_psram(void)
{
  int aqui = 0;
  return esp_ptr_external_ram(&aqui);
}

static void mq_carregar(void);   // the MQTT server list, further down

// ---------------------------------------------------------------------------
// THE SETTINGS TABLE: NVS key, variable, type and valid range. Reading at
// boot and saving are a single loop over it. A value read outside the range (old
// version, garbage) is ignored: the variable's default stays.
// ---------------------------------------------------------------------------
typedef enum { AJ_BOOL, AJ_U8, AJ_U16, AJ_U32, AJ_I16, AJ_I32, AJ_TXT } aj_tipo_t;
typedef struct { const char* chave; void* var; aj_tipo_t tipo; int64_t min, max; } ajuste_t;
#define BOOL_(c, v)        { c, &v, AJ_BOOL, 0, 255 }
#define U8_(c, v, a, b)    { c, &v, AJ_U8,  a, b }
#define U16_(c, v)         { c, &v, AJ_U16, 0, 65535 }

static const ajuste_t AJUSTES[] = {
  U8_("brilho", s_brilho, AJUSTES_BRILHO_MIN, 255),
  U8_("volume", s_volume, 0, 255),
  BOOL_("wifi", s_wifi),
  BOOL_("grove", s_grove),
  BOOL_("usb_a", s_usb_a),
  U8_("tela_s", s_tela_s, AJUSTES_TELA_MIN_S, AJUSTES_TELA_MAX_S),   // never a screen that does not turn off
  U8_("relogio_p", s_relogio_painel, AJUSTES_RELOGIO_MIN, AJUSTES_RELOGIO_MAX),
  { "casa_lat", &s_casa_lat_ue6, AJ_I32, INT32_MIN, INT32_MAX },
  { "casa_lon", &s_casa_lon_ue6, AJ_I32, INT32_MIN, INT32_MAX },
  { "giro_esc_x", &s_giro_esc_ppm[0], AJ_I32, GIRO_ESC_MIN_PPM, GIRO_ESC_MAX_PPM },
  { "giro_esc_y", &s_giro_esc_ppm[1], AJ_I32, GIRO_ESC_MIN_PPM, GIRO_ESC_MAX_PPM },
  { "giro_esc_z", &s_giro_esc_ppm[2], AJ_I32, GIRO_ESC_MIN_PPM, GIRO_ESC_MAX_PPM },
  BOOL_("beep_trilha", s_beep_trilha),
  U8_("esp_tema", s_tema_espectro, 0, 255),
  U16_("cor_tocador", s_cor_tocador),
  U8_("mus_modo", s_modo_tocador, 0, 255),
  BOOL_("beep_mesh", s_beep_mesh),
  U8_("espnow_c", s_espnow_canal, 0, 13),
  U8_("espnow_l", s_espnow_letra, 0, AJUSTES_ESPNOW_LETRA_MAX),
  U8_("bat_soc", s_bateria_soc, 0, 100),
  BOOL_("wifi_coch", s_wifi_cochilo),
  BOOL_("capa_net", s_capa_net),
  BOOL_("c6_sono", s_c6_sono),
  BOOL_("sono_lv", s_sono_leve),
  U8_("mesh_via", s_mesh_via, 0, AJUSTES_MESH_INTERNET),
  U8_("mesh_pos", s_mesh_pos_min, 0, 255),
  BOOL_("mesh_net", s_mesh_internet),
  BOOL_("bluetooth", s_bluetooth),
  U8_("mqtt_srv", s_mqtt_srv, 0, 255),
  { "fuso_min", &s_fuso_min, AJ_I16, AJUSTES_FUSO_MIN_MIN, AJUSTES_FUSO_MAX_MIN },
  U8_("fuso_zona", s_fuso_zona, 0, 254),
  BOOL_("relog_12h", s_relogio_12h),
  BOOL_("config_ini", s_config_ini),
  BOOL_("bloqueio", s_bloqueio),
  BOOL_("pendrive", s_pendrive),
  U8_("lixo_dias", s_lixeira_dias, 0, 255),
  U8_("parede", s_parede, 0, AJUSTES_PAREDE_NENHUM),
  { "lingua", s_idioma, AJ_TXT, 0, sizeof(s_idioma) },
  { "mesh_pin", &s_mesh_pin, AJ_U32, 0, 999999 },
  BOOL_("mesh_pin_t", s_mesh_pin_tela),
  { "mesh_end", s_mesh_end, AJ_TXT, 0, sizeof(s_mesh_end) },
};
#define N_AJUSTES (int)(sizeof(AJUSTES) / sizeof(AJUSTES[0]))
static_assert(N_AJUSTES <= 64, "a mascara de sujos e um uint64_t");

static int64_t valor_de(const ajuste_t& a)
{
  switch (a.tipo) {
    case AJ_BOOL: return *(bool*)a.var ? 1 : 0;
    case AJ_U8:   return *(uint8_t*)a.var;
    case AJ_U16:  return *(uint16_t*)a.var;
    case AJ_U32:  return *(uint32_t*)a.var;
    case AJ_I16:  return *(int16_t*)a.var;
    default:      return *(int32_t*)a.var;
  }
}

static void por_valor(const ajuste_t& a, int64_t v)
{
  switch (a.tipo) {
    case AJ_BOOL: *(bool*)a.var     = (v != 0);     break;
    case AJ_U8:   *(uint8_t*)a.var  = (uint8_t)v;   break;
    case AJ_U16:  *(uint16_t*)a.var = (uint16_t)v;  break;
    case AJ_U32:  *(uint32_t*)a.var = (uint32_t)v;  break;
    case AJ_I16:  *(int16_t*)a.var  = (int16_t)v;   break;
    default:      *(int32_t*)a.var  = (int32_t)v;   break;
  }
}

static void ler_ajuste(nvs_handle_t h, const ajuste_t& a)
{
  if (a.tipo == AJ_TXT) {
    size_t tam = (size_t)a.max;
    if (nvs_get_str(h, a.chave, (char*)a.var, &tam) != ESP_OK) { ((char*)a.var)[0] = 0; }
    return;
  }
  int64_t v;
  esp_err_t e;
  if (a.tipo == AJ_BOOL || a.tipo == AJ_U8) { uint8_t x;  e = nvs_get_u8(h, a.chave, &x);  v = x; }
  else if (a.tipo == AJ_U16)                { uint16_t x; e = nvs_get_u16(h, a.chave, &x); v = x; }
  else if (a.tipo == AJ_U32)                { uint32_t x; e = nvs_get_u32(h, a.chave, &x); v = x; }
  else                                      { int32_t x;  e = nvs_get_i32(h, a.chave, &x); v = x; }   // I16 lives in an i32
  if (e == ESP_OK && v >= a.min && v <= a.max) { por_valor(a, v); }
}

static void gravar_ajuste(nvs_handle_t h, const ajuste_t& a)
{
  const int64_t v = a.tipo == AJ_TXT ? 0 : valor_de(a);
  switch (a.tipo) {
    case AJ_BOOL: case AJ_U8: nvs_set_u8(h, a.chave, (uint8_t)v);   break;
    case AJ_U16:              nvs_set_u16(h, a.chave, (uint16_t)v); break;
    case AJ_U32:              nvs_set_u32(h, a.chave, (uint32_t)v); break;
    case AJ_TXT:              nvs_set_str(h, a.chave, (const char*)a.var); break;
    default:                  nvs_set_i32(h, a.chave, (int32_t)v);  break;
  }
}

// ---------------------------------------------------------------------------
// SAVING: ALWAYS DEFERRED, ALONG A SINGLE PATH. Whoever changes a setting only marks the
// variable as dirty; 1 s after the LAST mark, the esp_timer task (internal
// stack) writes the dirty ones in one go, with one commit. This solves at once:
//   - dragging (brightness, volume): NVS receives only the final value, and the UI
//     task never stalls with the cache off;
//   - whoever has a stack in PSRAM (Berry): never writes, only marks.
// What was not marked is not written - a setting never touched keeps
// following the firmware default.
// ---------------------------------------------------------------------------
#define ADIAR_GRAVACAO_US 1000000
static esp_timer_handle_t     s_adiar = nullptr;
static std::atomic<uint64_t>  s_sujos{0};

static void gravar_sujos(void*)
{
  const uint64_t sujos = s_sujos.exchange(0);
  if (!sujos) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  for (int i = 0; i < N_AJUSTES; ++i) {
    if (sujos & (1ull << i)) { gravar_ajuste(h, AJUSTES[i]); }
  }
  nvs_commit(h);      // without the commit, the value vanishes at the next boot
  nvs_close(h);
}

static void adiar_gravacao(uint64_t prazo_us)
{
  if (!s_adiar) {
    const esp_timer_create_args_t a = { .callback = gravar_sujos, .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK, .name = "ajustes_gravar",
                                        .skip_unhandled_events = true };
    if (esp_timer_create(&a, &s_adiar) != ESP_OK) { s_adiar = nullptr; return; }
  }
  esp_timer_stop(s_adiar);   // each new value pushes the deadline back
  esp_timer_start_once(s_adiar, prazo_us);
}

// The variable changed: mark and schedule. 'var' is the address in the table.
static void marcar(const void* var)
{
  if (!s_pronto) { return; }
  for (int i = 0; i < N_AJUSTES; ++i) {
    if (AJUSTES[i].var == var) { s_sujos.fetch_or(1ull << i); adiar_gravacao(ADIAR_GRAVACAO_US); return; }
  }
}

void ajustes_gravar_agora(void)
{
  if (s_pronto && s_sujos.load()) { adiar_gravacao(0); }
}

void ajustes_iniciar(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS partition worn out or from an old version. Erasing here is safe: there are only
    // our settings, nothing the user really loses.
    ESP_LOGW(TAG, "NVS had to be reinitialised");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS unavailable (%s) - settings only apply to this session",
             esp_err_to_name(err));
    return;
  }
  s_pronto = true;

  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    uint8_t v;
    const bool tem_marca = nvs_get_u8(h, "config_ini", &v) == ESP_OK;
    for (int i = 0; i < N_AJUSTES; ++i) { ler_ajuste(h, AJUSTES[i]); }
    nvs_close(h);
    // FIRST USE: without the mark, but with ANY other setting already saved, the
    // device was already running the system before this version - it is not first use.
    if (!tem_marca) {
      nvs_iterator_t it = nullptr;
      s_config_ini = nvs_entry_find(NVS_DEFAULT_PART_NAME, NAMESPACE, NVS_TYPE_ANY, &it) == ESP_OK;
      nvs_release_iterator(it);
      // New device: saves the mark as "still to do". That way, if it is switched off
      // midway through the wizard (with brightness or Wi-Fi already saved), the wizard
      // comes back at the next boot instead of counting as configured.
      if (!s_config_ini) { marcar(&s_config_ini); }
    }
  }
  // The password key (hal/tab5_wifi) before any encryption: on the first
  // boot it is randomly generated and saved - here, with the boot stack in internal RAM.
  tab5_wifi_chave_preparar();
  mq_carregar();   // the MQTT server list (and migration from the old format)

  ESP_LOGI(TAG, "settings read: brightness %d, WiFi %s", (int)s_brilho, s_wifi ? "on" : "off");
}

uint8_t ajustes_brilho(void) { return s_brilho; }

void ajustes_definir_brilho(uint8_t brilho)
{
  if (brilho < AJUSTES_BRILHO_MIN) { brilho = AJUSTES_BRILHO_MIN; }
  s_brilho = brilho;

  // Applies at once: moving a slider and not seeing the screen react would be confusing.
  // Through the normal API (and not the panel directly), because this is the
  // OFFICIAL brightness - the value tab5_power will restore on waking from idle.
  M5.Display.setBrightness(s_brilho);

  marcar(&s_brilho);   // saved 1 s after the last value
}

// The PWM is linear in light, but the eye is not: 4% duty already looks like "a quarter
// of the brightness". That is why the position goes through the gamma 2.2 curve - the bottom half of the
// scale covers 0.9% to ~22% of the duty, in fine steps.
int ajustes_brilho_para_pos(int brilho)
{
  const float f = (float)(brilho - AJUSTES_BRILHO_MIN) / (float)(AJUSTES_BRILHO_MAX - AJUSTES_BRILHO_MIN);
  return (int)(powf(f < 0 ? 0 : f, 1.0f / 2.2f) * AJUSTES_BRILHO_POS_MAX + 0.5f);
}

int ajustes_pos_para_brilho(int pos)
{
  const float f = (float)pos / (float)AJUSTES_BRILHO_POS_MAX;
  return AJUSTES_BRILHO_MIN + (int)(powf(f, 2.2f) * (AJUSTES_BRILHO_MAX - AJUSTES_BRILHO_MIN) + 0.5f);
}

uint8_t ajustes_brilho_pct(void)
{
  return (uint8_t)((ajustes_brilho_para_pos(s_brilho) + 5) / 10);
}

void ajustes_definir_brilho_pct(uint8_t pct)
{
  ajustes_definir_brilho((uint8_t)ajustes_pos_para_brilho((pct > 100 ? 100 : pct) * 10));
}

uint8_t ajustes_volume(void) { return s_volume; }

void ajustes_definir_volume(uint8_t volume)
{
  if (volume < AJUSTES_VOLUME_MIN) { volume = AJUSTES_VOLUME_MIN; }
  if (volume > AJUSTES_VOLUME_MAX) { volume = AJUSTES_VOLUME_MAX; }
  s_volume = volume;

  // No I2C cost here - setVolume() only stores the number in the Speaker object,
  // it does not turn on the ES8388 (see the big note in ajustes.h). That is why it is safe
  // to call from ANY task, unlike tab5_camera_iniciar() and
  // friends - the rule "only the UI task talks I2C" does not apply
  // here.
  M5.Speaker.setVolume(s_volume);

  marcar(&s_volume);   // saved 1 s after the last value
}

bool ajustes_wifi(void) { return s_wifi; }

void ajustes_definir_wifi(bool ligado)
{
  s_wifi = ligado;
  marcar(&s_wifi);
}

bool ajustes_grove(void) { return s_grove; }

uint8_t ajustes_tela_s(void) { return s_tela_s; }

void ajustes_definir_tela_s(uint8_t segundos)
{
  s_tela_s = (segundos < AJUSTES_TELA_MIN_S) ? AJUSTES_TELA_MIN_S
           : (segundos > AJUSTES_TELA_MAX_S) ? AJUSTES_TELA_MAX_S : segundos;
  marcar(&s_tela_s);
}

void ajustes_definir_grove(bool ligado)
{
  s_grove = ligado;
  marcar(&s_grove);
  ESP_LOGI(TAG, "Grove 5V always on: %s", ligado ? "yes" : "no");
}

bool ajustes_usb_a(void) { return s_usb_a; }

void ajustes_definir_usb_a(bool ligado)
{
  s_usb_a = ligado;
  marcar(&s_usb_a);
  ESP_LOGI(TAG, "USB-A (flash drive): %s", ligado ? "on" : "off");
}

uint8_t ajustes_relogio_painel(void) { return s_relogio_painel; }

void ajustes_definir_relogio_painel(uint8_t mhz)
{
  s_relogio_painel = (mhz < AJUSTES_RELOGIO_MIN) ? AJUSTES_RELOGIO_MIN
                   : (mhz > AJUSTES_RELOGIO_MAX) ? AJUSTES_RELOGIO_MAX : mhz;
  marcar(&s_relogio_painel);
  ESP_LOGI(TAG, "panel clock = %d MHz", (int)s_relogio_painel);
}

bool ajustes_casa(float* lat, float* lon)
{
  if (s_casa_lat_ue6 == 0 && s_casa_lon_ue6 == 0) { return false; }
  if (lat) { *lat = (float)s_casa_lat_ue6 / 1000000.0f; }
  if (lon) { *lon = (float)s_casa_lon_ue6 / 1000000.0f; }
  return true;
}

void ajustes_definir_casa(float lat, float lon)
{
  s_casa_lat_ue6 = (int32_t)(lat * 1000000.0f);
  s_casa_lon_ue6 = (int32_t)(lon * 1000000.0f);
  ESP_LOGI(TAG, "home position saved");   // without the coordinates: the log lives on the card
  marcar(&s_casa_lat_ue6);
  marcar(&s_casa_lon_ue6);
}

bool ajustes_beep_trilha(void) { return s_beep_trilha; }

void ajustes_definir_beep_trilha(bool tocar)
{
  s_beep_trilha = tocar;
  marcar(&s_beep_trilha);
  ESP_LOGI(TAG, "Trilha beep: %s", tocar ? "on" : "off");
}

uint16_t ajustes_cor_tocador(void) { return s_cor_tocador; }

// A Berry app calls this (musica.cor), from the Berry task - stack in PSRAM:
// that is why it only marks (saving is always deferred, see marcar).
void ajustes_definir_cor_tocador(uint16_t cor565)
{
  if (cor565 == s_cor_tocador) { return; }   // without writing the flash for nothing
  s_cor_tocador = cor565;
  marcar(&s_cor_tocador);   // saved 1 s after the last value
}

uint8_t ajustes_modo_tocador(void) { return s_modo_tocador; }

// The Berry module also changes the mode.
void ajustes_definir_modo_tocador(uint8_t modo)
{
  if (modo == s_modo_tocador) { return; }
  s_modo_tocador = modo;
  marcar(&s_modo_tocador);   // saved 1 s after the last value
}

uint8_t ajustes_tema_espectro(void) { return s_tema_espectro; }

void ajustes_definir_tema_espectro(uint8_t tema)
{
  s_tema_espectro = tema;
  marcar(&s_tema_espectro);   // saved 1 s after the last value
}

bool ajustes_beep_mesh(void) { return s_beep_mesh; }

void ajustes_definir_beep_mesh(bool tocar)
{
  s_beep_mesh = tocar;
  marcar(&s_beep_mesh);
  ESP_LOGI(TAG, "Mesh beep: %s", tocar ? "on" : "off");
}

uint8_t ajustes_espnow_canal(void) { return s_espnow_canal; }

void ajustes_definir_espnow_canal(uint8_t canal)
{
  if (canal > 13) { canal = 0; }
  if (canal == s_espnow_canal) { return; }   // the service reapplies it at boot: no rewriting the flash for nothing
  s_espnow_canal = canal;
  marcar(&s_espnow_canal);
}

uint8_t ajustes_espnow_letra(void) { return s_espnow_letra; }

void ajustes_definir_espnow_letra(uint8_t nivel)
{
  if (nivel > AJUSTES_ESPNOW_LETRA_MAX) { nivel = 0; }
  if (nivel == s_espnow_letra) { return; }
  s_espnow_letra = nivel;
  marcar(&s_espnow_letra);
}

uint8_t ajustes_bateria_soc(void) { return s_bateria_soc; }

void ajustes_definir_bateria_soc(uint8_t pct)
{
  if (pct > 100 || pct == s_bateria_soc) { return; }
  s_bateria_soc = pct;
  marcar(&s_bateria_soc);
}

bool ajustes_wifi_cochilo(void) { return s_wifi_cochilo; }

bool ajustes_sono_leve(void) { return s_sono_leve; }

void ajustes_definir_sono_leve(bool ligado)
{
  if (ligado == s_sono_leve) { return; }
  s_sono_leve = ligado;
  marcar(&s_sono_leve);
}

// The power test (services/tab5_ensaio) really turns services off; the
// originals stay here until it gives them back, so a restart midway does not leave
// Wi-Fi/Bluetooth/Grove off forever. From the UI (internal stack).
bool ajustes_ensaio_originais(uint8_t out[AJUSTES_ENSAIO_N])
{
  if (!s_pronto) { return false; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return false; }
  size_t t = AJUSTES_ENSAIO_N;
  const bool ok = nvs_get_blob(h, "ensaio_o", out, &t) == ESP_OK && t == AJUSTES_ENSAIO_N;
  nvs_close(h);
  return ok;
}

void ajustes_definir_ensaio_originais(const uint8_t* o)
{
  if (!s_pronto || pilha_na_psram()) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (o) { nvs_set_blob(h, "ensaio_o", o, AJUSTES_ENSAIO_N); } else { nvs_erase_key(h, "ensaio_o"); }
  nvs_commit(h);
  nvs_close(h);
}

bool ajustes_c6_sono(void) { return s_c6_sono; }

void ajustes_definir_c6_sono(bool ligado)
{
  if (ligado == s_c6_sono) { return; }
  s_c6_sono = ligado;
  marcar(&s_c6_sono);
}

bool ajustes_capa_internet(void) { return s_capa_net; }

void ajustes_definir_capa_internet(bool ligado)
{
  if (ligado == s_capa_net) { return; }
  s_capa_net = ligado;
  marcar(&s_capa_net);
}

void ajustes_definir_wifi_cochilo(bool ligado)
{
  if (ligado == s_wifi_cochilo) { return; }
  s_wifi_cochilo = ligado;
  marcar(&s_wifi_cochilo);
}

uint8_t ajustes_mesh_via(void) { return s_mesh_via; }

void ajustes_definir_mesh_via(uint8_t via)
{
  if (via > AJUSTES_MESH_INTERNET) { via = AJUSTES_MESH_SERIAL; }
  if (via == s_mesh_via) { return; }   // NVS has finite write cycles
  s_mesh_via = via;
  marcar(&s_mesh_via);
  ESP_LOGI(TAG, "Mesh goes via %s", via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth"
                                  : via == AJUSTES_MESH_INTERNET  ? "internet" : "Grove cable");
}

uint32_t ajustes_mesh_pin(void) { return s_mesh_pin; }

void ajustes_definir_mesh_pin(uint32_t pin)
{
  if (pin > 999999) { return; }
  s_mesh_pin = pin;
  marcar(&s_mesh_pin);
}

void ajustes_mesh_ble_endereco(char out[18]) { memcpy(out, s_mesh_end, sizeof(s_mesh_end)); }

bool ajustes_mesh_pin_na_tela(void) { return s_mesh_pin_tela; }

void ajustes_definir_mesh_pin_na_tela(bool sim)
{
  if (sim == s_mesh_pin_tela) { return; }
  s_mesh_pin_tela = sim;
  marcar(&s_mesh_pin_tela);
}

void ajustes_definir_mesh_ble_endereco(const char* end)
{
  // Another node: what was known about the previous one's PIN does not apply to it.
  if (strcmp(end ? end : "", s_mesh_end) != 0) { ajustes_definir_mesh_pin_na_tela(false); }
  snprintf(s_mesh_end, sizeof(s_mesh_end), "%s", end ? end : "");
  marcar(s_mesh_end);
}

bool ajustes_mesh_internet(void) { return s_mesh_internet; }

void ajustes_definir_mesh_internet(bool ligado)
{
  if (ligado == s_mesh_internet) { return; }
  s_mesh_internet = ligado;
  marcar(&s_mesh_internet);
}

uint8_t ajustes_mesh_posicao_min(void) { return s_mesh_pos_min; }

void ajustes_definir_mesh_posicao_min(uint8_t minutos)
{
  if (minutos == s_mesh_pos_min) { return; }
  s_mesh_pos_min = minutos;
  marcar(&s_mesh_pos_min);
}

bool ajustes_bluetooth(void) { return s_bluetooth; }

void ajustes_definir_bluetooth(bool ligado)
{
  s_bluetooth = ligado;
  marcar(&s_bluetooth);
}

bool ajustes_bloqueio(void) { return s_bloqueio; }

void ajustes_definir_bloqueio(bool ligado)
{
  s_bloqueio = ligado;
  marcar(&s_bloqueio);
}

uint8_t ajustes_lixeira_dias(void) { return s_lixeira_dias; }

void ajustes_definir_lixeira_dias(uint8_t dias)
{
  if (dias == s_lixeira_dias) { return; }
  s_lixeira_dias = dias;
  marcar(&s_lixeira_dias);
}

bool ajustes_pendrive(void) { return s_pendrive; }

void ajustes_definir_pendrive(bool ligado)
{
  s_pendrive = ligado;
  marcar(&s_pendrive);
}

const char* ajustes_idioma(void) { return s_idioma; }

void ajustes_definir_idioma(const char* codigo)
{
  snprintf(s_idioma, sizeof(s_idioma), "%s", codigo ? codigo : "en");
  marcar(s_idioma);
}

uint8_t ajustes_parede(void) { return s_parede; }

void ajustes_definir_parede(uint8_t opcao)
{
  if (opcao > AJUSTES_PAREDE_NENHUM) { opcao = AJUSTES_PAREDE_SORTEIO; }
  s_parede = opcao;
  marcar(&s_parede);
}

float ajustes_giro_escala(int eixo)
{
  if (eixo < 0 || eixo > 2) { return 1.0f; }
  return (float)s_giro_esc_ppm[eixo] / 1000000.0f;
}

void ajustes_definir_giro_escala(int eixo, float escala)
{
  if (eixo < 0 || eixo > 2) { return; }
  int32_t ppm = (int32_t)lroundf(escala * 1000000.0f);
  if (ppm < GIRO_ESC_MIN_PPM || ppm > GIRO_ESC_MAX_PPM) { return; }
  s_giro_esc_ppm[eixo] = ppm;
  ESP_LOGI(TAG, "gyro scale, axis %c: %.4f", "xyz"[eixo], (double)escala);
  marcar(&s_giro_esc_ppm[eixo]);
}

// ICON ORDER - see ajustes.h. Read from NVS on demand (only at boot and from the
// console), with no copy in RAM: the shell keeps the order in use.
void ajustes_ordem_apps(char* out, size_t n)
{
  if (!out || n == 0) { return; }
  out[0] = 0;
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
  size_t tam = n;
  if (nvs_get_str(h, "apps_ordem", out, &tam) != ESP_OK) { out[0] = 0; }
  nvs_close(h);
}

void ajustes_definir_ordem_apps(const char* nomes)
{
  if (!s_pronto) { return; }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) { return; }
  if (nomes && nomes[0]) { nvs_set_str(h, "apps_ordem", nomes); }
  else                   { nvs_erase_key(h, "apps_ordem"); }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "app order: %s", (nomes && nomes[0]) ? nomes : "(default)");
}

// ---- first use, time zone and MQTT server (see ajustes.h) --------------------
bool ajustes_config_inicial(void) { return s_config_ini; }

void ajustes_definir_config_inicial(bool feita)
{
  s_config_ini = feita;
  marcar(&s_config_ini);
}

int16_t ajustes_fuso_min(void) { return s_fuso_min; }

void ajustes_definir_fuso_min(int16_t minutos)
{
  if (minutos < AJUSTES_FUSO_MIN_MIN) { minutos = AJUSTES_FUSO_MIN_MIN; }
  if (minutos > AJUSTES_FUSO_MAX_MIN) { minutos = AJUSTES_FUSO_MAX_MIN; }
  if (minutos == s_fuso_min) { return; }
  s_fuso_min = minutos;
  marcar(&s_fuso_min);
  ESP_LOGI(TAG, "time zone: UTC%+d:%02d", minutos / 60, abs(minutos) % 60);
}

bool ajustes_relogio_12h(void) { return s_relogio_12h; }

void ajustes_definir_relogio_12h(bool sim)
{
  s_relogio_12h = sim;
  marcar(&s_relogio_12h);
}

uint8_t ajustes_fuso_zona(void) { return s_fuso_zona; }

void ajustes_definir_fuso_zona(uint8_t zona)
{
  if (zona == s_fuso_zona) { return; }
  s_fuso_zona = zona;
  marcar(&s_fuso_zona);
  ESP_LOGI(TAG, "time-zone region: %u", (unsigned)zona);
}

uint8_t ajustes_mqtt_servidor(void) { return s_mqtt_srv; }

void ajustes_definir_mqtt_servidor(uint8_t escolha)
{
  if (escolha == s_mqtt_srv) { return; }
  s_mqtt_srv = escolha;
  marcar(&s_mqtt_srv);
}

// ---- the MQTT server list (see ajustes.h) ---------------------------------
// A SINGLE BLOB in NVS ("mqtt_lista"): the version, how many, and the servers with the
// password already encrypted. A DECRYPTED copy in RAM (PSRAM - read-only outside here):
// internet mode consults it on every connection, and decrypting every time would be
// wasteful. Read at boot (ajustes_iniciar), which also migrates the old
// format to this one.
//
// The UI writes it (Settings, Mesh, welcome), on its own task -
// internal stack. There is no Berry API for this (see pilha_na_psram).
typedef struct { char nome[24], srv[64], usr[32], raiz[48]; char pwd_hex[112]; } mq_guardado_t;
typedef struct { uint8_t versao, n; uint8_t vago[2]; mq_guardado_t s[AJUSTES_MQTT_MAX]; } mq_blob_t;
#define MQ_VERSAO 1
EXT_RAM_BSS_ATTR static ajustes_mqtt_srv_t s_mq[AJUSTES_MQTT_MAX];
static int s_mq_n = 0;

static void mq_guardar(void)
{
  if (!s_pronto) { return; }
  if (pilha_na_psram()) { ESP_LOGE(TAG, "MQTT list not saved: stack in PSRAM"); return; }
  // The blob goes from internal RAM: writing the flash disables the cache, and with it the PSRAM.
  mq_blob_t* b = (mq_blob_t*)heap_caps_calloc(1, sizeof(mq_blob_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b) { ESP_LOGE(TAG, "MQTT list not saved: out of memory"); return; }
  b->versao = MQ_VERSAO;
  b->n = (uint8_t)s_mq_n;
  for (int i = 0; i < s_mq_n; ++i) {
    const ajustes_mqtt_srv_t& s = s_mq[i];
    mq_guardado_t& g = b->s[i];
    snprintf(g.nome, sizeof(g.nome), "%s", s.nome);
    snprintf(g.srv,  sizeof(g.srv),  "%s", s.srv);
    snprintf(g.usr,  sizeof(g.usr),  "%s", s.usr);
    snprintf(g.raiz, sizeof(g.raiz), "%s", s.raiz);
    char hex[TAB5_WIFI_CIFRA_HEX] = "";
    if (s.pwd[0] && !tab5_wifi_cifrar_texto(s.pwd, hex, sizeof(hex))) { hex[0] = 0; }
    snprintf(g.pwd_hex, sizeof(g.pwd_hex), "%s", hex);
  }
  // Only the used ones: the blob shrinks with the list.
  const size_t tam = offsetof(mq_blob_t, s) + sizeof(mq_guardado_t) * (size_t)s_mq_n;
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_blob(h, "mqtt_lista", b, tam);
    nvs_commit(h);
    nvs_close(h);
  }
  memset(b, 0, sizeof(*b));
  heap_caps_free(b);
}

static void mq_por(const char* nome, const char* srv, const char* usr, const char* pwd, const char* raiz)
{
  if (s_mq_n >= AJUSTES_MQTT_MAX) { return; }
  ajustes_mqtt_srv_t& s = s_mq[s_mq_n++];
  snprintf(s.nome, sizeof(s.nome), "%s", nome);
  snprintf(s.srv,  sizeof(s.srv),  "%s", srv);
  snprintf(s.usr,  sizeof(s.usr),  "%s", usr);
  snprintf(s.pwd,  sizeof(s.pwd),  "%s", pwd);
  snprintf(s.raiz, sizeof(s.raiz), "%s", raiz);
}

// THE OLD FORMAT: a fixed list (1 Mesh Brasil, 2 USA, 3 Europe, 4 ANZ, 5
// Brazil global) and an "own" server (255) in four loose keys. It becomes
// the factory list, plus whatever the person was using, with the choice pointing at
// the same server as before.
static void mq_migrar(void)
{
  s_mq_n = 0;
  mq_por("Mesh Brasil", "platform.meshbrasil.com", "meshdev", "large4cats", "meshdev");
  mq_por("Meshtastic Brasil", "mqtt.meshtastic.org", "meshdev", "large4cats", "msh/BR");
  uint8_t nova = AJUSTES_MQTT_AUTOMATICO;
  switch (s_mqtt_srv) {
    case 1: nova = 1; break;
    case 5: nova = 2; break;
    case 2: mq_por("Meshtastic EUA", "mqtt.meshtastic.org", "meshdev", "large4cats", "msh/US");
            nova = (uint8_t)s_mq_n; break;
    case 3: mq_por("Meshtastic Europa", "mqtt.meshtastic.org", "meshdev", "large4cats", "msh/EU_868");
            nova = (uint8_t)s_mq_n; break;
    case 4: mq_por("Meshtastic Australia", "mqtt.meshtastic.org", "meshdev", "large4cats", "msh/ANZ");
            nova = (uint8_t)s_mq_n; break;
    default: break;
  }
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
    char srv[64] = "", usr[32] = "", raiz[48] = "", pwd[32] = "", hex[TAB5_WIFI_CIFRA_HEX] = "";
    size_t t = sizeof(srv);  nvs_get_str(h, "mqtt_p_srv", srv, &t);
    t = sizeof(usr);         nvs_get_str(h, "mqtt_p_usr", usr, &t);
    t = sizeof(raiz);        nvs_get_str(h, "mqtt_p_raiz", raiz, &t);
    t = sizeof(hex);
    if (nvs_get_str(h, "mqtt_p_pwd", hex, &t) != ESP_OK || !tab5_wifi_decifrar_texto(hex, pwd, sizeof(pwd))) { pwd[0] = 0; }
    if (srv[0]) {
      mq_por("Own server", srv, usr, pwd, raiz[0] ? raiz : "msh");
      if (s_mqtt_srv == 255) { nova = (uint8_t)s_mq_n; }
    }
    memset(pwd, 0, sizeof(pwd));
    nvs_erase_key(h, "mqtt_p_srv");
    nvs_erase_key(h, "mqtt_p_usr");
    nvs_erase_key(h, "mqtt_p_raiz");
    nvs_erase_key(h, "mqtt_p_pwd");
    nvs_commit(h);
    nvs_close(h);
  }
  mq_guardar();
  if (nova != s_mqtt_srv) { s_mqtt_srv = nova; marcar(&s_mqtt_srv); }
  ESP_LOGI(TAG, "MQTT servers: list created (%d), choice %u", s_mq_n, (unsigned)s_mqtt_srv);
}

static void mq_carregar(void)
{
  s_mq_n = 0;
  if (!s_pronto) { return; }
  mq_blob_t* b = (mq_blob_t*)heap_caps_calloc(1, sizeof(mq_blob_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b) { return; }
  bool lido = false;
  nvs_handle_t h;
  if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
    size_t tam = sizeof(*b);
    lido = nvs_get_blob(h, "mqtt_lista", b, &tam) == ESP_OK && b->versao == MQ_VERSAO
        && b->n <= AJUSTES_MQTT_MAX && tam >= offsetof(mq_blob_t, s) + sizeof(mq_guardado_t) * b->n;
    nvs_close(h);
  }
  bool legado = false;   // some password in the old cipher (MAC key): rewrite
  if (lido) {
    for (int i = 0; i < b->n; ++i) {
      const mq_guardado_t& g = b->s[i];
      char pwd[32] = "";
      bool velha = false;
      if (g.pwd_hex[0] && !tab5_wifi_decifrar_texto(g.pwd_hex, pwd, sizeof(pwd), &velha)) { pwd[0] = 0; }
      if (g.pwd_hex[0] && velha) { legado = true; }
      mq_por(g.nome, g.srv, g.usr, pwd, g.raiz);
      memset(pwd, 0, sizeof(pwd));
    }
  }
  memset(b, 0, sizeof(*b));
  heap_caps_free(b);
  if (!lido) { mq_migrar(); }
  else if (legado) { mq_guardar(); ESP_LOGW(TAG, "MQTT servers: passwords rewritten with the new key"); }
  if (s_mqtt_srv > s_mq_n) { s_mqtt_srv = AJUSTES_MQTT_AUTOMATICO; }   // pointed beyond the list
}

int ajustes_mqtt_n(void) { return s_mq_n; }

bool ajustes_mqtt_ler(int i, ajustes_mqtt_srv_t* out)
{
  if (i < 0 || i >= s_mq_n || !out) { return false; }
  *out = s_mq[i];
  return true;
}

int ajustes_mqtt_gravar(int i, const ajustes_mqtt_srv_t* s)
{
  if (!s) { return -1; }
  if (i < 0 || i >= s_mq_n) {
    if (s_mq_n >= AJUSTES_MQTT_MAX) { return -1; }
    i = s_mq_n++;
  }
  s_mq[i] = *s;
  mq_guardar();
  ESP_LOGI(TAG, "MQTT server %d: %s (%s, root %s)", i + 1, s->nome, s->srv, s->raiz);
  return i;
}

void ajustes_mqtt_apagar(int i)
{
  if (i < 0 || i >= s_mq_n) { return; }
  for (int k = i; k < s_mq_n - 1; ++k) { s_mq[k] = s_mq[k + 1]; }
  --s_mq_n;
  memset(&s_mq[s_mq_n], 0, sizeof(s_mq[s_mq_n]));
  mq_guardar();
  // The choice follows the same server: the deleted one goes back to automatic, those
  // after it moved up one place.
  const int e = (int)s_mqtt_srv - 1;
  if (e == i)     { ajustes_definir_mqtt_servidor(AJUSTES_MQTT_AUTOMATICO); }
  else if (e > i) { ajustes_definir_mqtt_servidor((uint8_t)e); }
}
