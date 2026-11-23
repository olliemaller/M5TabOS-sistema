// BOOT ANIMATION - the one that appears when the Tab5 is SWITCHED ON (not to be confused with an
// app's "opening", the animation of the icon growing).
//
// A living web: seven FIXED main nodes (the mesh drawing, slightly
// more prominent) and background points that wander, linking and unlinking
// with each other and with the nodes. Characters fly from the nodes to the centre, the
// name "M5tabOS" is decrypted letter by letter and "[ conectado ]" appears.
// The approved mock-up is in the 2026-11-15 entry of docs/PASSAGEM.md.
//
// WHEN: only at boot, called by the UI task (kernel/core.cpp)
// after the panel has taken on the final clock and the virtual screen exists. On
// waking from idle it does NOT appear. A touch on the screen skips the rest.
//
// COST TO BOOT: nothing extra. The animation fills the time in which the
// UI task would only be waiting; the rest of boot (USB, flash drive, radio)
// carries on in parallel in app_main.
//
// DEVELOPMENT: the TAB5_MOSTRAR_ANIM_BOOT switch (kernel/internal.hpp)
// disables the whole animation - whoever restarts the device constantly to
// test doesn't need to sit through it every time.
#pragma once

// Shows the whole animation (~1.9 s, or until a touch) and returns the screen already
// ready for the system's first drawing.
void boot_anim_mostrar(void);
