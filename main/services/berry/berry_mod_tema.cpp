// Berry module 'tema' - the colours and measurements of the central theme (tab5_tema.hpp)
// for Berry apps. They are only constants: the script writes tema.FUNDO instead
// of a colour number, and follows along by itself when the system palette changes.
//
//   tela.limpar(tema.FUNDO)
//   tela.arredondado(x, y, l, a, tema.RAIO_M, tema.SUPERFICIE)
//
// See berry_apps.cpp (the registration) and berry_interno.hpp (membro_t).

#include "tab5_tema.hpp"
#include "berry_interno.hpp"

const membro_t BERRY_MOD_TEMA[] = {
  // surfaces
  { "FUNDO", nullptr, TEMA_FUNDO },
  { "SUPERFICIE", nullptr, TEMA_SUPERFICIE },
  { "SUPERFICIE_ALTA", nullptr, TEMA_SUPERFICIE_ALTA },
  { "LINHA", nullptr, TEMA_LINHA },
  // text
  { "TEXTO", nullptr, TEMA_TEXTO },
  { "TEXTO_2", nullptr, TEMA_TEXTO_2 },
  { "TEXTO_3", nullptr, TEMA_TEXTO_3 },
  // accent
  { "DESTAQUE", nullptr, TEMA_DESTAQUE },
  { "DESTAQUE_SUAVE", nullptr, TEMA_DESTAQUE_SUAVE },
  { "SOBRE_DESTAQUE", nullptr, TEMA_SOBRE_DESTAQUE },
  // states
  { "SUCESSO", nullptr, TEMA_SUCESSO },
  { "ALERTA", nullptr, TEMA_ALERTA },
  { "PERIGO", nullptr, TEMA_PERIGO },
  { "INFO", nullptr, TEMA_INFO },
  // app colours
  { "APP_AZUL", nullptr, TEMA_APP_AZUL },
  { "APP_CIANO", nullptr, TEMA_APP_CIANO },
  { "APP_VERDE", nullptr, TEMA_APP_VERDE },
  { "APP_LIMA", nullptr, TEMA_APP_LIMA },
  { "APP_AMARELO", nullptr, TEMA_APP_AMARELO },
  { "APP_LARANJA", nullptr, TEMA_APP_LARANJA },
  { "APP_VERMELHO", nullptr, TEMA_APP_VERMELHO },
  { "APP_ROSA", nullptr, TEMA_APP_ROSA },
  { "APP_ROXO", nullptr, TEMA_APP_ROXO },
  { "APP_GRAFITE", nullptr, TEMA_APP_GRAFITE },
  // corners, spacing and lettering
  { "RAIO_P", nullptr, TEMA_RAIO_P },
  { "RAIO_M", nullptr, TEMA_RAIO_M },
  { "RAIO_G", nullptr, TEMA_RAIO_G },
  { "MARGEM", nullptr, TEMA_MARGEM },
  { "LETRA_LEGENDA", nullptr, TEMA_LETRA_LEGENDA },
  { "LETRA_CORPO", nullptr, TEMA_LETRA_CORPO },
  { "LETRA_SUBTITULO", nullptr, TEMA_LETRA_SUBTITULO },
  { "LETRA_TITULO", nullptr, TEMA_LETRA_TITULO },
  { "LETRA_GRANDE", nullptr, TEMA_LETRA_GRANDE },
  { "TOQUE_MIN", nullptr, TEMA_TOQUE_MIN },
  { nullptr },
};
