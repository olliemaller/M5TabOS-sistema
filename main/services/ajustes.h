// System settings - the USER'S PREFERENCES, stored in NVS.
//
// The distinction that motivated this module: each app's tab5_perfil_t says "what
// hardware I need turned on" - Grove, radio, IMU. That is an
// application requirement. Brightness is something else: it is the preference of whoever is holding
// the device, and no app has authority over it. They were mixed up in the
// same struct, and the app won - wrong.
//
// Now the system overrides the profile's brightness when opening the app. The
// field still exists in tab5_perfil_t (which needs to stay self-contained and
// copyable to other projects), but this module is in charge here.
//
// NVS and not the SD card: settings have to work with no card at all, and be read at
// boot before any mounting.

#pragma once

#include <stdint.h>
#include <stddef.h>

// Brightness range, in M5GFX's value (0-255 becomes the duty of a 9-bit PWM
// at 44.1 kHz on GPIO22). The minimum is NOT zero on purpose: with the screen off
// you would not be able to see the control to undo it. The eye perceives
// brightness on a logarithmic scale (see the Settings slider, which follows that curve),
// so the floor has to be low - but not too low: at 2 (0.9% duty) the screen
// stayed lit while dragging the slider and CAME BACK DARK after idle, because
// the backlight driver does not start from zero with such a short pulse (see
// tab5_power_acender). 6 = 1.8% duty: still dark, but always visible.
#define AJUSTES_BRILHO_MIN  6
#define AJUSTES_BRILHO_MAX  255

// Loads from NVS (and initialises NVS, if needed). Call before the tasks.
void ajustes_iniciar(void);

// Every settings change goes to NVS 1 s after the last one (on the
// esp_timer task). Before cutting the power, this brings the pending write forward.
void ajustes_gravar_agora(void);

uint8_t ajustes_brilho(void);

// Applies to the screen NOW and saves. An out-of-range value is clamped, not refused.
void ajustes_definir_brilho(uint8_t brilho);

// BRIGHTNESS ON THE EYE'S SCALE. The PWM value is linear in light, but the eye is not;
// the "position" runs from 0 to AJUSTES_BRILHO_POS_MAX and the PWM comes from it via the
// gamma 2.2 curve (the Settings slider moves along it). The percentage (0-100, for those who speak
// that way: the Berry scripts) is the same position in steps of 10. 0 does not turn the
// screen off - AJUSTES_BRILHO_MIN applies.
#define AJUSTES_BRILHO_POS_MAX 1000
int ajustes_brilho_para_pos(int brilho);   // PWM -> position
int ajustes_pos_para_brilho(int pos);      // position -> PWM
uint8_t ajustes_brilho_pct(void);
void    ajustes_definir_brilho_pct(uint8_t pct);

// Whether the user wants Wi-Fi on. The one that acts on it is the Wi-Fi service;
// only the wish lives here.
bool ajustes_wifi(void);
void ajustes_definir_wifi(bool ligado);

// Keep the Grove 5V ALWAYS on.
//
// Without this, a module that needs to boot (the Unit C6L runs the whole of Meshtastic
// on an ESP32-C6) restarts every time the rail is cut, and never gets to
// talk. With this on it stays up like any peripheral.
//
// Cost: the rail draws power even with nothing plugged in, which is why it starts off.
bool ajustes_grove(void);
void ajustes_definir_grove(bool ligado);

// The USB-A PORT on: 5V at the connector and the flash drive mounted automatically at /usb
// (services/tab5_usb). Starts ON - plugging in a flash drive and playing music
// must not depend on finding a setting. With nothing plugged in it costs almost nothing
// (the 5V switch idle); with a device plugged in, light sleep is suspended.
bool ajustes_usb_a(void);
void ajustes_definir_usb_a(bool ligado);

// Time without touch until the screen goes off, in SECONDS.
//
// Stored in seconds and not milliseconds because it fits in an NVS uint8_t and
// because that is how people think about the setting. The system converts it.
//
// The 15 s minimum is not a whim: below that the screen goes off in the middle of
// reading, and the wake-up tap becomes part of normal use. The 60 s maximum
// comes from the other side - the screen is the device's biggest consumer, and each second
// lit costs more than any saving we squeezed out of everything else.
#define AJUSTES_TELA_MIN_S  15
#define AJUSTES_TELA_MAX_S  60

uint8_t ajustes_tela_s(void);
void    ajustes_definir_tela_s(uint8_t segundos);

// PANEL CLOCK (DPI/pixel clock), in MHz.
//
// Origin: until now it could only be changed with 'tela relogio <mhz>'
// on the console, and the value died at the next boot (the system always reapplied a
// fixed 60). Lowering the clock cuts the continuous PSRAM reading the panel
// demands (see tab5_power.cpp) - the same family of savings as brightness, only
// working on the OTHER side of the equation (how much the panel ASKS for, not how much the system
// spends serving it). Stored, and not just in RAM, to survive a restart, and
// exposed in Settings for anyone without the serial cable to hand.
//
// The range is the SAME one tab5_power_relogio_dpi() accepts (24..80) - clamped
// here too, and not only there, for the same reason as tela_s: a value saved by
// mistake must not become a panel that never comes up again.
// Ceiling of 80 (was 100). Above 80 the
// panel only spends PSRAM bandwidth with no visible gain, and Settings' step of 5
// (24, 29 ... 79) never stopped at 80: it jumped to 84. With the ceiling at 80,
// clamping does the rest - 79+5 becomes 80, 25-5 becomes 24.
#define AJUSTES_RELOGIO_MIN  24
#define AJUSTES_RELOGIO_MAX  80

uint8_t ajustes_relogio_painel(void);

// ONLY SAVES - does not apply to the panel. What really changes the clock is
// tab5_power_relogio_dpi(); the caller (console, Settings) decides the
// ORDER between applying and saving, because only it knows whether the change worked.
void    ajustes_definir_relogio_painel(uint8_t mhz);

// VOLUME of the internal speaker (ES8388). NATIVE range of
// M5.Speaker.setVolume() (0..255) - unlike brightness, 0 is a
// legitimate state (mute), so there is no floor: the difference that justifies the
// brightness floor (a dark screen hides its own control for undoing it) does not
// exist here.
//
// APPLIED AT ONCE, same pattern as ajustes_definir_brilho() - but WITHOUT
// any hardware cost while nobody is playing sound: setVolume() only stores
// a number in the Speaker object, the ES8388 stays off until the first
// M5.Speaker.tone()/playWav() from some app (M5Unified initialises the codec
// lazily - see the big warning in app_config.cpp, next to the
// VOLUME section). That is why it can be applied here, at boot, without waiting for any app
// to open.
#define AJUSTES_VOLUME_MIN  0
#define AJUSTES_VOLUME_MAX  255

uint8_t ajustes_volume(void);
void    ajustes_definir_volume(uint8_t volume);

// HOME - reference position for calculating distance/direction to a
// received location (Mesh, app_mesh.cpp). It belongs to the user and not to Mesh
// specifically, for the same reason as brightness: it is not a requirement of any
// app, it is the preference of whoever holds the device - stored here so it can
// serve any app that needs it in future, not just Mesh.
//
// Returns false while not configured (lat=lon=0 is the factory value, and
// also the only real point where nobody lives - so it is safe to use as "empty"
// without needing a separate flag in NVS).
bool ajustes_casa(float* lat, float* lon);
void ajustes_definir_casa(float lat, float lon);

// TRILHA (Trail) SOUND ALERT: silence only Trilha, with Mesh still sounding.
// It lives here (user preference) and not
// in notificacao.h (which only keeps the state in RAM, see notif_silenciar()) -
// the same distinction as the rest of this file: WHAT sounds is the decision of whoever
// holds the device, not the service's. Default PLAY (true): the notification
// beep already existed and worked; this is a way to turn it off, not
// on. It stays inside the Trilha app itself for now (not in Settings) -
// if one day more sources ask for the same control, each gets the
// analogous pair (ajustes_beep_mesh() etc.), rather than a generic mechanism without
// a second real case to validate the format (see docs/ARQUITETURA.md, 1.1).
bool ajustes_beep_trilha(void);
void ajustes_definir_beep_trilha(bool tocar);

// MESH SOUND ALERT - the analogous pair foreseen in the note above, done in the
// same format. Same logic, same
// default (PLAY).
bool ajustes_beep_mesh(void);
void ajustes_definir_beep_mesh(bool tocar);

// SPECTRUM COLOUR THEME of the Music app (0 = rainbow, the default). The app
// knows how many themes exist; only the number is stored here.
uint8_t ajustes_tema_espectro(void);
void    ajustes_definir_tema_espectro(uint8_t tema);

// ACCENT COLOUR OF THE system PLAYER (services/tab5_musica), RGB565.
// 0 = the system's (lilac). It is set by the player's UI (the Music
// app via its theme, a Berry app via its own); the service only publishes it.
uint16_t ajustes_cor_tocador(void);
void     ajustes_definir_cor_tocador(uint16_t cor565);

// PLAYER MODE (services/tab5_musica): bit 0 = shuffle, bits 1-2 =
// repeat (0 no, 1 the whole queue, 2 the same song). Applies when turned on again.
uint8_t ajustes_modo_tocador(void);
void    ajustes_definir_modo_tocador(uint8_t modo);

// ESP-NOW CHANNEL. 0 = the router's; 1..13 = fixed channel (the Wi-Fi
// drops the router). It was RAM only: every restart went back to the router and the
// Cardputer on channel 1 vanished until someone reopened the app and chose again.
// The one that applies it is the ESP-NOW service (tab5_espnow) - only the choice lives here.
uint8_t ajustes_espnow_canal(void);
void    ajustes_definir_espnow_canal(uint8_t canal);

// ESP-NOW CONVERSATION TEXT SIZE: 0 = default, 1..3 = larger (the
// "Aa" button on the app's bar steps from one to the next). Stored so it does not
// go back to small on every restart.
#define AJUSTES_ESPNOW_LETRA_MAX 3
uint8_t ajustes_espnow_letra(void);
void    ajustes_definir_espnow_letra(uint8_t nivel);

// THE LAST KNOWN BATTERY CHARGE (0..100; AJUSTES_SOC_NENHUM = never
// saved). Saved by the battery guard (services/tab5_bateria) every 2
// points and on shutting down due to a flat battery: a boot WITH THE CABLE cannot start
// from the voltage, which while charging reads well above the real charge.
#define AJUSTES_SOC_NENHUM 255

// MAXIMUM SAVING WHILE IDLE (services/tab5_wifi_cochilo): with the screen off
// and nothing needing the network, Wi-Fi turns off and comes back when the screen lights or for
// 1 min every 15. OFF from the factory: the default is to stay connected in
// power-saving mode (P4 sleeping with a heartbeat, C6 in maximum modem sleep) - when the
// screen lights the network is already there.
bool    ajustes_wifi_cochilo(void);
// P4 LIGHT SLEEP WHILE IDLE (power-saving contract, kernel/app.h). On from the
// factory; 'energia sono off' on the console turns it off (safety net).
bool    ajustes_sono_leve(void);
void    ajustes_definir_sono_leve(bool ligado);
void    ajustes_definir_wifi_cochilo(bool ligado);
// MUSIC COVER ART FROM THE INTERNET (services/tab5_musica): with no cover in the file or
// in the folder, the title and artist of the playing song go to an iTunes search.
// On from the factory; when off, nothing from the library leaves the device.
// THE C6 SLEEPING WHILE ASSOCIATED DURING IDLE (hal/tab5_c6_sono): with the screen off the
// SDIO link is torn down and the C6 enters light sleep, and ESP-NOW only listens with the
// screen on. Off from the factory until battery consumption has been measured.
bool    ajustes_c6_sono(void);
void    ajustes_definir_c6_sono(bool ligado);
bool    ajustes_capa_internet(void);
// The settings from before the power test (services/tab5_ensaio), while
// it runs: wifi, bluetooth, grove, mesh_internet, mesh_via. nullptr clears them.
#define AJUSTES_ENSAIO_N 5
bool    ajustes_ensaio_originais(uint8_t out[AJUSTES_ENSAIO_N]);
void    ajustes_definir_ensaio_originais(const uint8_t* originais);
void    ajustes_definir_capa_internet(bool ligado);
uint8_t ajustes_bateria_soc(void);
void    ajustes_definir_bateria_soc(uint8_t pct);

// HOW THE TAB5 TALKS TO THE MESHTASTIC MESH. Serial = the Grove cable
// (PROTO/UART); Bluetooth = BLE via the internal C6, wireless; Internet = MQTT
// over Wi-Fi, with no node at all. The PIN is the one for Meshtastic BLE pairing
// (default 123456 on devices without a screen).
//
// THE GROVE TAKES PRIORITY: when probing FINDS a C6L on the cable, the route becomes
// Serial by itself (kernel/core.cpp). After that the manual choice holds until the
// next find - and the internet can also be chosen this way, as well as
// covering for a missing node (ajustes_mesh_internet).
#define AJUSTES_MESH_SERIAL     0
#define AJUSTES_MESH_BLUETOOTH  1
#define AJUSTES_MESH_INTERNET   2
uint8_t  ajustes_mesh_via(void);

// MESH OVER THE INTERNET WHEN THE NODE IS NOT CONNECTED: with Wi-Fi connected and
// no C6L (cable or Bluetooth), messages arrive via the MQTT server.
// On from the factory.
bool     ajustes_mesh_internet(void);
void     ajustes_definir_mesh_internet(bool ligado);
void     ajustes_definir_mesh_via(uint8_t via);
uint32_t ajustes_mesh_pin(void);
void     ajustes_definir_mesh_pin(uint32_t pin);
// The chosen node shows the PIN on its screen (a new one on every pairing): the saved PIN
// is no use, and the system always asks. Learnt on the first rejection of the saved
// PIN; goes back to false when another node is chosen.
bool     ajustes_mesh_pin_na_tela(void);
void     ajustes_definir_mesh_pin_na_tela(bool sim);
// BLE address of the chosen node ("" = the first Meshtastic that shows up).
void     ajustes_mesh_ble_endereco(char out[18]);
void     ajustes_definir_mesh_ble_endereco(const char* end);

// TAB5 GPS FOR THE MESH: every how many MINUTES the Tab5 passes on the
// GPS (Grove) position. 0 = never (default: position is personal data -
// it only goes out if the user asks). With the node on the other end it goes ONLY TO THE NODE, which
// broadcasts it at its own rate and precision (see enviar_posicao in
// tab5_mesh.cpp); over the internet, with no node, the Tab5 broadcasts it itself. Edited in
// the Mesh app, Configurar o no > Localizacao.
uint8_t ajustes_mesh_posicao_min(void);
void    ajustes_definir_mesh_posicao_min(uint8_t minutos);

// BLUETOOTH ON. The Bluetooth route in Mesh turns it on as well (see the Settings app).
bool ajustes_bluetooth(void);
void ajustes_definir_bluetooth(bool ligado);

// Lock screen: on waking from idle the device shows the time and date and asks for
// a swipe up. Off from the factory.
bool ajustes_bloqueio(void);
void ajustes_definir_bloqueio(bool ligado);

// FLASH DRIVE ON CONNECT: plugged into a computer over USB-C, the Tab5 opens the
// Pendrive app (the SD card becomes a disk on the computer). Off from the factory: the
// flash drive takes the console off the cable, and whoever flashes firmware over USB-C does not
// want that happening by itself. With an app open it does not interrupt - it only notifies
// (notification centre). The Pendrive app opens by hand with the setting off.
bool ajustes_pendrive(void);
void ajustes_definir_pendrive(bool ligado);

// RECYCLE BIN: automatically delete items older than N days (30 = 30 days). 0 = never
// (default: what goes into the bin stays until it is emptied). The one that applies it is
// tab5_arq_lixeira_manter (services/tab5_arquivos).
uint8_t ajustes_lixeira_dias(void);
void    ajustes_definir_lixeira_dias(uint8_t dias);

// GYROSCOPE SCALE, per BMI270 axis (0 = x, 1 = y, 2 = z). The chip leaves
// the factory with a sensitivity error of a few per cent, and Radar's heading
// inherits that error on every turn. Measured in Radar itself (full turns against
// a reference); 1.0 = no correction. Accepted range 0.90..1.10.
float ajustes_giro_escala(int eixo);
void  ajustes_definir_giro_escala(int eixo, float escala);

// ORDER OF THE ICONS ON THE HOME SCREEN, as the app NAMES separated by
// commas ("Mesh,Trilha,Radar"). Name and not index: a new app in the firmware
// or a removed app does not shuffle the rest - whoever is not in the list goes at the
// end, in apps.cpp order, and a name that no longer exists is ignored. Empty = the
// apps.cpp order. The shell interprets it (sistema_home_*, app.h).
void ajustes_ordem_apps(char* out, size_t n);
void ajustes_definir_ordem_apps(const char* nomes);   // "" or nullptr = default

// INTERFACE LANGUAGE (the code of the chosen .po). It is interpreted by
// services/idioma.h - the UI uses idioma_*, not these.
const char* ajustes_idioma(void);                 // "pt", "en", "fr"...
void        ajustes_definir_idioma(const char* codigo);

// HOME SCREEN WALLPAPER (home): fine abstract lines on black, the
// same drawings as the lock screen background. SORTEIO (Random, the factory default) picks
// a drawing at each boot and keeps it until the next; 1..5 fixes one; NENHUM is the
// plain black of before.
#define AJUSTES_PAREDE_SORTEIO  0
#define AJUSTES_PAREDE_ESTILOS  5   // 1..5 = threads, waves, web, rays, circles
#define AJUSTES_PAREDE_NENHUM   6

uint8_t ajustes_parede(void);
void    ajustes_definir_parede(uint8_t opcao);

// ---------------------------------------------------------------------------
// FIRST USE, TIME ZONE AND MESH-OVER-INTERNET SERVER
// ---------------------------------------------------------------------------

// HAS THE INITIAL SET-UP BEEN DONE? false only on a device that has never run the
// system (empty NVS - including after flashing with M5Burner, which wipes
// the settings): boot opens the welcome wizard (kernel/boas_vindas).
// Anyone who already had ANY setting saved counts as configured - updating the
// firmware does not bring the wizard back.
bool ajustes_config_inicial(void);
void ajustes_definir_config_inicial(bool feita);

// TIME ZONE, in minutes from UTC (-180 = UTC-3, the default). Only the
// DISPLAY uses it (tab5_hora_local): the RTC, logs and timestamps stay in UTC.
// Fixed, no daylight saving - anyone with daylight saving adjusts twice a year.
#define AJUSTES_FUSO_MIN_MIN  (-12 * 60)
#define AJUSTES_FUSO_MAX_MIN  ( 14 * 60)
int16_t ajustes_fuso_min(void);
void    ajustes_definir_fuso_min(int16_t minutos);

// TIME-ZONE REGION (services/tab5_fuso): 0 = manual, the fixed offset above;
// any other region brings its own offset and daylight-saving rule. Picking a
// region also stores its standard offset in ajustes_fuso_min, so code that
// only reads the offset (the MQTT server guess, Berry's ajustes.fuso) keeps
// a sensible value.
// 12-HOUR CLOCK: times shown as "9:05pm" (tab5_hora_hhmm) instead of "21:05".
bool    ajustes_relogio_12h(void);
void    ajustes_definir_relogio_12h(bool sim);

uint8_t ajustes_fuso_zona(void);
void    ajustes_definir_fuso_zona(uint8_t zona);

// MQTT SERVERS FOR Mesh INTERNET MODE (no node: the Tab5 talks to the mesh
// over Wi-Fi). It is THE PERSON'S OWN LIST, not one fixed in the code: up to
// AJUSTES_MQTT_MAX servers, each with a name, address, user, password
// (stored encrypted, like the Wi-Fi ones - key derived from the chip) and topic
// root. Sizes: those of Meshtastic's MQTT.
//
// THE CHOICE: 0 = AUTOMATIC, the long-standing behaviour (the server the node
// taught, or the Brazilian community's); 1..n = item n-1 of the list, which
// overrides everything. Deleting an item fixes up the choice as well.
//
// From the factory the list has Mesh Brasil and Meshtastic's public server at the
// Brazil root. The TEMPLATES for adding and the meaning of each field live in
// services/tab5_mesh_mqtt (the UI uses mesh_mqtt_*, not these).
#define AJUSTES_MQTT_AUTOMATICO  0
#define AJUSTES_MQTT_MAX         8
typedef struct { char nome[24], srv[64], usr[32], pwd[32], raiz[48]; } ajustes_mqtt_srv_t;
uint8_t ajustes_mqtt_servidor(void);
void    ajustes_definir_mqtt_servidor(uint8_t escolha);
int     ajustes_mqtt_n(void);
bool    ajustes_mqtt_ler(int i, ajustes_mqtt_srv_t* out);          // i = 0..n-1
int     ajustes_mqtt_gravar(int i, const ajustes_mqtt_srv_t* s);   // i = -1: new. Returns the index; -1 = full
void    ajustes_mqtt_apagar(int i);
