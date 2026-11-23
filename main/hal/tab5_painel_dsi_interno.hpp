#pragma once

// CONTAINED HACK - SINGLE POINT, consolidated.
//
// tab5_tela.cpp (double buffer) and tab5_power.cpp (panel idle/tear-down)
// need to reach PROTECTED members of lgfx::Panel_DSI that M5GFX does not
// expose through a public getter: the DPI panel handle (_disp_panel_handle),
// the line pointer array (_lines_buffer), the buffer kept in _config_detail
// and the dimensions in _cfg.
// Each file had ITS OWN subclass doing the same thing (PainelAberto in one,
// PainelDSIAberto in the other) - two copies of the same trick, each exposing
// a different subset of the same fields. The risk was not theoretical: if
// M5GFX reorganises Panel_DSI, the two could break at DIFFERENT times (one
// still compiling "by luck" while the other had stopped), hiding the problem
// behind partially migrated behaviour instead of a clean, immediate
// compilation error.
//
// A single point, used by both files: breakage (at COMPILE time, not
// silently at run time - the same guarantee as before) hits both at once, in
// the same way, and the fix is made only once.
//
// We never instantiate this class - it only serves as a "key" in the
// static_cast, just like the two it replaces.
#include "lgfx/v1/platforms/esp32p4/Panel_DSI.hpp"
#include "esp_lcd_mipi_dsi.h"

namespace {
  struct PainelDSIInterno : public lgfx::Panel_DSI {
    esp_lcd_panel_handle_t pegar_handle(void)  { return _disp_panel_handle; }
    void      zerar_handle(void)               { _disp_panel_handle = nullptr; }

    uint8_t** pegar_lines(void)                { return _lines_buffer; }
    void      trocar_lines(uint8_t** v)        { _lines_buffer = v; }
    void      zerar_lines(void)                { _lines_buffer = nullptr; }

    void      zerar_buffer(void)               { _config_detail.buffer = nullptr; }

    int       largura(void)                    { return _cfg.panel_width; }
    int       altura(void)                     { return _cfg.panel_height; }
  };

  // Returns nullptr if the current panel is not a Panel_DSI (idle in
  // progress, for example) - every caller already handles that case.
  inline PainelDSIInterno* tab5_painel_dsi_interno(void)
  {
    auto* p = M5.Display.panel();
    if (p == nullptr) { return nullptr; }
    return static_cast<PainelDSIInterno*>(static_cast<lgfx::Panel_DSI*>(p));
  }
}
