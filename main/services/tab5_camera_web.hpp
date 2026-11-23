// THE CAMERA IN THE PC'S BROWSER - the /camera/ part of the web server (tab5_web),
// on the same pattern as the Terminal, the Editor and Mesh.
//
// VIDEO FRAME BY FRAME, NOT A STREAM. The page asks for one JPEG after another
// (quadro.jpg); each request takes the camera's next frame at the video
// size (tab5_camera_pedir_quadro), compresses it on the chip's JPEG encoder and
// replies. An endless MJPEG stream would tie up the server's ONLY task -
// the Terminal, Mesh and the Editor would stop responding while the video
// was open; frame by frame, they get in between one frame and the next.
//
//   GET quadro.jpg?alta=1   the next frame in LANDSCAPE, as the sensor sees it: 640x480
//                           (960x720 with 'alta') - the page rotates it for display
//   GET foto.jpg            the sensor's full frame, 1200x1600 (for downloading)
//
// THE CAMERA SWITCHES ON BY ITSELF on the first requested frame and SWITCHES OFF
// CAMERA_OCIOSA_MS after the last (closing the tab, switching app in the portal):
// opening the part doesn't switch the sensor on. It is the system's (hal/tab5_camera,
// reference-counted): the Camera app open on the device at the same time
// keeps working - each with its own size.
#pragma once

// Opens the camera part on the server (/camera/). From any task.
// false = see tab5_web_abrir.
bool tab5_camera_web_ligar(void);
