// The device's own environmental sensors.
//
// THERE IS NO HUMIDITY SENSOR IN THE TAB5. I looked: M5Unified declares none
// for this board, and the internal bus only has touch (GT911), the INA226 for
// power, the IMU and the expanders. Humidity would need a Grove module - and
// Grove is already taken.
//
// TEMPERATURE exists, with a caveat that must go with the number: it is the
// SILICON temperature of the ESP32-P4, not the air's. The chip heats up, so it
// sits above ambient - typically well above with the screen on. It is for
// seeing the device warm up (and the effect of DFS), not for knowing if the room
// is cold.
//
// Chosen over the IMU temperature for two reasons: it costs no I2C
// transaction at all (it is an internal ADC) and doesn't depend on the IMU being awake.
//
// MODULE, NOT A QUEUE API, FOR NOW. Today the only consumer is the status
// bar, and a queue for a single reader would be structure without load. The reading is already
// cached - whoever consumes it later reads the same function, and the queue comes in on the day
// someone waits for an event instead of asking.
#pragma once

#include <stdbool.h>

// Switches the sensor on. Once, at boot. Without this the readings return false.
void tab5_sensores_iniciar(void);

// Reads the sensor FOR REAL and updates the cache. UI task only, alongside
// energy sampling - it is cheap, but not free.
void tab5_sensores_medir(void);

// Last reading. false when the sensor didn't come up or hasn't measured yet.
bool tab5_sensores_temp(float* celsius);

// Average of all readings since boot - this device's "normal" for this
// session, so the current reading has something to be compared with. false
// while there are none.
bool tab5_sensores_temp_media(float* celsius);
