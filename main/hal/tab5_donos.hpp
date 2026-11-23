// OWNER OF EACH TASK - who is responsible for each running task, in the
// spirit of task managers' "user / system".
//
// FreeRTOS only knows a task's NAME. Guessing the owner from the name would
// be a table that goes stale with every new task. Here the owner is DECLARED
// by whoever creates the task, at creation time:
//   - SERVICO: system services (WiFi, ESP-NOW, Mesh, GPS, log...) - register
//     their own tasks;
//   - APP:     task created by an open app (tarefa_app_criar, called from the
//     UI with an app in front) - the kernel registers it itself;
//   - FUNDO:   an app's background part (kernel/fundo) - the kernel registers;
//   - SISTEMA: everything else, and the default: kernel, drivers, IDF itself.
//
// Lives in hal/ and depends only on FreeRTOS: HAL, services and kernel
// register without anyone coming to depend on a higher layer. 'dono' is a
// string that lives for ever (a literal, or the app/background name, which
// are literals too).
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
  DONO_SISTEMA = 0,
  DONO_SERVICO,
  DONO_APP,
  DONO_FUNDO,
} dono_tipo_t;

// Registers (or changes) the owner. Call right after creating the task.
void tab5_dono_registrar(TaskHandle_t h, dono_tipo_t tipo, const char* dono);

// The task is about to die: remove it from the registry (the handle may be
// reused by another task later). Call BEFORE vTaskDelete.
void tab5_dono_esquecer(TaskHandle_t h);

// The registered owner; DONO_SISTEMA (and dono = NULL) if nobody registered.
dono_tipo_t tab5_dono_de(TaskHandle_t h, const char** dono);
