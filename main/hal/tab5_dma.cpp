// THE DW-GDMA DMA SHARED BETWEEN THE SCREEN AND THE CAMERA - a channel is
// only deleted once STOPPED.
//
// THE PROBLEM (crash reproduced on 05/11/2026: screen waking with the browser
// camera streaming; core dump in the 'ui' task, write to 0x50081128, inside
// dpi_panel_init -> dw_gdma_channel_use_link_list). The DW-GDMA has a channel
// group with a user count: the screen (DPI panel, a channel in an infinite
// loop carrying the framebuffer to DSI) and the camera (CSI, another
// channel). The DPI panel's esp_lcd_panel_del() deletes the channel
// (dw_gdma_del_channel) WITHOUT STOPPING it first - the channel stays enabled
// in hardware. On its own this does not show: with no other user the group
// count drops to zero, and the driver powers down and RESETS the DW-GDMA,
// which clears the orphan channel. With the camera on the group stays up; on
// wake, the new panel gets THE SAME channel number, still enabled, and
// writing its configuration causes a bus fault.
//
// THE SOLUTION. The linker diverts dw_gdma_del_channel() here
// (-Wl,--wrap=dw_gdma_del_channel, main/CMakeLists.txt): the channel is
// disabled and, if still running, aborted - through the driver's own API -
// and only then deleted. It applies to every DW-GDMA user (screen, camera and
// whatever comes), without editing the component; screen idle (hal/tab5_power,
// desmontar_tela) still just calls esp_lcd_panel_del.

#include "esp_private/dw_gdma.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" esp_err_t __real_dw_gdma_del_channel(dw_gdma_channel_handle_t chan);

extern "C" esp_err_t __wrap_dw_gdma_del_channel(dw_gdma_channel_handle_t chan)
{
  if (chan) {
    dw_gdma_channel_enable_ctrl(chan, false);
    // The looping channel (the screen) never finishes its block by itself:
    // after a moment for the transfer in progress, the abort is what
    // guarantees it stops.
    esp_rom_delay_us(20);
    dw_gdma_channel_abort(chan);
  }
  return __real_dw_gdma_del_channel(chan);
}

// ---------------------------------------------------------------------------
// THE SCREEN DMA INTERRUPT: LEVEL 3, ALWAYS ON CORE 1 (the blue flash)
// ---------------------------------------------------------------------------
// THE PROBLEM (proven on 14/11/2026). In IDF 5.5 the DMA carrying the
// framebuffer to DSI is NOT circular: the list has a single item, marked
// is_last, and at every end of frame the DW-GDMA ISR restarts it
// (esp_lcd_panel_dpi.c, mipi_dsi_dma_trans_done_cb). The slack is the gap
// between frames (~0.5 ms). If the ISR is later than that, the panel goes ONE
// WHOLE FRAME without an image and shows BLUE - with no underrun in the
// bridge (that would be magenta), which is why the text detector never
// counted it; the signal is tab5_quadro's "late" counter (always ~2 periods).
//   - The ISR was created at level 1 (the lowest): any other interrupt on the
//     core (C6 SDIO, SD card, timers) got ahead of it. Under stress, the
//     delays came with the core in IDLE waiting for an interrupt (another ISR
//     had held it) and coming out of a critical section of the Berry task.
//   - And it was created on the core of WHOEVER CREATES the panel: boot
//     (core 0) and each wake from idle (another task) - the core changed by
//     itself.
//   - Proof: 'tela atrasar 20 1' (holds the ISR's core) = late frames and
//     the screen flashing blue; on the other core, nothing.
//
// THE SOLUTION, for every DW-GDMA channel (screen and camera share the same
// interrupt source and level - dw_gdma.c: "all channels in the same group
// should use the same interrupt priority", first come decides; a single
// channel at 3 would give "intr_priority conflict" and the panel would not
// come up):
//   1. dw_gdma_new_channel: intr_priority = 3. The highest level that can
//      still use FreeRTOS in the ISR (the panel callback gives a semaphore);
//      it gets ahead of level 1 and 2 ISRs. A critical section (mask 1-3)
//      still holds it.
//   2. dw_gdma_channel_register_event_callbacks (where IDF allocates the
//      interrupt, on the caller's core): always executed on CORE 1, away from
//      the SDIO/Wi-Fi ISRs and the Berry task, which live on 0. When called
//      from the other core, a short task pinned to 1 makes the call and
//      returns the result. It only happens when creating a channel (boot,
//      wake, camera) - never on the frame path.

extern "C" esp_err_t __real_dw_gdma_new_channel(const dw_gdma_channel_alloc_config_t* config,
                                                 dw_gdma_channel_handle_t* ret_chan);

extern "C" esp_err_t __wrap_dw_gdma_new_channel(const dw_gdma_channel_alloc_config_t* config,
                                                 dw_gdma_channel_handle_t* ret_chan)
{
  if (!config) { return __real_dw_gdma_new_channel(config, ret_chan); }
  dw_gdma_channel_alloc_config_t c = *config;
  c.intr_priority = 3;
  return __real_dw_gdma_new_channel(&c, ret_chan);
}

extern "C" esp_err_t __real_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t chan,
                                                                      dw_gdma_event_callbacks_t* cbs,
                                                                      void* user_data);

static constexpr int NUCLEO_DA_TELA = 1;

// The request carried to the core-1 task, and the reply.
struct registro_t {
  dw_gdma_channel_handle_t   chan;
  dw_gdma_event_callbacks_t* cbs;
  void*                      user_data;
  esp_err_t                  resultado;
  TaskHandle_t               quem_pediu;
};

static void tarefa_registro(void* arg)
{
  registro_t* r = (registro_t*)arg;
  r->resultado = __real_dw_gdma_channel_register_event_callbacks(r->chan, r->cbs, r->user_data);
  xTaskNotifyGive(r->quem_pediu);
  vTaskDelete(nullptr);
}

extern "C" esp_err_t __wrap_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t chan,
                                                                      dw_gdma_event_callbacks_t* cbs,
                                                                      void* user_data)
{
  if (xPortGetCoreID() == NUCLEO_DA_TELA) {
    return __real_dw_gdma_channel_register_event_callbacks(chan, cbs, user_data);
  }
  registro_t r = { chan, cbs, user_data, ESP_FAIL, xTaskGetCurrentTaskHandle() };
  // Stack in internal RAM (xTaskCreatePinnedToCore uses the default):
  // esp_intr_alloc touches structures that cannot be in PSRAM.
  if (xTaskCreatePinnedToCore(tarefa_registro, "gdma_irq", 3072, &r,
                              uxTaskPriorityGet(nullptr), nullptr, NUCLEO_DA_TELA) != pdPASS) {
    // No memory for the task: register right here (the screen works, it just
    // lacks the core-1 protection) - better than no screen.
    ESP_EARLY_LOGW("tab5_dma", "no memory to move the DMA ISR to core %d", NUCLEO_DA_TELA);
    return __real_dw_gdma_channel_register_event_callbacks(chan, cbs, user_data);
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return r.resultado;
}
