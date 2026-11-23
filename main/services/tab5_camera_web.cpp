// A camera no navegador do PC - ver tab5_camera_web.hpp.
#include "tab5_camera_web.hpp"

#include <stdio.h>
#include <string.h>
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pool_tarefas.h"
#include "tab5_camera.hpp"
#include "tab5_hora.hpp"
#include "tab5_web.hpp"

static const char* TAG = "camera_web";

extern const char PAGINA_INICIO[] asm("_binary_tab5_camera_web_html_start");
extern const char PAGINA_FIM[]    asm("_binary_tab5_camera_web_html_end");

#define CAMERA_OCIOSA_MS 5000   // sem pedir quadro: a camera desliga
#define QUADRO_ESPERA_MS 1500   // teto da espera por um frame (o primeiro depois de ligar demora)

// Os tamanhos, multiplos de 16 (o bloco do JPEG com YUV420). O VIDEO vai
// DEITADO, como o sensor ve (4:3): girar no Tab5 custa ~117 ms por quadro, e
// a pagina gira de graca (ver tab5_camera_pedir_quadro). A FOTO, que vira
// arquivo, sai em pe.
typedef struct { int l, a, qualidade; bool em_pe; } tamanho_t;
static const tamanho_t VIDEO    = { 640, 480, 70, false };
static const tamanho_t VIDEO_HD = { 960, 720, 70, false };
static const tamanho_t FOTO     = { TAB5_CAM_FOTO_L, TAB5_CAM_FOTO_A, 92, true };

// TUDO SOB s_trava: a camera ligada, o codificador e os buffers (PSRAM) do
// tamanho em uso. Quem pede um quadro segura a trava ate responder - o
// desligamento por ociosidade espera ele terminar.
//
// DOIS QUADROS, UM ADIANTADO. Reduzir o quadro custa ~75 ms de PPA (medido
// 05/11/2026), e comprimir + enviar outros ~70: um depois do outro, ~6 q/s.
// Assim que um quadro fica pronto, o seguinte ja e pedido no outro buffer - a
// camera o prepara enquanto este e comprimido e enviado, e o proximo pedido o
// encontra pronto ou quase. So no video: a foto e uma de cada vez.
static SemaphoreHandle_t     s_trava = nullptr;
static bool                  s_ligada = false;
static uint32_t              s_ligou_ms = 0;
static jpeg_encoder_handle_t s_jpeg = nullptr;
static uint16_t*             s_buf[2] = { nullptr, nullptr };   // RGB565 do tamanho s_l x s_a
static int                   s_adiantado = -1;   // o buffer com o pedido ja feito (-1 = nenhum)
static uint8_t*              s_saida = nullptr;  // o JPEG
static size_t                s_saida_n = 0;
static int                   s_l = 0, s_a = 0;
static bool                  s_em_pe = false;
static uint32_t              s_ultimo_ms = 0;
static esp_timer_handle_t    s_relogio = nullptr;
// QUANTO CUSTA UM QUADRO, somado e mostrado no log a cada QUADROS_MEDIDA:
// a espera do frame, o JPEG e o envio pela rede - o numero que diz onde o
// video perde tempo.
#define QUADROS_MEDIDA 30
static struct { uint32_t n; int64_t espera, jpeg, envio; uint64_t bytes; } s_medida;

// Os primeiros frames depois de ligar saem do pipeline sem sentido (visto
// 05/11/2026: listras e lixo ate o ISP assentar): esses nao vao ao navegador.
#define AQUECER_MS 400

static void travar(void)    { xSemaphoreTake(s_trava, portMAX_DELAY); }
static void destravar(void) { xSemaphoreGive(s_trava); }

// Espera o quadro pedido em 'b' chegar. false = nao chegou no prazo.
static bool esperar(uint16_t* b, bool* ok)
{
  for (uint32_t esperou = 0; esperou < QUADRO_ESPERA_MS; esperou += 5) {
    if (tab5_camera_quadro_pronto(b, ok)) { return true; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

// O pedido adiantado que ninguem vai usar (troca de tamanho, foto): espera a
// camera largar o buffer antes de qualquer um mexer nele.
static void descartar_adiantado(void)
{
  if (s_adiantado < 0) { return; }
  bool ok;
  esperar(s_buf[s_adiantado], &ok);
  s_adiantado = -1;
}

static void soltar_buffers(void)
{
  for (auto& b : s_buf) { heap_caps_free(b); b = nullptr; }
  heap_caps_free(s_saida);
  s_saida = nullptr; s_saida_n = 0; s_l = s_a = 0;
}

// Os buffers do tamanho pedido (trocar de tamanho realoca). Sob a trava.
static bool buffers(const tamanho_t& t)
{
  if (s_buf[0] && s_l == t.l && s_a == t.a && s_em_pe == t.em_pe) { return true; }
  descartar_adiantado();
  soltar_buffers();
  for (auto& b : s_buf) { b = tab5_camera_quadro_alocar(t.l, t.a); }
  // A SAIDA: 1 byte por pixel e teto folgado para o JPEG nestas qualidades.
  const jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
  s_saida = (uint8_t*)jpeg_alloc_encoder_mem((size_t)t.l * t.a, &mem, &s_saida_n);
  if (!s_buf[0] || !s_buf[1] || !s_saida) {
    ESP_LOGE(TAG, "sem PSRAM para %dx%d", t.l, t.a);
    soltar_buffers();
    return false;
  }
  s_l = t.l; s_a = t.a; s_em_pe = t.em_pe;
  return true;
}

// Liga a camera e o codificador na primeira vez. Sob a trava.
static bool ligar(void)
{
  if (s_ligada) { return true; }
  if (!s_jpeg) {
    const jpeg_encode_engine_cfg_t cfg = { .intr_priority = 0, .timeout_ms = 300 };
    if (jpeg_new_encoder_engine(&cfg, &s_jpeg) != ESP_OK) { ESP_LOGE(TAG, "codificador JPEG nao subiu"); s_jpeg = nullptr; return false; }
  }
  if (!tab5_camera_iniciar("navegador")) { ESP_LOGE(TAG, "a camera nao ligou"); return false; }
  s_ligada = true;
  s_ligou_ms = tab5_agora_ms();
  ESP_LOGI(TAG, "camera ligada para o navegador");
  return true;
}

// Desliga tudo (ociosa). Fora da trava de quem chama: pega a trava.
static void desligar(void*)
{
  travar();
  if (s_ligada && tab5_agora_ms() - s_ultimo_ms >= CAMERA_OCIOSA_MS) {
    tab5_camera_parar("navegador");   // espera a tarefa da camera: depois disso os buffers sao nossos
    s_ligada = false;
    s_adiantado = -1;
    soltar_buffers();
    if (s_jpeg) { jpeg_del_encoder_engine(s_jpeg); s_jpeg = nullptr; }
    ESP_LOGI(TAG, "camera desligada (%d s sem quadro)", CAMERA_OCIOSA_MS / 1000);
  }
  destravar();
}

// O relogio de ociosidade: desligar mexe no esp_video (pesado) - numa vaga do pool.
static void ao_relogio(void*)
{
  if (!pool_criar(nullptr, desligar, nullptr, "cam_web_parar", 4096, 3)) { desligar(nullptr); }
}

static void rearmar(void)
{
  s_ultimo_ms = tab5_agora_ms();
  if (!s_relogio) {
    const esp_timer_create_args_t a = { .callback = ao_relogio, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                        .name = "cam_web", .skip_unhandled_events = true };
    if (esp_timer_create(&a, &s_relogio) != ESP_OK) { s_relogio = nullptr; return; }
  }
  esp_timer_stop(s_relogio);
  esp_timer_start_once(s_relogio, (uint64_t)CAMERA_OCIOSA_MS * 1000);
}

// O PROXIMO QUADRO PRONTO em s_buf[*i]: o adiantado, se houver, ou um pedido
// agora. No video, ja deixa o seguinte pedido no outro buffer. Sob a trava.
static bool pegar_quadro(const tamanho_t& t, int* i)
{
  if (!ligar() || !buffers(t)) { return false; }
  // Os frames de aquecimento: pedidos e descartados ate o pipeline assentar.
  do {
    int k = s_adiantado;
    s_adiantado = -1;
    if (k < 0) {
      k = 0;
      if (!tab5_camera_pedir_quadro(s_buf[k], t.l, t.a, t.em_pe)) { return false; }
    }
    bool ok = false;
    if (!esperar(s_buf[k], &ok) || !ok) { return false; }
    *i = k;
  } while (tab5_agora_ms() - s_ligou_ms < AQUECER_MS);
  if (!t.em_pe && tab5_camera_pedir_quadro(s_buf[1 - *i], t.l, t.a, t.em_pe)) { s_adiantado = 1 - *i; }
  return true;
}

// UM QUADRO: o proximo frame no tamanho 't', comprimido e respondido. A
// tarefa do servidor fica aqui o que falta do frame, o JPEG e o envio.
static esp_err_t responder_quadro(httpd_req_t* req, const tamanho_t& t, const char* nome)
{
  travar();
  const int64_t t0 = esp_timer_get_time();
  int i = 0;
  const bool ok = pegar_quadro(t, &i);
  const int64_t t1 = esp_timer_get_time();
  uint32_t n = 0;
  if (ok) {
    const jpeg_encode_cfg_t cfg = { .height = (uint32_t)t.a, .width = (uint32_t)t.l,
                                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                                    .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                                    .image_quality = (uint32_t)t.qualidade, .pixel_reverse = false };
    if (jpeg_encoder_process(s_jpeg, &cfg, (const uint8_t*)s_buf[i], tab5_camera_quadro_bytes(t.l, t.a),
                             s_saida, s_saida_n, &n) != ESP_OK) {
      n = 0;
    }
  }
  rearmar();
  if (!n) {
    destravar();
    ESP_LOGW(TAG, "quadro %dx%d falhou (%s)", t.l, t.a, !ok ? "a camera nao entregou" : "jpeg");
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    return httpd_resp_sendstr(req, "the camera did not deliver a frame");
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char disp[64];
  if (nome) {
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", nome);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
  }
  const int64_t t2 = esp_timer_get_time();
  const esp_err_t r = httpd_resp_send(req, (const char*)s_saida, n);
  const int64_t t3 = esp_timer_get_time();
  s_medida.espera += t1 - t0; s_medida.jpeg += t2 - t1; s_medida.envio += t3 - t2; s_medida.bytes += n;
  if (++s_medida.n >= QUADROS_MEDIDA) {
    const uint32_t q = s_medida.n;
    ESP_LOGI(TAG, "%lu quadros %dx%d: espera %lld ms, jpeg %lld ms, envio %lld ms, %llu KB cada (%llu KB/s na rede)",
             (unsigned long)q, t.l, t.a, s_medida.espera / q / 1000, s_medida.jpeg / q / 1000, s_medida.envio / q / 1000,
             s_medida.bytes / q / 1024, s_medida.envio ? s_medida.bytes * 1000000 / s_medida.envio / 1024 : 0);
    s_medida = { };
  }
  destravar();
  return r;
}

static esp_err_t pagina(httpd_req_t* req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGINA_INICIO, PAGINA_FIM - PAGINA_INICIO - 1);   // TEXT poe um '\0' no fim
}

static esp_err_t quadro(httpd_req_t* req)
{
  char v[4];
  const bool alta = tab5_web_param(req, "alta", v, sizeof(v)) && v[0] == '1';
  return responder_quadro(req, alta ? VIDEO_HD : VIDEO, nullptr);
}

static esp_err_t foto(httpd_req_t* req)
{
  char nome[40];
  const uint32_t e = tab5_hora_epoch();
  struct tm t = { };
  if (e) { tab5_hora_local(e, &t); }
  snprintf(nome, sizeof(nome), "tab5_%04d%02d%02d_%02d%02d%02d.jpg", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return responder_quadro(req, FOTO, nome);
}

static const tab5_web_rota_t ROTAS[] = {
  { "/",           HTTP_GET, pagina, false, nullptr },
  { "/quadro.jpg", HTTP_GET, quadro, false, nullptr },
  { "/foto.jpg",   HTTP_GET, foto,   false, nullptr },
};

bool tab5_camera_web_ligar(void)
{
  if (!s_trava) { s_trava = xSemaphoreCreateMutex(); }
  if (!s_trava) { return false; }
  ESP_LOGI(TAG, "camera no navegador: /camera/");
  return tab5_web_abrir("Camera", "camera", ROTAS, sizeof(ROTAS) / sizeof(ROTAS[0]));
}
