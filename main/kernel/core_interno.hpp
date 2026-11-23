// What the core files share: the state used by more than one of them
// (declared here, defined in core.cpp or in the file indicated) and the internal
// functions that cross files. For them only: no app includes this header.
// What crosses the core <-> shell boundary is still kernel/internal.hpp
// (included by this one); apps talk to the system only through app.h.
//
// The core was split by topic (only moved between files, no logic changed):
//   core.cpp          engine state, touch and keyboard ISRs, wake from light sleep, UI task, sistema_iniciar
//   core_toque.cpp    GT911 watchdog and magnifier, continuous reading, sample fingers, bench (simulate touch/gesture/open)
//   core_radios.cpp   Bluetooth, Mesh over the internet, Wi-Fi on/off, Grove and the Mesh path
//   core_apps.cpp     open/close app, SD mount in its own task, refused app
//   core_energia.cpp  screen kept on by request, power-saving contract, panel clock, profile
//   core_som.cpp      speaker idling, USB pins, streamed sound, beep
//   core_desenho.cpp  usable area, measured passes, content drawing pass
#pragma once

#include "internal.hpp"
#include <atomic>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// State used by more than one file (each defined in the file indicated)
// ---------------------------------------------------------------------------
// core.cpp:
extern bool s_dedo_na_tela;                 // OUR state: went down and has not come up yet
extern volatile bool s_toque_pulso;         // the ISR saw a pulse since the last read
extern std::atomic<uint32_t> s_pulsos;      // GT911 pulses since boot (touch watchdog)
extern bool s_dedo_do_despertar;            // the finger that woke the screen (or opened the app) touches nothing
extern TaskHandle_t s_tarefa_interface;     // captured at creation, see sistema_iniciar()
extern uint32_t s_ultimo_desenho;

// core_toque.cpp:
extern uint32_t s_vig_falhas, s_vig_presos, s_vig_surdos, s_vig_reinicios;
extern uint8_t  s_vig_status;
extern sistema_toque_t s_toques[SISTEMA_TOQUES_MAX];
extern int      s_n_toques;
extern bool     s_toque_consumido;
extern volatile int s_sim_x, s_sim_y;
extern const app_t* volatile s_sim_abrir;
extern volatile bool s_sim_fechar;
extern volatile int s_simg[4];
extern volatile bool s_simg_pedido;

// core_apps.cpp:
extern volatile bool s_sd_montando;         // true from the trigger until the task finishes

// core_energia.cpp:
extern std::atomic<uint32_t> s_acordado;
extern volatile bool s_pedir_acordar;
extern SemaphoreHandle_t s_relogio_feito;   // created with the UI (sistema_iniciar)

// core_som.cpp:
extern const uint32_t SPEAKER_OCIOSO_MS;
extern uint32_t s_speaker_ocioso_desde;     // 0 = playing or off
extern SemaphoreHandle_t s_som_parou;       // created in sistema_iniciar
extern SemaphoreHandle_t s_fluxo_pronto;    // created in sistema_iniciar

// core_desenho.cpp:
extern uint32_t s_fase_limpar_us;           // the last clearing of the area (+ nav)

// ---------------------------------------------------------------------------
// Functions that cross files
// ---------------------------------------------------------------------------
// core.cpp:
uint32_t periodo_do_app(void);

// core_toque.cpp:
void ajustar_leitura_do_toque(bool continuo);
void lupa_toque(uint32_t t, bool tocando);
void vigiar_toque(uint32_t t, bool tocando);
void vigiar_toque_repouso(uint32_t t, bool tocando, bool em_repouso);

// core_radios.cpp:
void vigiar_mesh_internet(uint32_t t);
void bt_aplicar(bool ligar);

// core_apps.cpp:
void concluir_abertura_do_sd(void);

// core_energia.cpp:
void atender_pedido_de_relogio(void);
tab5_perfil_t perfil_da_home(void);

// core_som.cpp:
void som_configurar(void);
void som_atender_fluxo(void);
void som_atender_reserva(void);
void atualizar_beep_e_ociosidade_do_som(uint32_t t);

// core_desenho.cpp:
area_t area_util(void);
int limite_do_app(const area_t& a);
void animar_na_rampa(void);
void anotar_passada(const char* nome, uint32_t us, uint32_t barra, uint32_t limpar, uint32_t conteudo);
// recorte_app: only the scroll-shift path passes it (the default is the whole usable area)
void desenhar_conteudo(bool limpar_area, bool completo_para_apps, const area_t* recorte_app = nullptr);
