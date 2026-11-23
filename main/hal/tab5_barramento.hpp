// PRIORITY ON THE MEMORY BUS (the ESP32-P4's AXI-ICM).
//
// THE PROBLEM, at the root. The screen is a MIPI-DSI panel in DPI mode: the
// controller reads the framebuffer from PSRAM CONTINUOUSLY, line by line, at
// the pixel clock rate. At 80MHz on a 720x1280 panel with porches, that is
// ~119 MB/s that CANNOT be late - every line must be in the FIFO when the
// panel asks for it.
//
// When the processor is also hammering PSRAM (clearing the screen, drawing
// bubbles, flushing 256KB of cache), the two contend for the same bus. If the
// screen's DMA loses for too long, the FIFO runs dry - and the IDF driver
// itself describes what happens:
//
//   "when an underrun happens, the LCD display may already becomes blue,
//    it's too late to recover the display, so we just print an error message
//    as a hint to the user that he should optimize the memory bandwidth
//    (with AXI-ICM)"                        (esp_lcd_panel_dpi.c, line 113)
//
// THAT IS THE BLUE FLASH. It is not a bug in any app: it is bus arbitration.
//
// WHAT WE HAD BEEN DOING WAS PATCHWORK. Clearing in bands with pauses, avoiding
// clearing twice, spacing out repaints - all of that REDUCES demand on the CPU
// side, and helps. But it does not solve it, because with every new app
// someone has to remember all the rules, and the contention still exists.
//
// THE SOLUTION THE DRIVER ITSELF POINTS TO is AXI-ICM: the interconnect matrix
// has arbitration priority PER MASTER, and by default ALL are 0 - the screen's
// DMA is treated the same as the processor's cache. Giving higher priority to
// the master that feeds the panel, the arbiter serves the screen first and the
// FIFO stops running dry, whatever the CPU is doing.
//
// DPI reads via DW-GDMA (esp_lcd_panel_dpi.c uses esp_private/dw_gdma.h), which
// shows up in the ICM as gdma_mst1 and gdma_mst2.
//
// HONESTY ABOUT THE RISK: no IDF component calls this API - it is exposed and
// unused. I could not test it on hardware. That is why there is a console
// command to turn it on and off at run time: you can compare the two states
// without reflashing, and the driver's log ("underrun happens") tells you
// straight away which one is right.
#pragma once

#include <stdbool.h>

// Applies the priority. Once, at boot.
void tab5_barramento_priorizar_tela(bool ligar);

// Current state, for the console to show.
bool tab5_barramento_priorizado(void);
