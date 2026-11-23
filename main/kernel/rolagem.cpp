#include "tab5_tema.hpp"
#include "rolagem.hpp"
#include "tab5_tela.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void rolagem_definir(rolagem_t* r, uint8_t eixo, uint8_t ancoragem)
{
  r->atual = r->no_visor = r->max = 0;
  r->ancora = r->descida = -1;
  r->afastamento = 0;
  r->eixo = eixo;
  r->ancoragem = ancoragem;
  r->passo_encaixe = 0;
}

void rolagem_zerar(rolagem_t* r)
{
  r->atual = 0;
  r->max = 0;   // a consumer that keeps 'max' between passes (Mesh) is not
                // left with a stale limit from a previous screen/conversation in
                // the instant between this and the next draw recalculating; whoever
                // recalculates 'max' EVERY cycle (the kernel) does not even notice.
  r->ancora = -1;
  r->descida = -1;
  r->afastamento = 0;
}

bool rolagem_arrastar(rolagem_t* r, int coord, bool dentro)
{
  // RELEASE THE ANCHOR ON LEAVING THE ZONE, not only when the finger lifts for good -
  // a pattern unified in the consolidation (the three Mesh lists
  // already did this; the kernel used to release only on '!tocando', tolerating the
  // finger wandering outside the usable area without letting go of the anchor - a finger that left
  // and came back jumped the whole distance at once, because the anchor stayed
  // at the last point INSIDE. Releasing on leaving avoids that jump, and it is the
  // behaviour that already applied to most of the lists in this system.
  if (!dentro) { r->ancora = -1; return false; }

  // First sample inside the zone: only anchor, there is no 'previous cycle'
  // to take any delta from - avoiding that is what prevents a jump on
  // ENTERING the zone (the finger may have come from far outside it).
  if (r->ancora < 0) { r->ancora = coord; r->descida = coord; r->afastamento = 0; return false; }

  const int dy = coord - r->ancora;
  if (dy == 0) { return false; }
  r->ancora = coord;

  // DISTANCE FROM THE DOWN POINT, not the sum of the path travelled - a finger that
  // trembles (or goes back and forth) in place must not be read as a big drag just
  // for having "moved" a lot back and forth. Same historical logic as the kernel.
  const int longe = (coord > r->descida) ? (coord - r->descida) : (r->descida - coord);
  if (longe > r->afastamento) { r->afastamento = longe; }

  // SIGN: an ordinary list goes up/down with the finger (finger up = content
  // goes up, hence subtract); anchored at the end does the opposite (finger down
  // reveals older content, hence add).
  int nova = (r->ancoragem == ROLAGEM_INICIO) ? (r->atual - dy) : (r->atual + dy);
  if (nova < 0)      { nova = 0; }
  if (nova > r->max) { nova = r->max; }
  if (nova == r->atual) { return false; }

  r->atual = nova;
  return true;
}

bool rolagem_foi_arraste(rolagem_t* r)
{
  const bool foi = r->afastamento > ROLAGEM_ARRASTE_MIN;
  r->afastamento = 0;
  return foi;
}

bool rolagem_limite(rolagem_t* r, int total, int janela)
{
  int novo_max = total - janela;
  if (novo_max < 0) { novo_max = 0; }
  r->max = novo_max;
  if (r->atual > r->max) {
    // The list shrank (an item was removed, a conversation changed) and the
    // current position ended up past the end - pull it back.
    r->atual = r->max;
    return true;
  }
  return false;
}

rolagem_passada_t rolagem_deslocar(rolagem_t* r, const area_t& janela, uint16_t cor)
{
  rolagem_passada_t p = {};
  p.delta = (r->ancoragem == ROLAGEM_INICIO) ? (r->atual - r->no_visor)
                                             : (r->no_visor - r->atual);
  p.deslocou = tab5_tela_rolar(janela.x, janela.y, janela.largura, janela.altura, p.delta, cor);
  if (p.deslocou) {
    const int ad = (p.delta > 0) ? p.delta : -p.delta;
    p.exposta = area_t{
      janela.x,
      (p.delta > 0) ? (janela.y + janela.altura - ad) : janela.y,
      janela.largura, ad
    };
  }
  // ALWAYS synchronise, whether it shifted or not: if it did not shift, the caller
  // will clear/redraw the whole window on its own (each in its
  // own way - see the big note in the .hpp), and after that the visible
  // page ALSO reflects 'atual', just by another path.
  r->no_visor = r->atual;
  return p;
}

void rolagem_barra(lgfx::LovyanGFX& d, const rolagem_t* r, const area_t& janela,
                   uint16_t cor_trilha, int margem)
{
  if (r->max <= 0) { return; }
  // Extra gate only for anchored-at-the-end: with atual==0 the "end" is already in
  // view, the little bar there would be noise (the same rule Mesh messages
  // already had before this consolidation).
  if (r->ancoragem == ROLAGEM_FIM && r->atual <= 0) { return; }

  const int bx  = janela.x + janela.largura - margem;
  const int alt = (janela.altura * janela.altura) / (janela.altura + r->max);
  const int pos = (r->ancoragem == ROLAGEM_INICIO) ? r->atual : (r->max - r->atual);
  const int by  = janela.y + ((janela.altura - alt) * pos) / r->max;

  d.fillRect(bx, janela.y, 4, janela.altura, cor_trilha);
  d.fillRoundRect(bx, by, 4, alt, 2, TEMA_LINHA);   // the theme's divider colour
}
