#pragma once

// OUR OWN ALGORITHM IN THE ISP PIPELINE (IPA) - see the header comment of
// tab5_camera_ipa.c for the why. This header is what the rest of the system
// sees: a snapshot of the ISP state, for diagnostics (the console's 'camera'
// command).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t quadros;          // frames processed since the camera turned on
  float    ganho_r, ganho_b; // white balance requested by the AWB
  float    ccm_awb[3][3];    // the calibration's colour matrix (without the balance)
  float    ccm_isp[3][3];    // the one sent to the ISP, after white protection
  float    g_branco_antes;   // green of a saturated white with the calibration matrix (1 = white)
  float    g_branco_depois;  // the same, with the protected matrix
  uint32_t saturacao;        // 128 = 1.0
  uint32_t exposicao_us;
  float    ganho_sensor;
  uint32_t hist[16];         // Y histogram (ISP output), bins of 16
} tab5_cam_ipa_estado_t;

// Copies the latest state. False if the pipeline has not run any frame yet.
bool tab5_camera_ipa_estado(tab5_cam_ipa_estado_t* e);

#ifdef __cplusplus
}
#endif
