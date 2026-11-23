// CAMERA service - the Tab5's SC2356 (SC202CS) sensor, MIPI-CSI, 1600x1200.
//
// RESEARCHED, not invented: there was NO camera driver at all in this
// project until now - only CAM_RST (tab5_power_camera). The facts below come
// from Espressif's OFFICIAL BSP for this exact board
// (github.com/espressif/esp-bsp, bsp/m5stack_tab5), not from guesswork:
//
//   - Sensor: SC2356 / SC202CS, MIPI-CSI. Kconfig: CONFIG_CAMERA_SC202CS.
//   - Reset pin: NO direct GPIO - it is the SAME CAM_RST that
//     tab5_power_camera() already controls (expander 0, bit 6). The app
//     system used to turn it on by itself via perfil.camera=true; since the
//     camera became a service (see the big notice just below), what turns the
//     pin on and OFF is tab5_camera_iniciar()/parar() itself.
//   - SCCB (the sensor's I2C, to configure it): G32/G31 - EXACTLY the same
//     pins M5Unified already uses for the INTERNAL bus (M5.In_I2C: touch,
//     IMU, PMIC, expanders - see M5Unified.cpp, board_M5Tab5). In other
//     words, the camera is just one more address on the SAME bus the rest of
//     the system already owns and protects with the rule "only the UI task
//     talks I2C".
//
// HOW THIS DOES NOT CLASH WITH WHAT EXISTS: instead of opening a second bus
// on the same pins (forbidden - ESP-IDF does not allow two owners of the same
// GPIO), tab5_camera_iniciar() takes the handle M5Unified already installed
// with i2c_master_get_bus_handle() - an IDF function made exactly for "I want
// the bus another component already opened", without initialising anything
// new. Confirmed: M5GFX uses i2c_new_master_bus() (the NEW driver) under
// M5.In_I2C - which is why the handle is of the right type.
//
// THE REST (V4L2: open/ioctl/mmap on /dev/videoN) is the same pattern
// Espressif's official example uses for THIS board
// (esp-bsp/examples/display_camera_video) - we just swapped LVGL for M5GFX and
// the asynchronous callback for tarefa_app_t (the template this project
// already uses for every background service, see kernel/tarefa_app.h).
#pragma once

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
// IT BECAME A SERVICE, NO LONGER "A RESOURCE OF THE CAMERA APP"
// ("which internal sensors can become services?" -> the camera was the only
// piece of expensive hardware that still existed only while ONE particular
// app was on screen).
// -------------------------------------------------------------------------
// REFERENCE COUNTING, same template as tab5_sd.hpp: tab5_camera_iniciar()
// can be called by MORE THAN ONE consumer at once (the Camera app AND, for
// example, the 'foto' console command, or a future second app) - the first
// call actually turns the sensor on, the following ones just count one more
// interested party, and the sensor only turns off when the LAST one calls
// tab5_camera_parar(). Opening the Camera app while the 'foto' command still
// holds the camera (or vice versa) is safe: neither knocks the other out.
//
// THE SENSOR HAS ONLY ONE MODE (1600x1200, see the photo below); THE SIZE
// BELONGS TO THE USER. Turning on does not choose a resolution: the preview
// has the destination's size (tab5_camera_definir_destino) and each requested
// frame its own (tab5_camera_pedir_quadro). So the Camera app, a Berry app,
// the console and the PC browser (services/tab5_camera_web) use the camera at
// the same time without one refusing another. (Previously iniciar() took the
// preview size and refused a second consumer with a different size.)
//
// THE CAMERA NOW POWERS ITSELF - previously, CAM_RST (the sensor's reset pin)
// was only turned on because the Camera app declared perfil.camera=true, and
// the KERNEL applied that before ao_abrir() ran (see tab5_power.hpp). A real
// service cannot depend on ONE particular app being open to get power - so
// tab5_camera_iniciar() now calls tab5_power_camera(true) ITSELF, in the same
// spirit in which tab5_grove.cpp already turns on the 5V rail by itself for
// GPS/Mesh (see the notice there). The Camera app NO LONGER declares
// perfil.camera - see the notice in app_camera.cpp.
//
// THE ONLY DELICATE POINT: core.cpp still reapplies the power profile
// (imu/mic/camera/etc) every time an app opens or closes, as the union of
// what ALL currently open apps declare - and WITHOUT KNOWING this service
// exists, that reapplication would turn CAM_RST off in the middle of a
// session requested outside an app (the 'foto' command, for example) just
// because the app THAT OPENED AT THAT INSTANT does not ask for the camera.
// tab5_camera_ativa() (below) exists exactly to plug that hole:
// sistema_impor_preferencias() ORs it in, just as it already does with
// ajustes_grove() for Grove.
//
// ONLY FROM THE UI TASK, DIRECTLY - found live with the 'foto' command:
// these two functions touch SCCB (the internal I2C, the SAME bus as
// touch/IMU/power/RTC), and this project's rule is "only the UI task talks
// I2C" (with no lock at all in the driver). app_camera.cpp can call directly
// because ao_abrir()/ao_fechar() ALREADY run in that task. ANY other caller
// (a console command, a future background task) MUST go through
// sistema_camera_iniciar()/sistema_camera_parar() (kernel/app.h) instead of
// calling these - the bridge sends the request to the right task. Calling
// these directly from another task is what produced "Set format regs fail"
// on the first attempt of 'foto'.

// Turns the sensor on (or just adds one more interested party, if already on
// - see the big notice above about reference counting): shared I2C bus,
// esp_video_init, opens the device, allocates the capture buffers, registers
// the PPA (scale + rotate 270 degrees - the sensor is mounted sideways on the
// board) and starts the task that produces frames.
//
// From any task: SCCB goes through the internal I2C lock
// (hal/tab5_sccb.cpp). Does not depend on perfil.camera; it turns CAM_RST on
// itself. Returns false if any step fails (the log explains which) - in that
// case there is nothing to turn off.
// 'quem' (app, "navegador", "console") is who shows up as using the CAMERA
// resource (kernel/recursos.h) - the same name in parar.
bool tab5_camera_iniciar(const char* quem);

// Drops ONE interest (see the reference counting above). Only when the LAST
// one goes does the task actually come down (tarefa_app_encerrar, with a
// timeout - see the notice there about a task deliberately abandoned rather
// than forcibly killed), the stream stops, the buffers go, the PPA client
// leaves and CAM_RST goes back into reset. CALL ONLY FROM THE UI TASK (see
// the big notice above) - where tab5_camera_iniciar() was called from
// (ao_fechar()). Safe to call even if tab5_camera_iniciar() never ran,
// failed, or was already stopped another way.
void tab5_camera_parar(const char* quem);

// PAUSE WITH THE SCREEN OFF - a SYSTEM rule, not an app one (06/11/2026,
// docs/SUSPENSAO_2026-11-05.md). The caller is the kernel (kernel/core.cpp),
// on idle, on behalf of the foreground app: with the screen off nobody sees
// the preview, and a needlessly running camera holds 180 MHz and blocks
// light sleep (the CAMERA resource). Previously each app did this in its own
// ao_repouso (Camera app, Berry apps) - and a consumer that forgot kept
// filming in the dark.
//
// tab5_camera_pausar(quem): if 'quem' uses the camera, releases its use (like
// tab5_camera_parar) and keeps the preview destination. The preview stops
// writing to the surface even if another consumer (the browser) keeps the
// camera on. Returns true if it paused. A requested frame that dies with the
// camera comes back as ready with ok = false (the requester already handles
// the failure).
// tab5_camera_despausar(religar): religar = true turns it on again as the same
// 'quem' and restores the destination; false just forgets the pause (the app
// closed). If the paused owner calls tab5_camera_parar/iniciar in between
// (closed, or the script turned it back on), the pause is forgotten and the
// count stays right.
// Only from the UI task, like iniciar/parar.
bool tab5_camera_pausar(const char* quem);
void tab5_camera_despausar(bool religar);

// Is any interested party alive right now (reference count > 0)? It exists
// so sistema_impor_preferencias() (kernel/core.cpp) does not cut CAM_RST from
// under a consumer that is not the focused app - see the big notice at the
// top of this file. Normal apps do not need to call this.
bool tab5_camera_ativa(void);

// WHERE THE PREVIEW IS WRITTEN. The camera task writes each frame, already
// rotated and scaled to width x height (those of tab5_camera_iniciar),
// STRAIGHT into a rectangle of an RGB565 image - in practice the system's
// virtual screen (sistema_superficie, kernel/app.h). No intermediate copy.
// 'buf' needs cache alignment (the virtual screen has it).
// 'avisar' (optional) is called from the camera task for every frame written
// - sistema_acordar_interface, so the UI picks the frame up straight away.
typedef struct {
  void*  buf;          // null = no destination (requested frames only)
  size_t bytes;        // total size of buf
  int    pic_l, pic_a; // dimensions of the whole image
  int    x, y;         // corner of the rectangle within it
  int    l, a;         // size of the rectangle: the preview (uniform scale, filling)
  void (*avisar)(void);
} tab5_cam_destino_t;

void tab5_camera_definir_destino(const tab5_cam_destino_t* d);   // nullptr = none

// true ONCE per frame written to the destination since the last call.
bool tab5_camera_quadro_escrito(void);

// FRAME ON REQUEST: the next whole frame, at the requester's l x a size
// (RGB565, line by line top to bottom), with the same uniform scale as the
// preview - only what does not fit the aspect ratio is cropped from the
// edges. It is the PHOTO (TAB5_CAM_FOTO_L x _A portrait: the sensor's whole
// frame, unscaled) and the browser VIDEO (smaller, JPEG-compressed by the
// requester).
//
// PORTRAIT OR AS THE SENSOR SEES IT. The sensor is sideways on the board: in
// portrait, the PPA rotates 270 degrees and mirrors (like the screen and the
// photo) - and ROTATING COSTS: ~117 ms per 480x640 frame (measured
// 05/11/2026; the PPA reads PSRAM column by column), against the ~32 ms
// between two frames. 'em_pe' = false delivers the frame landscape (l x a in
// the sensor's 4:3 ratio), only reduced, and whoever displays it rotates -
// the browser does that for free. To show it in portrait: rotate 90 degrees
// clockwise and mirror horizontally (= transpose).
//
// THE SENSOR RUNS IN 1600x1200 MODE (sdkconfig.defaults). The driver's
// 1280x720 mode does not scale the image down: it enables a WINDOW in the
// middle of the sensor (columns 160-1447, rows 240-967), 80% of the width and
// 60% of the height - the photo came out as if zoomed.
//
// THE CAMERA ONLY CAPTURES; WHOEVER ASKED SAVES OR COMPRESSES. The request is
// served on the next frame with ONE PPA operation (a few ms): the preview
// does not stop. Up to TAB5_CAM_PEDIDOS (2) requests at once - an app's photo
// and the PC's video do not compete for a turn.
#define TAB5_CAM_FOTO_L 1200
#define TAB5_CAM_FOTO_A 1600

// The right buffer for an l x a frame (PSRAM, cache-line aligned, the size
// the PPA requires). Free with heap_caps_free.
uint16_t* tab5_camera_quadro_alocar(int l, int a);
size_t    tab5_camera_quadro_bytes(int l, int a);

// Requests the next frame into 'destino' (from tab5_camera_quadro_alocar, at
// the same l x a). false = camera off, no free slot, or this destination
// already has a request. 'destino' is written by the camera task until
// tab5_camera_quadro_pronto() returns true - do not free it before then
// (tab5_camera_parar() also waits for the task to finish).
bool tab5_camera_pedir_quadro(uint16_t* destino, int l, int a, bool em_pe = true);

// true ONCE when the frame requested for 'destino' has arrived (ok = PPA
// succeeded); the slot is then free for the next request.
bool tab5_camera_quadro_pronto(const uint16_t* destino, bool* ok);

// SENSOR REGISTER, for diagnostics (the console's 'camera' command).
// valor >= 0 writes before reading; -1 only reads. false = camera off or SCCB
// failed. Whatever is written holds until the camera turns off: on turning on
// again the driver reapplies the initialisation table.
bool tab5_camera_reg(uint16_t reg, int valor, uint8_t* lido);
