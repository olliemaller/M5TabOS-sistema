// TOCANDO AGORA - ver midia.h. Fica fora da lista de .bss na PSRAM
// (linker.lf): tem portMUX.
#include "midia.h"
#include "app.h"
#include "internal.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static midia_info_t      s_info = { };
static midia_controles_t s_ctl  = { };
static uint32_t          s_versao = 1;       // palavra sozinha: lida sem trava
static uint32_t          s_versao_pos = 1;

void midia_assumir(const char* fonte, const char* app, const midia_controles_t* c)
{
  taskENTER_CRITICAL(&s_mux);
  s_ctl = c ? *c : midia_controles_t{ };
  s_info.ativa   = true;
  s_info.fonte   = fonte;
  s_info.app     = app;
  s_info.busca   = s_ctl.ir_para != nullptr;
  s_info.tocando = false;
  s_info.pos_ms  = 0;
  s_info.dur_ms  = 0;
  s_info.titulo[0] = '\0';
  ++s_versao; ++s_versao_pos;
  taskEXIT_CRITICAL(&s_mux);
  sistema_pedir_redesenho();
}

void midia_soltar(void)
{
  taskENTER_CRITICAL(&s_mux);
  s_info = { };
  s_ctl  = { };
  ++s_versao; ++s_versao_pos;
  taskEXIT_CRITICAL(&s_mux);
  sistema_pedir_redesenho();
}

void midia_publicar(const char* titulo, bool tocando, uint32_t pos_ms, uint32_t dur_ms)
{
  bool mudou = false;
  taskENTER_CRITICAL(&s_mux);
  if (s_info.ativa) {
    // Compare only what fits: a title longer than the buffer is stored truncated,
    // and comparing the full MIDIA_TITULO bytes saw a "change" on every call
    // (endless redraws of the mini-player while a long-titled track played).
    if (titulo && strncmp(titulo, s_info.titulo, MIDIA_TITULO - 1) != 0) {
      strncpy(s_info.titulo, titulo, MIDIA_TITULO - 1);
      s_info.titulo[MIDIA_TITULO - 1] = '\0';
      ++s_versao; mudou = true;
    }
    if (tocando != s_info.tocando) { s_info.tocando = tocando; ++s_versao; mudou = true; }
    if (pos_ms / 1000 != s_info.pos_ms / 1000 || dur_ms / 1000 != s_info.dur_ms / 1000) { mudou = true; }
    s_info.pos_ms = pos_ms;
    s_info.dur_ms = dur_ms;
    if (mudou) { ++s_versao_pos; }
  }
  taskEXIT_CRITICAL(&s_mux);
  // So acorda a interface quando ha quem mostre o segundo (a central aberta).
  // Com ela fechada, a barra confere no proprio passo (barra_mudou) - acordar
  // a cada segundo de musica com a tela apagada seria gasto a toa.
  if (mudou && s_painel_avisos) { sistema_acordar_interface(); }
}

void midia_definir_cor(uint16_t cor565)
{
  taskENTER_CRITICAL(&s_mux);
  const bool mudou = s_info.ativa && s_info.cor != cor565;
  if (mudou) { s_info.cor = cor565; ++s_versao; ++s_versao_pos; }
  taskEXIT_CRITICAL(&s_mux);
  if (mudou) { sistema_pedir_redesenho(); }
}

bool midia_ler(midia_info_t* out)
{
  taskENTER_CRITICAL(&s_mux);
  *out = s_info;
  taskEXIT_CRITICAL(&s_mux);
  return out->ativa;
}

uint32_t midia_versao(void)         { return s_versao; }
uint32_t midia_versao_posicao(void) { return s_versao_pos; }

// Copia o ponteiro sob a trava e chama fora dela (o controle so marca um
// pedido, mas nao ha motivo para rodar codigo de app numa secao critica).
#define CHAMAR(campo, ...) do {                         \
    taskENTER_CRITICAL(&s_mux);                         \
    auto fn = s_info.ativa ? s_ctl.campo : nullptr;     \
    taskEXIT_CRITICAL(&s_mux);                          \
    if (fn) { fn(__VA_ARGS__); }                        \
  } while (0)

void midia_tocar_pausar(void)   { CHAMAR(tocar_pausar); }
void midia_proxima(void)        { CHAMAR(proxima); }
void midia_anterior(void)       { CHAMAR(anterior); }
void midia_ir_para(uint32_t ms) { CHAMAR(ir_para, ms); }
