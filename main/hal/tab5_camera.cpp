#include "tab5_camera.hpp"
#include "tab5_marcos.hpp"
#include "tarefa_app.h"
#include "tab5_power.hpp"
#include "tab5_mutex.hpp"
#include "recursos.h"   // the camera declares what it uses: the system decides on suspension

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_cam_sensor.h"
#include "esp_video_caps.h"
#include "linux/videodev2.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>

static const char* TAG = "tab5_camera";

// The esp_video driver's sensor. The function is global, but the header
// declaring it is private to the component (esp_video_device_internal.h).
extern "C" esp_cam_sensor_device_t* esp_video_get_csi_video_device_sensor(void);

#define TAB5_CAM_NBUF 2
// FRAMES ON REQUEST at the same time (see tab5_camera_pedir_quadro): an app's
// photo and the browser's video do not compete for a turn.
#define TAB5_CAM_PEDIDOS 2

// THE SENSOR IS MOUNTED SIDEWAYS on the board - the same BSP_CAMERA_ROTATION=270
// as the official BSP. Without this the image would come out rotated 90 degrees.
#define TAB5_CAM_ROTACAO PPA_SRM_ROTATION_ANGLE_270

typedef struct {
  int      fd;
  uint8_t* bruto[TAB5_CAM_NBUF];   // the driver's MMAP buffers (pipeline format, see below)
  size_t   bruto_tam;
  uint32_t bruto_largura, bruto_altura;

  ppa_client_handle_t ppa;
  SemaphoreHandle_t   ppa_fim;   // given by the PPA ISR at the end of each operation (see ppa_rodar)

  // THE PREVIEW: where it is written (null buf = nowhere) and, for its size,
  // the uniform scale and the central piece of the raw frame (see encaixar).
  // Computed when the destination is set - the size belongs to the
  // destination, not the camera: whoever turns the camera on does not lock
  // anyone else's resolution.
  tab5_cam_destino_t destino;
  int       recorte_w, recorte_h;
  float     escala;
  bool      tem_novo;
  SemaphoreHandle_t trava;

  tarefa_app_t tarefa;
  bool ligado;
  bool video_iniciado;   // esp_video_init() succeeded - needs esp_video_deinit() in parar()

  // REQUESTED FRAMES - see tab5_camera_pedir_quadro(). Protected by 'trava'.
  // null destino = free slot; pronto = the task has written it (the requester
  // collects it with tab5_camera_quadro_pronto, which frees the slot).
  struct { uint16_t* destino; int l, a; bool em_pe, pronto, ok; } pedidos[TAB5_CAM_PEDIDOS];
} tab5_camera_t;

static tab5_camera_t s_cam = { .fd = -1 };

// The capture task was abandoned at shutdown (it did not confirm exit): its
// resources are deliberately leaked and the camera will not open again until
// a reboot. OUTSIDE s_cam, which iniciar_hardware() zeroes on every open.
static bool s_abandonada = false;

static inline void travar(void)    { if (s_cam.trava) { xSemaphoreTake(s_cam.trava, portMAX_DELAY); } }
static inline void destravar(void) { if (s_cam.trava) { xSemaphoreGive(s_cam.trava); } }

// ---------------------------------------------------------------------------
// REFERENCE COUNTING - see the big notice in tab5_camera.hpp.
// ---------------------------------------------------------------------------
// ITS OWN LOCK, separate from 's_cam.trava' (which only protects the FRAME
// FIELDS - indice_pronto/tem_novo/pedido - read and written on every pass of
// the capture loop). This one protects the 0<->1 TRANSITION of s_refs.
//
// UPDATED COMMENT - the original reason ("the console's 'foto' command calls
// these functions from the 'console' task itself") IS NO LONGER TRUE: that
// caused a real I2C collision (see the big notice in tab5_camera.hpp) and was
// fixed with the kernel/core.cpp bridge (sistema_camera_iniciar/parar) -
// TODAY, every call to tab5_camera_iniciar/parar() happens IN the UI task,
// directly (Camera app) or served by the bridge (console), never both at once
// from different tasks. The lock stays, on purpose, as a CHEAP safety net
// against someone one day calling these functions directly from a third task,
// breaking the contract - not protecting would cost almost nothing today, but
// removing it is betting that nobody will ever break the contract, and that
// bet has already been lost once in this very file.
static Trava s_ctrl;
static int               s_refs = 0;

static inline void travar_ctrl(void)    { s_ctrl.travar(); }
static inline void destravar_ctrl(void) { s_ctrl.destravar(); }

bool tab5_camera_ativa(void) { return s_refs > 0; }

// Likewise - iniciar_hardware() calls it on the error path (goto erro),
// before parar_hardware() is defined further down the file.
static void parar_hardware(void);

// THE PPA IN BLOCKING MODE DOES NOT SURVIVE SHUTDOWN (reading
// esp_driver_ppa/src/ppa_core.c). In BLOCKING mode the driver waits with
// xSemaphoreTake(trans_elm->sem, portMAX_DELAY) and IGNORES the result. But
// tarefa_app_encerrar() wakes the task with xTaskAbortDelay() - and, if it is
// in that wait, the driver returns "ESP_OK" with the operation STILL RUNNING
// in hardware. From then on: QBUF of a buffer the PPA is still reading, and
// ppa_unregister_client() refusing (the transaction still counts) - that,
// and not just the ISR race, was the source of the exhausted retries seen in
// testing.
//
// The way out: NON-blocking mode, with OUR OWN semaphore given by the ISR,
// and a wait that, if aborted, simply waits again. The task only leaves the
// PPA when the hardware has really finished.
static bool IRAM_ATTR ao_fim_ppa(ppa_client_handle_t, ppa_event_data_t*, void* sem)
{
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &acordou);
  return acordou == pdTRUE;
}

static esp_err_t ppa_rodar(ppa_srm_oper_config_t* cfg)
{
  cfg->mode      = PPA_TRANS_MODE_NON_BLOCKING;
  cfg->user_data = s_cam.ppa_fim;
  const esp_err_t err = ppa_do_scale_rotate_mirror(s_cam.ppa, cfg);
  if (err != ESP_OK) { return err; }
  // An operation takes a few ms; 1 s only runs out if the PPA has hung for good.
  for (int i = 0; i < 20; ++i) {
    if (xSemaphoreTake(s_cam.ppa_fim, pdMS_TO_TICKS(50)) == pdTRUE) { return ESP_OK; }
  }
  return ESP_ERR_TIMEOUT;
}

// THE SENSOR'S BLACK LEVEL. The SC202CS delivers black on a pedestal: its
// black-level loop aims at the target in registers 0x3907/0x3908, which the
// driver's initialisation table leaves at 0x041 (65 in 10 bits, 16 in RAW8).
// The component's calibration counts on that ("blc" block of 16 in the JSON)
// - but the subtraction is done by the ISP's BLC block, which only exists on
// P4 v3 (ESP_VIDEO_ISP_DEVICE_BLC, esp_video_caps.h). Below v3 the pedestal
// passes straight through: it becomes a fixed offset added to EVERY pixel,
// then multiplied by the white balance and the CCM. With daylight gains that
// gives 16 x (2.9 ; 0.4 ; 2.0) in linear - black comes out dark purple and
// the whole image gets a magenta veil, strongest in the shadows (gamma
// stretches the bottom of the scale). The way out is to ask the sensor itself
// for black at zero. After STREAMON: setting the format rewrites the
// initialisation table.
#define TAB5_CAM_PRETO_REG   0x3908
#define TAB5_CAM_PRETO_ALVO  0x00   // factory default 0x41

static void zerar_pedestal(void)
{
#if !ESP_VIDEO_ISP_DEVICE_BLC
  esp_cam_sensor_device_t* sensor = esp_video_get_csi_video_device_sensor();
  if (!sensor) { return; }
  esp_cam_sensor_reg_val_t r = { .regaddr = TAB5_CAM_PRETO_REG, .value = TAB5_CAM_PRETO_ALVO };
  const esp_err_t err = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r);
  if (err != ESP_OK) { ESP_LOGW(TAG, "black-level target on the sensor: %s", esp_err_to_name(err)); }
#endif
}

bool tab5_camera_reg(uint16_t reg, int valor, uint8_t* lido)
{
  if (!s_cam.ligado) { return false; }
  esp_cam_sensor_device_t* sensor = esp_video_get_csi_video_device_sensor();
  if (!sensor) { return false; }
  esp_cam_sensor_reg_val_t r = { .regaddr = reg, .value = 0 };
  if (valor >= 0) {
    r.value = (uint32_t)valor;
    if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r) != ESP_OK) { return false; }
  }
  if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_G_REG, &r) != ESP_OK) { return false; }
  if (lido) { *lido = (uint8_t)r.value; }
  return true;
}

// UNIFORM SCALE, FILLING: the same scale on both axes (the image does not
// stretch) and the smallest, in steps of 1/16 (the PPA's precision), that
// still covers the whole l x a; whatever is left of the frame is cropped
// equally from both sides. Rotated 270 (portrait), the output width comes
// from the raw frame's HEIGHT and the height from its WIDTH; unrotated (as
// the sensor sees), each axis from its own. For the whole frame (1200x1600
// portrait) the scale is 1 and nothing is cropped.
static void encaixar(int l, int a, bool em_pe, float* escala, int* rw, int* rh)
{
  const int W = (int)s_cam.bruto_largura, H = (int)s_cam.bruto_altura;
  const int lw = em_pe ? a : l, lh = em_pe ? l : a;   // what comes out of each raw axis
  int s16 = (lw * 16 + W - 1) / W;
  const int s16h = (lh * 16 + H - 1) / H;
  if (s16h > s16) { s16 = s16h; }
  if (s16 < 1) { s16 = 1; }
  *escala = (float)s16 / 16.0f;
  *rw = (lw * 16) / s16; if (*rw > W) { *rw = W; }
  *rh = (lh * 16) / s16; if (*rh > H) { *rh = H; }
}

// THE CAPTURE TASK. DQBUF blocks until the sensor delivers the next frame
// (free-running, ~30fps) - which is why it lives in tarefa_app_t and not in
// the UI task: if the sensor hangs, ONLY this task gets stuck (see the big
// notice in kernel/tarefa_app.h about "deliberately abandoned" - the same
// reasoning that applies to Mesh/GPS applies here).
static void laco_captura(tarefa_app_t* t)
{
  const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int64_t t_conta = esp_timer_get_time();
  int quadros = 0;

  while (tarefa_app_ativa(t)) {
    struct v4l2_buffer buf = { };
    buf.type   = tipo;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
      // xTaskAbortDelay does NOT wake a blocked ioctl - if we got here from a
      // real error (not because we asked it to exit), avoid a busy loop.
      if (tarefa_app_ativa(t)) { vTaskDelay(pdMS_TO_TICKS(50)); }
      continue;
    }

    // The basis of every operation: the sensor's raw frame (1600x1200),
    // rotated 270 (the sensor is mounted sideways - BSP_CAMERA_ROTATION=270)
    // and mirrored (mirror_x applies to the OUTPUT block, after rotation -
    // left/right were swapped).
    ppa_srm_oper_config_t base = { };
    base.in.buffer   = s_cam.bruto[buf.index];
    base.in.pic_w    = s_cam.bruto_largura;
    base.in.pic_h    = s_cam.bruto_altura;
    base.in.block_w  = s_cam.bruto_largura;
    base.in.block_h  = s_cam.bruto_altura;
    base.in.srm_cm   = PPA_SRM_COLOR_MODE_RGB565;
    base.out.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    base.rotation_angle = TAB5_CAM_ROTACAO;
    base.mirror_x    = true;

    travar();
    const tab5_cam_destino_t dst = s_cam.destino;
    const int rec_w = s_cam.recorte_w, rec_h = s_cam.recorte_h;
    const float escala = s_cam.escala;
    struct { uint16_t* destino; int l, a; bool em_pe; } ped[TAB5_CAM_PEDIDOS];
    for (int i = 0; i < TAB5_CAM_PEDIDOS; ++i) {
      const auto& p = s_cam.pedidos[i];
      ped[i] = { p.destino && !p.pronto ? p.destino : nullptr, p.l, p.a, p.em_pe };
    }
    destravar();

    // PREVIEW STRAIGHT INTO THE DESTINATION: the PPA writes the frame already
    // rotated and scaled into the rectangle the app specified - in practice,
    // inside the system's VIRTUAL SCREEN. Before, there were four trips
    // through PSRAM per frame (PPA -> camera buffer -> app memcpy -> pushImage
    // onto the virtual screen -> compositor); added to the panel, the
    // bandwidth did not add up at 30 fps and the image arrived late. Now
    // there are two (PPA -> virtual screen -> compositor).
    //
    // Uniform scale, filling (encaixar): in the Tab5's portrait this crops
    // about 4% of the width - the rest of the frame is what the photo saves.
    esp_err_t err = ESP_OK;
    if (dst.buf) {
      ppa_srm_oper_config_t cfg = base;
      cfg.out.buffer         = dst.buf;
      cfg.out.buffer_size    = dst.bytes;
      cfg.out.pic_w          = dst.pic_l;
      cfg.out.pic_h          = dst.pic_a;
      cfg.out.block_offset_x = dst.x;
      cfg.out.block_offset_y = dst.y;
      cfg.in.block_w         = rec_w;
      cfg.in.block_h         = rec_h;
      cfg.in.block_offset_x  = (s_cam.bruto_largura - rec_w) / 2;
      cfg.in.block_offset_y  = (s_cam.bruto_altura  - rec_h) / 2;
      cfg.scale_x = cfg.scale_y = escala;
      err = ppa_rodar(&cfg);
    }

    // REQUESTED FRAMES (the photo, the browser video): the SAME raw frame, at
    // the requester's size, with the same scaling rule. One PPA operation each
    // (a few ms): the preview does not stop, and what to do with the frame
    // (save, compress) is up to the requester.
    for (int i = 0; i < TAB5_CAM_PEDIDOS; ++i) {
      if (!ped[i].destino) { continue; }
      float e;
      int rw, rh;
      encaixar(ped[i].l, ped[i].a, ped[i].em_pe, &e, &rw, &rh);
      ppa_srm_oper_config_t fc = base;
      if (!ped[i].em_pe) {   // as the sensor sees: no rotation or mirroring (the displayer does it)
        fc.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        fc.mirror_x = false;
      }
      fc.out.buffer      = ped[i].destino;
      fc.out.buffer_size = tab5_camera_quadro_bytes(ped[i].l, ped[i].a);
      fc.out.pic_w       = ped[i].l;
      fc.out.pic_h       = ped[i].a;
      fc.in.block_w        = rw;
      fc.in.block_h        = rh;
      fc.in.block_offset_x = (s_cam.bruto_largura - rw) / 2;
      fc.in.block_offset_y = (s_cam.bruto_altura  - rh) / 2;
      fc.scale_x = fc.scale_y = e;
      // ONE LINE IN EVERY TWO (landscape only, reducing to half or less). The
      // PPA costs what it READS: ~110 ms for the ~3 MB of the crop (measured
      // 05/11/2026), far above the ~32 ms between frames. Declared at double
      // width, the raw frame becomes half the lines, each holding two sensor
      // lines side by side - and the block, one line wide, reads only the
      // first of each pair. The vertical scale doubles so the result has the
      // same number of lines. Half the PSRAM read; in a 480-line video taken
      // from ~1100, the difference is invisible.
      if (!ped[i].em_pe && e <= 0.5f) {
        fc.in.pic_w          = s_cam.bruto_largura * 2;
        fc.in.pic_h          = s_cam.bruto_altura / 2;
        fc.in.block_h        = rh / 2;
        fc.in.block_offset_y = (s_cam.bruto_altura - rh) / 4;
        fc.scale_y           = e * 2;
      }
      const esp_err_t ef = ppa_rodar(&fc);
      travar();
      if (s_cam.pedidos[i].destino == ped[i].destino) {
        s_cam.pedidos[i].ok     = (ef == ESP_OK);
        s_cam.pedidos[i].pronto = true;
      }
      destravar();
      if (ef != ESP_OK) { ESP_LOGW(TAG, "frame %dx%d: PPA failed (%s)", ped[i].l, ped[i].a, esp_err_to_name(ef)); }
    }

    // Return the RAW buffer to the driver NOW - the sensor can reuse the slot.
    ioctl(s_cam.fd, VIDIOC_QBUF, &buf);

    if (dst.buf && err == ESP_OK) {
      travar();
      s_cam.tem_novo = true;
      destravar();
      if (dst.avisar) { dst.avisar(); }
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "PPA failed: %s", esp_err_to_name(err)); }

    // Real frames per second, every 10 s - the number that tells whether the
    // sensor (or auto exposure, which lengthens the frame in the dark) is
    // delivering 30 fps.
    ++quadros;
    const int64_t agora = esp_timer_get_time();
    if (agora - t_conta >= 10000000) {
      ESP_LOGI(TAG, "%.1f frames/s", quadros * 1e6 / (double)(agora - t_conta));
      t_conta = agora;
      quadros = 0;
    }
  }
}

// THE REAL INITIALISATION - only runs on the 0->1 transition of s_refs (see
// tab5_camera_iniciar() further down, the PUBLIC function that decides when
// to call this one).
static bool iniciar_hardware(void)
{
  // The capture task from a previous session did not exit and is still using
  // esp_video, the PPA and the buffers (see parar_hardware): bringing it all
  // up again on top would fail ("/dev/video0 already registered") or, worse,
  // zero the s_cam it is still using. Refuse with a clear message; only a
  // reboot fixes it.
  if (s_abandonada) {
    ESP_LOGE(TAG, "camera unavailable: the previous capture task did not exit - reboot the device");
    return false;
  }
  s_cam = { .fd = -1 };
  s_cam.trava = xSemaphoreCreateMutex();
  if (!s_cam.trava) { return false; }

  // THE CAMERA POWERS ITSELF - see the big notice in the .hpp ("THE CAMERA
  // NOW POWERS ITSELF"). BEFORE anything else, even before the I2C handle:
  // the sensor's SCCB (used just below, inside esp_video_init) only responds
  // with CAM_RST out of reset.
  tab5_power_camera(true);

  // GIVE THE SENSOR TIME TO WAKE - found on hardware testing 'foto' from the
  // CONSOLE: "Get sensor ID failed" on the first SCCB, every time.
  //
  // The SC202CS driver (sc202cs.c, sc202cs_hw_reset) DOES have a 10ms delay
  // after releasing reset - but ONLY when 'reset_pin' is a GPIO the driver
  // itself controls. Our CAM_RST is external (I2C expander, see
  // tab5_power_camera()), so we pass GPIO_NUM_NC for reset_pin/pwdn_pin (see
  // csi_cfg below) - the driver never knows we have just taken the sensor out
  // of reset, and skips its own delay. WE have to provide that margin, and
  // until now it always "happened" by accident: the Camera app was still
  // mounting the card (usa_sd=true) and drawing "abrindo..." (opening...)
  // before ao_abrir() got here, tens of ms to spare without anyone asking.
  // The 'foto' command calls this function directly, with none of those steps
  // in between - and only then did the lack of an EXPLICIT delay show. 20ms:
  // double what the driver itself uses for the case it knows, a comfortable
  // margin without noticeably slowing the Camera app's opening.
  vTaskDelay(pdMS_TO_TICKS(20));

  // THE BUS IS THE SAME AS THE REST OF THE SYSTEM'S (M5.In_I2C) - see the big
  // notice in the .hpp. Take the handle already installed, do NOT create one.
  i2c_master_bus_handle_t i2c_handle = nullptr;
  if (i2c_master_get_bus_handle(I2C_NUM_1, &i2c_handle) != ESP_OK || !i2c_handle) {
    ESP_LOGE(TAG, "internal I2C bus (I2C_NUM_1) is not up - has M5.begin() run?");
    vSemaphoreDelete(s_cam.trava);
    s_cam.trava = nullptr;
    return false;
  }

  const esp_video_init_csi_config_t csi_cfg = {
    .sccb_config = {
      .init_sccb = false,
      .i2c_handle = i2c_handle,
      .freq = 400000,
    },
    .reset_pin = GPIO_NUM_NC,   // CAM_RST is the I2C expander, already applied via perfil.camera - see .hpp
    .pwdn_pin  = GPIO_NUM_NC,
  };
  esp_video_init_config_t video_cfg = { .csi = &csi_cfg };

  if (esp_video_init(&video_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init failed - check CONFIG_CAMERA_SC202CS in menuconfig");
    vSemaphoreDelete(s_cam.trava);
    s_cam.trava = nullptr;
    return false;
  }
  s_cam.video_iniciado = true;

  s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (s_cam.fd < 0) {
    ESP_LOGE(TAG, "opening %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    goto erro;
  }

  {
    // THE SENSOR'S DEFAULT FORMAT IS RAW8 (raw Bayer) - checked in sdkconfig
    // after downloading the component (CONFIG_CAMERA_SC202CS_MIPI_DEFAULT_FMT_RAW8_*).
    // That is the real sensor talking: a CMOS camera like this NEVER delivers
    // RGB directly, only the Bayer pattern. The demosaicing is done by the
    // chip's own ISP (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER, already
    // on) - but only when we ASK for RGB565 via VIDIOC_S_FMT on this SAME
    // /dev/video0. Without this step, the PPA just below would receive raw
    // Bayer and try to read it as RGB565 - the image would come out as
    // meaningless static, not a "weird" photo: raw sensor bytes, no colour.
    struct v4l2_format fmt = { };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
      ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
      goto erro;
    }
    ESP_LOGI(TAG, "sensor default format: %ux%u, fourcc 0x%08lx (RAW = Bayer, no ISP yet)",
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (unsigned long)fmt.fmt.pix.pixelformat);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      struct v4l2_format pedido = { };
      pedido.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      pedido.fmt.pix.width       = fmt.fmt.pix.width;
      pedido.fmt.pix.height      = fmt.fmt.pix.height;
      pedido.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
      if (ioctl(s_cam.fd, VIDIOC_S_FMT, &pedido) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(RGB565) failed - the ISP did not accept the conversion");
        goto erro;
      }
      // Confirm what actually took effect - the driver may adjust it.
      memset(&fmt, 0, sizeof(fmt));
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT (after S_FMT) failed");
        goto erro;
      }
    }

    s_cam.bruto_largura = fmt.fmt.pix.width;
    s_cam.bruto_altura  = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "capturing: %ux%u, fourcc 0x%08lx", (unsigned)s_cam.bruto_largura,
             (unsigned)s_cam.bruto_altura, (unsigned long)fmt.fmt.pix.pixelformat);
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
      ESP_LOGE(TAG, "driver stayed on fourcc 0x%08lx, not RGB565 - the PPA would receive garbage",
               (unsigned long)fmt.fmt.pix.pixelformat);
      goto erro;
    }

    // EXPOSURE AND COLOUR BELONG TO THE ISP. The fixed manual gain that was
    // here (index 16 = 1.5x) was a patch for the dark image; the cause was
    // the ISP pipeline controller being OFF in sdkconfig
    // (CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER): without it nothing
    // ran auto exposure (AGC/AE) or white balance (AWB) - the sensor
    // delivered "raw" Bayer, green (two green pixels in every four,
    // uncorrected) and at minimum gain. With it on, esp_video's "isp_task"
    // reads the ISP statistics every frame and adjusts the sensor's
    // gain/exposure (via SCCB, which goes through the I2C lock -
    // hal/tab5_sccb.cpp) and the ISP's colour gains/CCM/gamma, with the
    // SC202CS calibration that ships with the component. A fixed gain here
    // would only fight it.
    // The calibration in use is OUR OWN, hal/sc202cs_tab5.json (lower
    // exposure against blown-out whites) - the reason is in
    // sdkconfig.defaults, "Camera" section. The magenta white itself is fixed
    // in the colour matrix, by our own IPA: hal/tab5_camera_ipa.c.

    struct v4l2_requestbuffers req = { };
    req.count  = TAB5_CAM_NBUF;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
      ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
      goto erro;
    }

    for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
      struct v4l2_buffer buf = { };
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%d) failed", i);
        goto erro;
      }
      s_cam.bruto[i] = (uint8_t*)mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, s_cam.fd, buf.m.offset);
      if (!s_cam.bruto[i]) { ESP_LOGE(TAG, "mmap(%d) failed", i); goto erro; }
      s_cam.bruto_tam = buf.length;
      if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "initial VIDIOC_QBUF(%d) failed", i);
        goto erro;
      }
    }
  }

  {
    const ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM };
    if (ppa_register_client(&ppa_cfg, &s_cam.ppa) != ESP_OK) {
      ESP_LOGE(TAG, "ppa_register_client failed");
      goto erro;
    }
    s_cam.ppa_fim = xSemaphoreCreateBinary();
    const ppa_event_callbacks_t cbs = { .on_trans_done = ao_fim_ppa };
    if (!s_cam.ppa_fim || ppa_client_register_event_callbacks(s_cam.ppa, &cbs) != ESP_OK) {
      ESP_LOGE(TAG, "PPA callback failed");
      goto erro;
    }
  }

  {
    const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &tipo) != 0) {
      ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
      goto erro;
    }
  }
  zerar_pedestal();

  // Only DQBUF, PPA and QBUF - saving the photo moved out of here (see
  // tab5_camera_pedir_quadro), so the stack goes back to the original 4 KB.
  if (!tarefa_app_criar(&s_cam.tarefa, laco_captura, "cam", 4096, 3)) {
    ESP_LOGE(TAG, "could not start the capture task");
    { const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(s_cam.fd, VIDIOC_STREAMOFF, &tipo); }
    goto erro;
  }

  s_cam.ligado = true;
  tab5_marco("camera: on", 0);
  ESP_LOGI(TAG, "camera up (%ux%u, rotation %d)", (unsigned)s_cam.bruto_largura, (unsigned)s_cam.bruto_altura,
           (int)TAB5_CAM_ROTACAO);
  return true;

erro:
  parar_hardware();
  return false;
}

// THE REAL SHUTDOWN - only runs on the 1->0 transition of s_refs (see
// tab5_camera_parar() further down). Also called directly by
// iniciar_hardware() on the error path (goto erro above) - so it stays safe
// to call even with a half-done initialisation, exactly as this function's
// original comment already warned.
static void parar_hardware(void)
{
  tab5_marco("camera: turning off", 0);
  // WHAT SHUTTING DOWN COSTS - runs in the UI task (ao_fechar), so every ms
  // here is a frozen screen. The log says which step weighed most.
  const int64_t t0 = esp_timer_get_time();
  int64_t t_tarefa = t0, t_ppa = t0, t_video = t0;

  if (s_cam.ligado) {
    // The task only does DQBUF (blocks until the next frame, ~33 ms) and PPA
    // - no writes to the card. 500 ms is plenty of margin; if it does not
    // exit, tarefa_app_encerrar() abandons it (see tarefa_app.h).
    if (!tarefa_app_encerrar(&s_cam.tarefa, 500)) {
      // The abandoned task may be in the middle of DQBUF/PPA: freeing the PPA,
      // the mmap buffers, the fd, the lock or esp_video now would have it
      // writing into freed memory. None of it is freed. The sensor power also
      // stays on: cutting it with a read in progress can hang esp_video; the
      // cost is the power draw until a reboot. STREAMOFF is also skipped (it
      // is unknown whether it is safe with the task inside the driver).
      ESP_LOGE(TAG, "capture task did not exit within 500 ms - PPA, buffers, fd, esp_video and power "
                    "are leaked ON PURPOSE until reboot; the camera will not open again");
      s_abandonada = true;
      s_cam.ligado = false;
      return;
    }
    const int tipo = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &tipo);
  }
  s_cam.ligado = false;
  t_tarefa = esp_timer_get_time();

  // "client still has unprocessed trans" - ROOT CAUSE CONFIRMED by reading
  // the driver source (esp_driver_ppa/src/ppa_core.c), not just assumed:
  //
  //   ppa_transaction_done_cb() (the end-of-transaction ISR) does, IN ORDER,
  //   inside the SAME critical section:
  //     1. xSemaphoreGiveFromISR(trans_elm->sem, ...)   <- wakes whoever
  //        called ppa_do_scale_rotate_mirror (us, in blocking mode)
  //     2. client->trans_cnt--                          <- ONLY AFTERWARDS
  //
  //   ppa_unregister_client() refuses if trans_cnt != 0. In other words:
  //   there is a real window, a few instructions long, between "our blocking
  //   call returns control" and "the driver itself finishes counting that
  //   transaction as done". On SMP (the P4 is dual-core, and the "cam" task
  //   has no fixed affinity - tarefa_app_criar uses tskNO_AFFINITY) that
  //   window can really be crossed.
  //
  // AVOIDING IT IS NOT OPTIONAL - it is a race documented in the driver
  // itself, not a bug of ours to "fix properly"; short spaced retries ARE the
  // correct fix. What could be improved was the noise: a vTaskDelay(1) BEFORE
  // the first attempt (the margin the window really needs, just given on
  // purpose instead of discovered by trial and error) makes the 1st attempt
  // succeed almost every time; and since the error message comes from INSIDE
  // the driver (ESP_LOGE, not our log), the only way not to alarm anyone in
  // the log with an already-handled condition is to lower that TAG's level
  // just for the attempt and restore it afterwards.
  //
  // BUDGET ENLARGED - seen on hardware: the original 5 attempts (100ms of
  // margin) sometimes were NOT enough ("did not confirm" in the log, even
  // with the system carrying on alive afterwards). IT IS NOT JUST COSMETIC: I
  // read ppa_register_client()/ppa_unregister_client() in ppa_core.c, and
  // when 'do_unregister' is false the function returns BEFORE free()ing the
  // client (heap_caps_calloc'd) and before releasing the reference on the
  // shared SRM engine - i.e. running out of attempts really LEAKS that
  // structure, it does not just print an ugly warning. 20 attempts of 25ms
  // (max. half a second, and ONLY when the race really happens) is an
  // invisible price on camera close to greatly reduce the chance of running
  // out - the residual that can still leak if even that is not enough is
  // recorded here, not hidden: if this warning shows up again, the next step
  // is to investigate what could be delaying the PPA ISR by a whole HALF
  // SECOND, not to raise the number again.
  // (the other, larger source was the abort of the blocking wait, resolved
  // in ppa_rodar(). The retries remain for the ISR window.)
  if (s_cam.ppa) {
    vTaskDelay(pdMS_TO_TICKS(1));   // the margin the window documented above needs

    esp_log_level_set("ppa_core", ESP_LOG_NONE);
    esp_err_t err = ESP_FAIL;
    for (int tentativa = 0; tentativa < 20; ++tentativa) {
      err = ppa_unregister_client(s_cam.ppa);
      if (err == ESP_OK) { break; }
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_log_level_set("ppa_core", ESP_LOG_INFO);

    if (err != ESP_OK) { ESP_LOGW(TAG, "ppa_unregister_client did not confirm (%s) - carrying on anyway (PPA client leaked)", esp_err_to_name(err)); }
    s_cam.ppa = nullptr;
  }
  if (s_cam.ppa_fim) { vSemaphoreDelete(s_cam.ppa_fim); s_cam.ppa_fim = nullptr; }
  t_ppa = esp_timer_get_time();

  for (int i = 0; i < TAB5_CAM_NBUF; ++i) {
    if (s_cam.bruto[i]) { munmap(s_cam.bruto[i], s_cam.bruto_tam); s_cam.bruto[i] = nullptr; }
  }
  if (s_cam.fd >= 0) { close(s_cam.fd); s_cam.fd = -1; }

  // THE MISSING STEP (observed defect): esp_video_init() registers GLOBAL
  // devices (/dev/video0, and /dev/video20 "ISP" with the pipeline on) that
  // survive the fd being closed - without undoing that, the NEXT call to
  // esp_video_init() (on the next ao_abrir()) finds "/dev/video20 already
  // registered" and fails, and the app is stuck refusing to open until the
  // device reboots. esp_video_deinit() is the exact pair of esp_video_init().
  if (s_cam.video_iniciado) {
    esp_err_t err = esp_video_deinit();
    if (err != ESP_OK) { ESP_LOGW(TAG, "esp_video_deinit failed (%s)", esp_err_to_name(err)); }
    s_cam.video_iniciado = false;
  }
  t_video = esp_timer_get_time();

  if (s_cam.trava) { vSemaphoreDelete(s_cam.trava); s_cam.trava = nullptr; }
  s_cam.destino = { };
  s_cam.tem_novo = false;

  // EXACT PAIR of tab5_power_camera(true) at the start of iniciar_hardware()
  // - see the big notice in the .hpp. LAST: everything else has already been
  // torn down above, so there is no risk of cutting the sensor's power with
  // some other part of the shutdown still trying to talk SCCB to it.
  tab5_power_camera(false);

  const int64_t t_fim = esp_timer_get_time();
  ESP_LOGI(TAG, "off in %lld ms (task %lld, ppa %lld, esp_video %lld, power %lld)",
           (t_fim - t0) / 1000, (t_tarefa - t0) / 1000, (t_ppa - t_tarefa) / 1000,
           (t_video - t_ppa) / 1000, (t_fim - t_video) / 1000);
}

// ---------------------------------------------------------------------------
// PUBLIC API - the REFERENCE COUNTING lives only here (see the big notice in
// the .hpp). iniciar_hardware()/parar_hardware() above are never called
// outside these two functions.
// ---------------------------------------------------------------------------
// THE IDLE PAUSE (see tab5_camera_pausar in the .hpp). Only the UI task
// touches this (the kernel on idle, and iniciar/parar from the same owner).
static struct {
  bool ativa;
  char quem[24];
  tab5_cam_destino_t destino;   // the preview of whoever paused (null buf = had none)
  // Frames that died with the camera (it really stopped on pause): they come
  // back as ready-with-failure on unpause, so the requester does not wait for ever.
  struct { uint16_t* destino; int l, a; } mortos[TAB5_CAM_PEDIDOS];
} s_pausa;

static bool pausa_de(const char* quem)
{
  return s_pausa.ativa && quem && !strncmp(s_pausa.quem, quem, sizeof(s_pausa.quem) - 1);
}

static bool iniciar_interno(const char* quem);
static void parar_interno(const char* quem);

bool tab5_camera_iniciar(const char* quem)
{
  // The paused owner turned it back on itself (a script with the screen off):
  // the pause no longer makes sense, and it sets its own destination.
  if (pausa_de(quem)) { s_pausa = { }; }
  return iniciar_interno(quem);
}

void tab5_camera_parar(const char* quem)
{
  // The paused owner already released its use on pause: stopping again would discount another.
  if (pausa_de(quem)) { s_pausa = { }; return; }
  parar_interno(quem);
}

bool tab5_camera_pausar(const char* quem)
{
  if (s_pausa.ativa || !quem || !sistema_recurso_de(RECURSO_CAMERA, quem)) { return false; }
  s_pausa = { };
  travar();
  s_pausa.destino = s_cam.destino;
  destravar();
  // The preview stops NOW, even if another consumer keeps the camera on.
  tab5_camera_definir_destino(nullptr);
  travar_ctrl();
  const bool vai_parar = s_refs == 1;
  destravar_ctrl();
  if (vai_parar) {
    travar();
    for (int i = 0; i < TAB5_CAM_PEDIDOS; ++i) {
      const auto& p = s_cam.pedidos[i];
      if (p.destino && !p.pronto) { s_pausa.mortos[i] = { p.destino, p.l, p.a }; }
    }
    destravar();
  }
  parar_interno(quem);
  strlcpy(s_pausa.quem, quem, sizeof(s_pausa.quem));
  s_pausa.ativa = true;
  ESP_LOGI(TAG, "paused for idle: %s%s", quem, vai_parar ? " (turned off)" : "");
  return true;
}

void tab5_camera_despausar(bool religar)
{
  if (!s_pausa.ativa) { return; }
  const auto p = s_pausa;
  s_pausa = { };
  if (!religar) { return; }
  if (!iniciar_interno(p.quem)) {
    ESP_LOGE(TAG, "failed to turn %s's camera back on after idle", p.quem);
    return;
  }
  if (p.destino.buf) { tab5_camera_definir_destino(&p.destino); }
  travar();
  for (const auto& m : p.mortos) {
    if (!m.destino) { continue; }
    for (auto& v : s_cam.pedidos) {
      if (v.destino) { continue; }
      v = { m.destino, m.l, m.a, true, true, false };   // ready, with failure
      break;
    }
  }
  destravar();
  ESP_LOGI(TAG, "back on after idle: %s", p.quem);
}

static bool iniciar_interno(const char* quem)
{
  travar_ctrl();
  bool ok = true;
  if (s_refs > 0) { ++s_refs; }   // already on: just one more interested party
  else            { ok = iniciar_hardware(); if (ok) { s_refs = 1; } }
  destravar_ctrl();
  // THE CAMERA IS A RESOURCE (kernel/recursos.h): while it is on, suspension
  // holds the clock and does not enter light sleep - on behalf of whoever
  // asked, be it app, Berry, console or browser. Users need not know about
  // suspension.
  if (ok) { sistema_recurso_usar(RECURSO_CAMERA, quem ? quem : "camera"); }
  return ok;
}

static void parar_interno(const char* quem)
{
  sistema_recurso_soltar(RECURSO_CAMERA, quem ? quem : "camera");
  travar_ctrl();

  if (s_refs > 0) {
    --s_refs;
    if (s_refs == 0) { parar_hardware(); }
  }

  destravar_ctrl();
}

void tab5_camera_definir_destino(const tab5_cam_destino_t* d)
{
  if (!s_cam.trava) { return; }
  float e = 1.0f;
  int rw = 0, rh = 0;
  if (d && d->buf) { encaixar(d->l, d->a, true, &e, &rw, &rh); }
  travar();
  s_cam.destino = d ? *d : tab5_cam_destino_t{ };
  s_cam.escala = e;
  s_cam.recorte_w = rw;
  s_cam.recorte_h = rh;
  s_cam.tem_novo = false;
  destravar();
  if (d && d->buf) { ESP_LOGI(TAG, "preview %dx%d: scale %.4f, uses %dx%d of the frame", d->l, d->a, (double)e, rw, rh); }
}

bool tab5_camera_quadro_escrito(void)
{
  if (!s_cam.ligado) { return false; }
  travar();
  const bool novo = s_cam.tem_novo;
  s_cam.tem_novo = false;
  destravar();
  return novo;
}

size_t tab5_camera_quadro_bytes(int l, int a)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return ((size_t)l * a * 2 + alinhamento - 1) & ~(alinhamento - 1);
}

uint16_t* tab5_camera_quadro_alocar(int l, int a)
{
  size_t alinhamento = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alinhamento);
  return (uint16_t*)heap_caps_aligned_calloc(alinhamento, 1, tab5_camera_quadro_bytes(l, a), MALLOC_CAP_SPIRAM);
}

bool tab5_camera_pedir_quadro(uint16_t* destino, int l, int a, bool em_pe)
{
  if (!s_cam.ligado || !destino || l <= 0 || a <= 0) { return false; }
  bool aceito = false;
  travar();
  bool repetido = false;
  for (const auto& p : s_cam.pedidos) { repetido |= p.destino == destino; }
  for (auto& p : s_cam.pedidos) {
    if (repetido || aceito || p.destino) { continue; }
    p = { destino, l, a, em_pe, false, false };
    aceito = true;
  }
  destravar();
  return aceito;
}

bool tab5_camera_quadro_pronto(const uint16_t* destino, bool* ok)
{
  if (!s_cam.trava) { return false; }
  bool pronto = false;
  travar();
  for (auto& p : s_cam.pedidos) {
    if (p.destino != destino || !p.pronto) { continue; }
    if (ok) { *ok = p.ok; }
    p = { };
    pronto = true;
  }
  destravar();
  return pronto;
}
