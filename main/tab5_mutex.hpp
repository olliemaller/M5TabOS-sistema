// TRAVA (lock) - a mutex that exists before any task.
//
//   static Trava s_trava;
//   s_trava.travar(); ... s_trava.destravar();
//   { Trava::Guarda g(s_trava); ... }   // unlocks by itself at the end of the block
//
// The mutex is born in the constructor, which runs with the global constructors - before
// app_main and before any concurrency exists. There is no "create the first time", nor
// a race in that creation, nor an _iniciar() to remember to call. The mutex comes
// from the heap (internal RAM), as before: only the Trava object may live in PSRAM.
//
// tab5_sd.cpp keeps its mutex created in its tab5_sd_iniciar() - the
// choice there is deliberate (see the comment there).
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Trava {
public:
  Trava() : m_(xSemaphoreCreateMutex()) {}
  Trava(const Trava&) = delete;
  Trava& operator=(const Trava&) = delete;

  void travar(void)    { xSemaphoreTake(m_, portMAX_DELAY); }
  bool travar(TickType_t espera) { return xSemaphoreTake(m_, espera) == pdTRUE; }
  void destravar(void) { xSemaphoreGive(m_); }

  // For code that still passes the handle directly to xSemaphoreTake/Give.
  operator SemaphoreHandle_t() const { return m_; }

  class Guarda {
  public:
    explicit Guarda(Trava& t) : t_(t) { t_.travar(); }
    ~Guarda() { t_.destravar(); }
    Guarda(const Guarda&) = delete;
    Guarda& operator=(const Guarda&) = delete;
  private:
    Trava& t_;
  };

private:
  SemaphoreHandle_t m_;
};
