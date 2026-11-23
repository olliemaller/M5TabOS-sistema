// See tab5_toque_gt911.hpp: the driver defect and why this class exists.
#include "tab5_toque_gt911.hpp"
#include "tab5_power.hpp"   // tab5_painel_toque_gt911
#include "esp_log.h"
#include <algorithm>
#include "lgfx/v1/touch/Touch_GT911.hpp"   // M5GFX.h doesn't expose the GT911 class

static const char* TAG = "toque_gt911";

// I2C failure counter (diagnostics only).
static volatile uint32_t s_falhas = 0;

// GT911 command: writing 0 to 0x814E tells the chip the report has been read.
static constexpr uint8_t CMD_LIMPAR[] = { 0x81, 0x4E, 0x00 };

namespace {

class Toque_GT911 : public lgfx::Touch_GT911 {
public:
  // Takes over the touch configuration M5GFX has already built and started. Doesn't call
  // init(): the chip was already started by M5GFX and repeating it (i2c::init and the INT
  // pulse in wakeup()) would only disturb the bus already in use.
  void adotar(const config_t& cfg) {
    _cfg = cfg;
    _inited = true;
  }

  uint_fast8_t getTouchRaw(lgfx::touch_point_t* tp, uint_fast8_t count) override {
    if (!_inited || count == 0) return 0;
    if (count > 5) count = 5;

    // INT low = the chip has a new report. INT high: nothing
    // new, the last GOOD state holds (same as the original).
    if (_cfg.pin_int < 0 || !lgfx::gpio_in(_cfg.pin_int)) {
      const uint32_t agora = lgfx::millis();
      const uint32_t passou = agora - _ultima;
      _ultima = agora;

      _falhou = false;
      if (passou >= 128) {
        // The GT911 repeats the same report until it gets the 0 at 0x814E; after
        // a long idle the report is stale. Discard and reread, up to 24x.
        escrever(CMD_LIMPAR, 3);
        int tentativas = 24;
        do {
          lgfx::delay(1);
        } while (ler() != Leitura::nova && --tentativas);
      } else {
        ler();
      }

      // FIX: failed read => no finger, and the stale buffer is thrown away.
      if (_falhou) {
        _dados[0] = 0;
        s_falhas = s_falhas + 1;
        return 0;
      }
    }

    // Decodes the points: 8 bytes each (id, x, y, size), same as the original.
    const uint32_t pontos = std::min<uint_fast8_t>(count, _dados[0] & 0x0F);
    for (uint32_t i = 0; i < pontos; ++i) {
      const auto* d = reinterpret_cast<const uint16_t*>(&_dados[i * 8]);
      tp[i].id   = d[0] >> 8;
      tp[i].x    = d[1];
      tp[i].y    = d[2];
      tp[i].size = d[3];
    }
    return pontos;
  }

private:
  enum class Leitura { nova, sem_novidade, falha };
  static constexpr int MAX_PONTOS = 5;

  uint32_t _ultima = 0;                    // ms of the last query
  bool     _falhou = false;                // the last ler() had an I2C failure
  uint8_t  _dados[MAX_PONTOS * 8 + 2] = {};  // [0]=status, then the points

  bool escrever(const uint8_t* dados, size_t n) {
    return lgfx::i2c::transactionWrite(_cfg.i2c_port, _cfg.i2c_addr, dados, n, _cfg.freq).has_value();
  }

  // Reads the status (0x814E) and, if there are points, the points. Only writes to _dados if the
  // whole read succeeded; an I2C failure sets _falhou.
  Leitura ler(void) {
    if (!lgfx::i2c::beginTransaction(_cfg.i2c_port, _cfg.i2c_addr, _cfg.freq, false).has_value()) {
      _falhou = true;
      return Leitura::falha;
    }
    uint8_t status = 0;
    bool i2c_ok = lgfx::i2c::writeBytes(_cfg.i2c_port, CMD_LIMPAR, 2).has_value()
               && lgfx::i2c::restart(_cfg.i2c_port, _cfg.i2c_addr, _cfg.freq, true).has_value()
               && lgfx::i2c::readBytes(_cfg.i2c_port, &status, 1).has_value();
    bool pronto = i2c_ok && (status & 0x80);
    if (pronto) {
      const uint_fast8_t pontos = std::min<uint_fast8_t>(MAX_PONTOS, status & 0x0Fu);
      if (pontos) {
        pronto = lgfx::i2c::readBytes(_cfg.i2c_port, &_dados[1], pontos * 8, true).has_value();
        i2c_ok = pronto;
      }
    }
    lgfx::i2c::endTransaction(_cfg.i2c_port);

    if (!i2c_ok) {
      _falhou = true;
      return Leitura::falha;
    }
    if (!pronto) return Leitura::sem_novidade;   // chip has no new report: not an error

    _dados[0] = status;
    escrever(CMD_LIMPAR, 3);                    // tells the chip we've read it
    _falhou = false;
    return Leitura::nova;
  }
};

// Owner of the new touch. The old one stays with M5GFX (shared_ptr _touch_last):
// the panel only keeps a raw pointer, so the old one stays alive, idle, and is never
// freed by us. We never free this one: it lives until reset.
Toque_GT911 s_toque;

}  // namespace

bool tab5_toque_gt911_instalar(void) {
  if (!tab5_painel_toque_gt911()) return false;   // only the ILI9881C has a GT911

  auto* painel = M5.Display.panel();
  auto* antigo = M5.Display.touch();
  if (painel == nullptr || antigo == nullptr) {
    ESP_LOGW(TAG, "no panel or no touch - called before M5.begin()?");
    return false;
  }
  if (antigo == &s_toque) return true;            // already installed

  s_toque.adotar(antigo->config());
  // setTouch redoes the calibration from the same config: no change in
  // coordinates. M5.Touch queries the display on every update (it doesn't keep the
  // ITouch), so it starts using the new one on the next read.
  painel->setTouch(&s_toque);
  ESP_LOGI(TAG, "GT911 touch with I2C-failure protection installed");
  return true;
}

uint32_t tab5_toque_gt911_falhas(void) { return s_falhas; }
