// APP: Camera - live preview from the SC2356 (MIPI-CSI) + capture to the SD card.
//
// FIRST VERSION, deliberately small scope - see the big notice in
// hal/tab5_camera.hpp for what has already been verified (sensor, pins, I2C
// bus shared with M5.In_I2C, the official example this code follows).
//
// The photo comes out with the sensor's whole frame (1200x1600); the preview
// uses the same image at a uniform scale, without stretching (see
// hal/tab5_camera.cpp).
//
// WHAT IS LEFT FOR LATER, on purpose:
//   - Gallery (view the photos taken without leaving for the Files app).
//   - Zoom/focus/exposure settings.
// None of that is hard later - it is just more than fitted in a single answer
// without becoming a wall of code untested on any hardware.

#include "app.h"
#include "idioma.h"
#include "tab5_camera.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "tab5_bmp.hpp"
#include "pintura.hpp"
#include "trabalho.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include "tab5_arquivos.hpp"   // listing a folder belongs to the system
#include "tab5_camera_web.hpp"

static const char *TAG = "app.camera";

static uint16_t* s_foto   = nullptr;   // the photo being captured (TAB5_CAM_FOTO_L x _A, PSRAM) - owned by the app until it becomes a job
static char      s_foto_nome[32];
static volatile bool s_gravou_ok = false;   // written by the job (static: exists even with the app closed)
static int       s_largura, s_altura;
static uint32_t  s_fotos = 0;

// CAPTURANDO: the camera is taking the photo into s_foto (one frame, ~33 ms).
// GRAVANDO:   a DETACHED JOB writes the photo to the SD card (seconds).
// The app does NOT block navigation in either. When it is time to write, the
// buffer BECOMES THE JOB'S (sistema_trabalho_solto): it writes and frees it.
// That is why closing the app mid-write is instant - before, closing waited
// for the whole write and the UI froze.
typedef enum { ESTADO_NORMAL, ESTADO_CAPTURANDO, ESTADO_GRAVANDO, ESTADO_FLASH } estado_botao_t;
static estado_botao_t s_estado = ESTADO_NORMAL;
static bool     s_flash_sucesso = false;
static uint32_t s_flash_ate_ms = 0;   // flashes the button border briefly after the result

// DIFFERENTIAL PAINTING: the button is only repainted when its text changes
// (or on a full pass). Before, it was drawn whole - large text, on every
// camera frame, ~30 times per second - without anything changing.
static pintura_t s_p;
static int       s_botao_pintado = -1;
static bool      s_tem_quadro = false;   // some frame has already arrived since the camera was (re)started

// CAPTURAR (capture) button: fixed bottom strip, independent of the area size
// (it varies little between landscape/portrait, but the fractional maths
// avoids a magic number).
#define BOTAO_A 96

// HIGHEST "foto_NNNN.bmp" ALREADY ON THE SD CARD - observed bug: s_fotos
// started from zero on every OPENING of the app, so each new session
// overwrote "foto_0001.bmp" from the previous session. The numbering has to
// come from what already exists on the card, in the SAME spirit that
// tab5_mesh_carregar_do_cartao() already uses to rebuild the conversation list
// from the files - the card is the source of truth, not a variable in RAM that
// dies when the app closes.
static uint32_t maior_foto_existente(void)
{
  char pasta[128];
  if (!tab5_sd_pasta("Camera", pasta, sizeof(pasta))) { return 0; }
  return tab5_arq_maior_numero(pasta, "foto_%04lu.bmp");   // 0 = very first photo
}

// THE COUNT BELONGS TO THE WORKER: the readdir of the Camera folder used to
// run in ao_abrir, on the UI task - with hundreds of photos, the camera
// took a while to appear. The number is only needed at CAPTURE time; until it
// arrives (AVISO_TRABALHO), the button waits.
static volatile uint32_t s_fotos_lidas = 0;
static volatile bool     s_fotos_prontas = false;   // the job finished (result in s_fotos_lidas)
static bool              s_fotos_ok = false;        // s_fotos is now valid

static void trabalho_contar(void*)
{
  s_fotos_lidas = maior_foto_existente();
  s_fotos_prontas = true;
}

// The preview rectangle, in SCREEN coordinates.
static area_t retangulo_preview(void)
{
  const area_t a = sistema_area();
  return area_t{ a.x, a.y, s_largura, s_altura };
}

// Turns the sensor on and points the preview DIRECTLY at the virtual screen:
// the camera's PPA writes the frame in place, and each frame requests a
// redraw - which only notifies the compositor (sistema_superficie_mudou). No
// copy by the CPU.
static bool ligar_camera(void)
{
  if (!tab5_camera_iniciar("Camera")) { return false; }
  superficie_t sup;
  if (!sistema_superficie(retangulo_preview(), &sup)) {
    ESP_LOGE(TAG, "no DMA surface for the preview");
    tab5_camera_parar("Camera");
    return false;
  }
  const tab5_cam_destino_t d = { sup.buf, sup.bytes, sup.pic_l, sup.pic_a, sup.x, sup.y,
                                 s_largura, s_altura, sistema_pedir_redesenho };
  tab5_camera_definir_destino(&d);
  return true;
}

static bool ao_abrir(void)
{
  const area_t a = sistema_area();
  s_largura = a.largura;
  // Multiple of 8: required by the DMA surface (sistema_superficie).
  s_altura  = (a.altura - BOTAO_A - 12) & ~7;
  if (s_largura <= 0 || s_altura <= 0) { return false; }

  if (!ligar_camera()) { return false; }

  s_fotos_ok = false;
  s_fotos_prontas = false;
  if (!sistema_trabalho(trabalho_contar, nullptr)) { s_fotos = 0; s_fotos_ok = true; }
  s_estado = ESTADO_NORMAL;
  s_flash_ate_ms = 0;
  s_botao_pintado = -1;
  s_tem_quadro = false;
  ESP_LOGI(TAG, "opened (preview %dx%d)", s_largura, s_altura);
  return true;
}

static void ao_fechar(void)
{
  tab5_camera_parar("Camera");   // waits for the camera task: no PPA writing (screen, s_foto) after this point
  // Only exists if the capture never became a job; one that did belongs to the job.
  if (s_foto) { heap_caps_free(s_foto); s_foto = nullptr; }
  ESP_LOGI(TAG, "closed");
}

// IDLE: no hook. With the screen off the SYSTEM pauses this app's camera and
// restarts it on wake, with the same destination (kernel/core.cpp,
// tab5_camera_pausar). A photo that was being captured comes back as a
// failure (quadro_pronto with ok = false) and the button leaves
// "CAPTURANDO..." by itself.

// THE WRITE REQUEST carries everything it needs: the job is detached and may
// run with the app already closed (or reopened, with another s_foto_nome).
typedef struct {
  uint16_t* px;
  char      nome[32];
} gravacao_t;

// On the system worker: writes the photo (whole frame) and frees everything.
static void trabalho_gravar(void* arg)
{
  gravacao_t* g = (gravacao_t*)arg;
  s_gravou_ok = tab5_bmp_salvar("Camera", g->nome, g->px, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A);
  heap_caps_free(g->px);
  heap_caps_free(g);
}

// Hands s_foto to a detached job. false = it failed (s_foto stays the app's).
static bool gravar_foto(void)
{
  gravacao_t* g = (gravacao_t*)heap_caps_malloc(sizeof(gravacao_t), MALLOC_CAP_DEFAULT);
  if (!g) { return false; }
  g->px = s_foto;
  snprintf(g->nome, sizeof(g->nome), "%s", s_foto_nome);
  if (!sistema_trabalho_solto(trabalho_gravar, g)) { heap_caps_free(g); return false; }
  s_foto = nullptr;
  return true;
}

static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_fotos_prontas && !s_fotos_ok) {
    s_fotos = s_fotos_lidas;
    s_fotos_ok = true;
    ESP_LOGI(TAG, "%u photo(s) already on the SD card", (unsigned)s_fotos);
  }
  if ((av & AVISO_TRABALHO) && s_estado == ESTADO_GRAVANDO) {
    s_estado = ESTADO_FLASH;
    s_flash_sucesso = s_gravou_ok;
    s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
  }
  sistema_pedir_redesenho();
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{
  pintura_iniciar(&s_p);
  // The frame is already on the virtual screen (the camera's PPA wrote it
  // there); the compositor only needs to know the area changed.
  if (tab5_camera_quadro_escrito()) {
    s_tem_quadro = true;
    sistema_superficie_mudou(retangulo_preview());
  } else if (sistema_area_foi_limpa()) {
    // Still no frame at all (sensor has just come up) - show a notice instead of
    // leaving the area black, looking frozen.
    tab5_texto(d, 2);
    d.setTextDatum(middle_center);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(tr("iniciando a câmera..."), a.x + s_largura / 2, a.y + s_altura / 2);
    d.setTextDatum(top_left);
  }

  // THE PHOTO HAS REACHED THE BUFFER: the write goes to the system worker.
  if (s_estado == ESTADO_CAPTURANDO) {
    bool ok = false;
    if (tab5_camera_quadro_pronto(s_foto, &ok)) {
      if (ok && gravar_foto()) {
        s_estado = ESTADO_GRAVANDO;
      } else {
        s_estado = ESTADO_FLASH;
        s_flash_sucesso = false;
        s_flash_ate_ms = pdTICKS_TO_MS(xTaskGetTickCount()) + 600;
      }
    }
  } else if (s_estado == ESTADO_FLASH && pdTICKS_TO_MS(xTaskGetTickCount()) >= s_flash_ate_ms) {
    s_estado = ESTADO_NORMAL;
  }

  // ---- bottom bar: CAPTURAR button + counter -----------------------
  // Only when what it says has changed: a new camera frame does not touch it.
  const int visual = (s_estado == ESTADO_FLASH) ? (s_flash_sucesso ? 10 : 11) : (int)s_estado;
  if (!pintura_mudou_i(&s_p, &s_botao_pintado, visual)) { return; }
  const int by = a.y + s_altura + 12;
  const char* texto = tr("CAPTURAR");
  // Main button highlighted; busy in neutral grey; result in green/red.
  // Dark ink on the green, because white on it is too faint to read.
  uint16_t    cor    = TEMA_DESTAQUE;
  uint16_t    tinta  = TEMA_SOBRE_DESTAQUE;
  if (s_estado == ESTADO_CAPTURANDO)                    { texto = tr("CAPTURANDO...");      cor = TEMA_SUPERFICIE_ALTA; tinta = TEMA_TEXTO; }
  else if (s_estado == ESTADO_GRAVANDO)                 { texto = tr("SALVANDO...");        cor = TEMA_SUPERFICIE_ALTA; tinta = TEMA_TEXTO; }
  else if (s_estado == ESTADO_FLASH && s_flash_sucesso) { texto = tr("CAPTURADO!");         cor = TEMA_SUCESSO;         tinta = TEMA_FUNDO; }
  else if (s_estado == ESTADO_FLASH)                    { texto = tr("FALHOU");             cor = TEMA_PERIGO;          tinta = TEMA_SOBRE_DESTAQUE; }

  tab5_botao(d, a.x + 16, by, a.largura - 32, BOTAO_A - 12,
             texto, cor, tinta, TAB5_BOTAO_TEXTO_GRANDE);

  // WHILE A REQUEST IS PENDING, the app has to keep checking -
  // periodo() below turns on the 100 ms clock only then.
}

// THE PACE IS THE CAMERA'S. Each frame written already requests a redraw
// (tab5_cam_destino_t.avisar): a 100 ms clock ON TOP of that only wedged
// passes with no new frame between those that had one - the UI busy right when
// the frame arrives, and the preview moving in jerks. The Camera app in
// Berry, which only redraws on a frame, was smoother than this one. The clock
// stays for what does not come from the camera: the photo in progress, the
// "CAPTURADO!" that disappears by itself, and the first frame not yet arrived.
static uint32_t periodo(void)
{
  return (s_estado != ESTADO_NORMAL || !s_tem_quadro) ? 100 : 0;
}

static void ao_tocar(int x, int y)
{
  const int by = s_altura + 12;
  if (y < by || y >= by + BOTAO_A - 12) { return; }
  // ALSO BLOCKS DURING THE FLASH ("CAPTURADO!"/"FALHOU", 600ms) - not just
  // during ESTADO_CAPTURANDO. Found while reviewing "what if it is pressed
  // repeatedly": a tap during the flash got past the old guard (which only
  // looked at CAPTURANDO), started a SECOND capture and locked the START
  // again - seen from outside as "the button does not return to
  // normal", when in fact it had returned and been locked again by a
  // tap nobody was sure had happened.
  if (s_estado != ESTADO_NORMAL) { return; }
  if (!s_fotos_ok) { return; }   // the numbering is still being read from the SD card (a moment)

  // One buffer per photo: the previous one went with the write job.
  if (!s_foto) { s_foto = tab5_camera_quadro_alocar(TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A); }
  if (!s_foto) { ESP_LOGE(TAG, "no PSRAM for the photo"); return; }
  if (!tab5_camera_pedir_quadro(s_foto, TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A)) { return; }
  snprintf(s_foto_nome, sizeof(s_foto_nome), "foto_%04lu", (unsigned long)(s_fotos + 1));
  ++s_fotos;
  s_estado = ESTADO_CAPTURANDO;

  // SHUTTER "CLICK" - at the moment of the tap, as on a real camera:
  // confirms "I captured THIS instant". 600 Hz ("I found it shrill").
  sistema_beep(600.0f, 40);
  sistema_pedir_redesenho();
}

// ---------------------------------------------------------------------------
// HOME SCREEN ICON - camera body + lens, primitives.
// ---------------------------------------------------------------------------
static void icone_camera(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t fundo = d.color565(45, 40, 20);
  const uint16_t corpo = TFT_ORANGE;

  d.fillRoundRect(x, y, lado, lado, lado / 6, fundo);

  const int cx = x + lado / 2, cy = y + lado / 2 + (int)(0.06f * lado);
  const int corpo_l = (int)(0.62f * lado), corpo_a = (int)(0.40f * lado);
  d.fillRoundRect(cx - corpo_l / 2, cy - corpo_a / 2, corpo_l, corpo_a, lado / 14, corpo);

  // Protruding "viewfinder" on top, on the left of the body.
  const int vis_l = (int)(0.18f * lado), vis_a = (int)(0.10f * lado);
  d.fillRoundRect(cx - corpo_l / 2 + (int)(0.06f * lado), cy - corpo_a / 2 - vis_a + 2,
                   vis_l, vis_a, lado / 20, corpo);

  // Lens: two concentric circles.
  const int r_ext = (int)(0.16f * lado), r_int = (int)(0.10f * lado);
  d.fillCircle(cx, cy, r_ext, fundo);
  d.drawCircle(cx, cy, r_ext, corpo);
  d.fillCircle(cx, cy, r_int, corpo);
}

extern const app_t APP_CAMERA = {
  .nome  = "Camera",
  .cor   = TFT_ORANGE,
  // NO .camera=true HERE - the camera became a service (reference
  // counted, see the big notice in hal/tab5_camera.hpp) and drives its
  // own CAM_RST inside tab5_camera_iniciar(), in the same
  // spirit that Grove already does for GPS/Mesh. Declaring it here as well
  // would only be redundant (the kernel would still apply it before ao_abrir(),
  // but what really guarantees the power now is the service itself).
  //
  // .alto_falante=true (together with the shutter click in
  // ao_tocar()): unlike the camera, the speaker has NO way to
  // declare itself after boot (reserving the ES8388's I2S pins is a
  // PHASE 1 decision, before M5.begin() - see the big notice in
  // ajustes.h). This app now uses real sound, so it declares it - even
  // though the Ajustes app declares the same today, it is a union (OR), not a conflict.
  .perfil = { .alto_falante = true },
  .usa_sd = true,
  .exige_sd = true,         // without an SD card there is nothing to do (see app.h)
  .orientacao = ORI_FIXO_EM_PE,   // the preview is DMA to the screen in rotation 0: not even the keyboard lies flat
  .ao_abrir   = ao_abrir,
  .ao_fechar  = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  // THE PREVIEW DOES NOT DEPEND ON THE CLOCK: each frame written by the
  // camera requests a redraw straight away (tab5_cam_destino_t.avisar). The
  // period only drives the button (CAPTURANDO/SALVANDO/flash) when there is no frame.
  .periodo_ms = 100,
  .periodo_agora = periodo,           // 0 with the preview running (the frame is the clock)
  .avisos    = AVISO_TRABALHO,   // the photo write has finished
  .ao_aviso  = ao_aviso,
  .icone = icone_camera,
  .ao_girar = nullptr,
  .comandos = nullptr,
  .fora_da_home = false,
  .no_pc = tab5_camera_web_ligar,   // the live camera in the browser (/camera/)
  .fundo_tema = true,               // the system clears the area with the theme background
};
