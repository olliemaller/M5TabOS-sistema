#include "tab5_barramento.hpp"

#include "hal/axi_icm_ll.h"
#include "esp_log.h"

static const char* TAG = "barramento";

// 0..15 in both fields, with a factory default of 0 for everyone.
//
// 8 and not 15 on purpose. What matters is the ORDER between masters, not the
// magnitude: the screen just has to be above the CPU to win. Leaving headroom
// above leaves room to one day put something between the two (the audio DMA,
// for example) without having to demote the screen.
#define PRIO_TELA   8
#define PRIO_PADRAO 0

static bool s_ligado = false;

void tab5_barramento_priorizar_tela(bool ligar)
{
  const uint32_t p = ligar ? PRIO_TELA : PRIO_PADRAO;

  // ALREADY SET: do not rewrite. Waking from idle reasserts the priority
  // every time - and with music playing the bus is busy (SD card via DMA,
  // speaker, decoder in PSRAM). Touching arbitration in the middle of that
  // traffic needlessly is not free; it is written only when something really
  // changed (the factory default came back).
  if (AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority == p
      && AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst2_priority == p
      && AXI_ICM.mst_arqos_reg0.reg_gdma_mst1_arqos == p
      && AXI_ICM.mst_arqos_reg0.reg_gdma_mst2_arqos == p
      && AXI_ICM.mst_awqos_reg0.reg_gdma_mst1_awqos == 0
      && AXI_ICM.mst_awqos_reg0.reg_gdma_mst2_awqos == 0) {
    s_ligado = ligar;
    return;
  }

  // ARBITRATION priority: who wins the bus when two ask at once.
  // This is the field that decides the contest.
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority = p;
  AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst2_priority = p;

  // ARQOS of the READ channel. Complementary: arbitration picks who gets in,
  // QoS says how much their request is worth to the memory controller.
  //
  // Read only (arqos), not write: the panel only READS the framebuffer.
  // Touching the write channel would not help the screen and would take
  // bandwidth from writers.
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, p);
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, p);

  s_ligado = ligar;
  ESP_LOGI(TAG, "screen priority for memory access: %s (gdma=%u, cache=%u)",
           ligar ? "HIGH (against the blue flash)" : "default",
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_gdma_mst1_priority,
           (unsigned)AXI_ICM.mst_arb_priority_reg0.reg_cache_priority);
}

bool tab5_barramento_priorizado(void) { return s_ligado; }
