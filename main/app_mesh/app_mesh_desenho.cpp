// APP Mesh - the DRAWING: the column glyphs, the Meshtastic logo, the rail, the
// conversations column, the node grid and table, search, the node detail and the
// mesh. Split from app_mesh.cpp (the app cycle); what the parts share
// is in app_mesh_interno.hpp.

#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include "idioma.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "ajustes.h"
#include "tab5_gps.hpp"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app_mesh_interno.hpp"
#include "tab5_tema.hpp"

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// "caixa" (box) is the name this app already used for tab5_botao() (see tab5_ui.hpp) -
// kept here as a thin alias so as not to rename every call. Even
// this used to be another local copy of the same button that existed in three
// more apps.
static void caixa(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                  const char* txt, uint16_t fundo, uint16_t tinta, float tam)
{
  tab5_botao(d, x, y, l, a, txt, fundo, tinta, tam);
}

// ---------------------------------------------------------------------------
// COLUMN GLYPHS
// ---------------------------------------------------------------------------
// The letters "C", "N" and "M" were the built-in font at size 4 - each pixel of it
// became a 4x4 square, and that was what made them jagged. Same solution as the home
// screen icons: primitives as a fraction of the size, which come out clean at any
// scale and also say what the letter only abbreviated.
//
// THE COLOUR COMES FROM OUTSIDE because each glyph has two states: selected draws in
// black over the box's blue, unselected in white over the dark background.
// The drawing is the same in both - only the ink changes.
// GEAR: the "this is configured here" sign - on the rail and next to each
// option that can be changed. Eight teeth in thick lines, the body and the hole
// in the background colour (passed in by the caller).
static void engrenagem(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, uint16_t fundo)
{
  int dente = r * 2 / 5;
  if (dente < 2) { dente = 2; }
  for (int k = 0; k < 8; ++k) {
    const float ang = (float)M_PI * k / 4.0f;
    tab5_linha_grossa(d, cx + (int)(r * 0.55f * cosf(ang)), cy + (int)(r * 0.55f * sinf(ang)),
                   cx + (int)(r * cosf(ang)),         cy + (int)(r * sinf(ang)), dente, cor);
  }
  d.fillCircle(cx, cy, r * 3 / 4, cor);
  d.fillCircle(cx, cy, r / 3, fundo);
}

static void glifo(lgfx::LovyanGFX& d, glifo_t g, int cx, int cy, int lado, uint16_t cor)
{
  const int r = lado / 2;
  int gr = lado / 12;                       // base thickness
  if (gr < 2) { gr = 2; }

  switch (g) {
    case GL_CONVERSAS: {
      // Speech bubble: rounded body and the tail at the bottom left. It is the
      // universal drawing for "chatting happens here".
      const int l = (int)(r * 1.5f), a = (int)(r * 1.1f);
      d.drawRoundRect(cx - l/2, cy - a/2 - r/6, l, a, a/3, cor);
      d.drawRoundRect(cx - l/2 + 1, cy - a/2 - r/6 + 1, l - 2, a - 2, a/3, cor);
      d.fillTriangle(cx - l/6, cy + a/2 - r/6 - 1,
                     cx - l/6 + a/3, cy + a/2 - r/6 - 1,
                     cx - l/3, cy + a/2 + r/3, cor);
      break;
    }
    case GL_NOS: {
      // List: three dots, each with its own line. It is a REGISTER - that was the
      // difference that was missing so it would not be confused with the mesh.
      for (int i = -1; i <= 1; ++i) {
        const int y = cy + i * (r / 2);
        d.fillCircle(cx - r * 3 / 5, y, gr, cor);
        d.fillRect(cx - r / 5, y - gr / 2, r, gr, cor);
      }
      break;
    }
    case GL_MALHA: {
      // Graph: a node in the centre and four around it, linked. Here the information is
      // the LINK, and not the list - that is why the edges are what you see first.
      const int d4[4][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1} };
      for (int i = 0; i < 4; ++i) {
        const int x = cx + d4[i][0] * r * 3 / 5;
        const int y = cy + d4[i][1] * r * 3 / 5;
        d.drawLine(cx, cy, x, y, cor);
        d.fillCircle(x, y, gr + 1, cor);
      }
      d.fillCircle(cx, cy, gr + 2, cor);
      break;
    }
    case GL_CONFIG: {
      // The hole's background colour: the accent blue in the selected box (light ink),
      // the rail background in the unselected one - the ink says which.
      engrenagem(d, cx, cy, r * 3 / 4, cor, cor == TEMA_SOBRE_DESTAQUE ? TEMA_DESTAQUE : TEMA_FUNDO);
      break;
    }
    case GL_DIFUSAO: {
      // Broadcast: a dot and waves leaving it on both sides. Speaks to
      // everyone, not to someone - that is the difference the icon needs to
      // carry against GL_DIRETO.
      d.fillCircle(cx, cy, gr + 1, cor);
      for (int k = 1; k <= 2; ++k) {
        const int rr = k * r / 2;
        d.drawArc(cx, cy, rr, rr - gr + 1,  30,  150, cor);
        d.drawArc(cx, cy, rr, rr - gr + 1, 210,  330, cor);
      }
      break;
    }
    case GL_DIRETO: {
      // A person: head and shoulders. A direct conversation is with SOMEONE.
      d.fillCircle(cx, cy - r / 3, r / 3, cor);
      d.fillArc(cx, cy + r * 3 / 4, r * 2 / 3, r * 2 / 3 - gr * 2, 180, 360, cor);
      break;
    }
  }
}

// Box with a glyph in place of the text. Same geometry and same colours as the text
// caixa(), so the two can live together without misaligning.
void caixa_glifo(lgfx::LovyanGFX& d, int x, int y, int l, int a,
                        glifo_t g, uint16_t fundo, uint16_t tinta)
{
  d.fillRoundRect(x, y, l, a, 10, fundo);
  const int lado = (l < a ? l : a) * 3 / 5;
  glifo(d, g, x + l / 2, y + a / 2, lado, tinta);
}

// ---------------------------------------------------------------------------
// THE MESHTASTIC LOGO
// ---------------------------------------------------------------------------
// Drawn with primitives and not stored as an image: that way it comes out at
// any size without jaggies, takes no flash, and the same code serves
// the 190px home screen icon and the 48px one on the rail.
//
// The geometry came from the official SVG (meshtastic/design, logo/logo.svg): green
// background rgb(103,234,148) and two ridges in rgb(44,45,60) - the full ridge on the
// right and a loose rise on the left, which is the leg of a second peak behind
// it. The design is inspired by LoRa chirps and reads as mountain, tent and the
// letter M at the same time; the ambiguity is intentional in the original.
//
// Coordinates as a FRACTION of the side, not in pixels. That is what allows the same
// stroke at both sizes without a second table to keep up to date.
void marca_mesh(lgfx::LovyanGFX& d, int x, int y, int lado)
{
  const uint16_t verde  = d.color565(103, 234, 148);
  const uint16_t escuro = d.color565(44, 45, 60);

  d.fillRoundRect(x, y, lado, lado, lado / 6, verde);

  // THICKNESS MEASURED FROM THE SVG, not estimated.
  //
  // The first version used 9% of the side and came out fat. In the original the stroke is
  // ~14.7 units and the ridge ~140 wide: 10.5% OF THE RIDGE. As the ridge
  // takes about half the icon, that leaves ~5% of the side - almost half the
  // first estimate.
  //
  // The 3px floor stays: below that the logo disappears instead of shrinking.
  int grosso = (lado * 5) / 100;
  if (grosso < 3) { grosso = 3; }

  #define MX(f) (x + (int)((f) * lado))
  #define MY(f) (y + (int)((f) * lado))

  // Main ridge. Proportion of the original: ~0.49 wide by ~0.36
  // high - flatter than the first version.
  tab5_linha_grossa(d, MX(0.365f), MY(0.72f), MX(0.610f), MY(0.34f), grosso, escuro);
  tab5_linha_grossa(d, MX(0.610f), MY(0.34f), MX(0.855f), MY(0.72f), grosso, escuro);

  // The loose rise on the left: SAME BASE, SAME TOP and SAME slope as the
  // ridge's left leg. It is the leg of a second peak behind, and a peak
  // behind is as tall as the one in front - stopping halfway, as it did,
  // it became a stray stroke instead of a mountain.
  //
  // Being parallel, the two never cross by construction: the separation is
  // constant over the whole height, and does not depend on the exact choice of
  // points.
  tab5_linha_grossa(d, MX(0.120f), MY(0.72f), MX(0.365f), MY(0.34f), grosso, escuro);

  #undef MX
  #undef MY
}

void desenhar_trilho(lgfx::LovyanGFX& d, const area_t& a)
{
  // It was SIS_FUNDO (the system palette): the columns are
  // FRAME, not content, and had to recede as the bar recedes. Swapped
  // for Mesh's own palette (fundo_msg) - so the rail, the conversations list and the
  // messages panel read as ONE screen, not two palettes stuck together.
  tab5_tela_limpar(a.x, a.y, TRILHO_L, a.altura, fundo_msg(d));
  // WHOLE RECTANGLE, and not just the right-hand line. With only one border, the column
  // seemed to bleed out of the area at top and bottom; closed, it becomes one
  // piece.
  //
  // SIS_BORDA and not white: pure white had the highest possible contrast
  // against the dark background and the frame shouted louder than its content. This
  // grey shows without competing.
  d.drawRect(a.x, a.y, TRILHO_L, a.altura, SIS_BORDA);
  const int nao_lidas = tab5_mesh_nao_lidas();
  const int diretas   = tab5_mesh_nao_lidas_diretas();
  const uint16_t FUNDO = fundo_msg(d);

  for (int i = 0; i < PAG_TOTAL; ++i) {
    const int y = a.y + 16 + i * (ITEM_A + 12);
    const bool sel = ((int)s_pag == i);
    // Background of the UNselected icon: it was pure black (found in
    // "the images... with the old background") and clashed with the rail
    // already recoloured around it. Now it follows the same palette.
    caixa_glifo(d, a.x + 12, y, TRILHO_L - 24, ITEM_A, (glifo_t)i,
                sel ? TEMA_DESTAQUE : FUNDO, sel ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO);

    // A DOT, NOT A NUMBER, and the difference is on purpose.
    //
    // The number answered "how many" - a question the conversations column already
    // answers better, conversation by conversation. Here, in a 66px icon, it only
    // competed for space with the letter and also needed to clear its own background.
    //
    // What the rail needs to say is "there is something new there", and for that a dot
    // is enough - the eye catches it at a glance, without reading. Red when there is a
    // DIRECT message: addressed to you is not the same as channel traffic.
    if (i == PAG_MSGS && nao_lidas > 0) {
      const int px = a.x + TRILHO_L - 24;
      const int py = y + 12;
      d.fillCircle(px, py, 7, diretas > 0 ? TEMA_PERIGO : TEMA_DESTAQUE);
      d.drawCircle(px, py, 7, TEMA_FUNDO);      // outline in the background colour: separates the dot from the icon
    }
  }
  // Link state at the foot of the rail: bright green, muted red. It is the answer
  // to "is it connected?" without leaving the page you are on.
  const bool ok = tab5_mesh_no_ar();

  // LOGO BELOW, DOT ABOVE. Together they answer "which network is this" and "is it
  // alive" without taking up any line of text - but the logo is the app's identity
  // and closes the foot of the rail better; the dot is state, and state goes up.
  const int ml    = TRILHO_L - 36;
  const int marca_y = a.y + a.altura - 18 - ml;   // same foot margin as before
  const int led_cy  = marca_y - 22 - 12;          // same 22px spacing

  d.fillCircle(a.x + TRILHO_L / 2, led_cy, 12, ok ? TEMA_SUCESSO : TEMA_PERIGO);
  marca_mesh(d, a.x + (TRILHO_L - ml) / 2, marca_y, ml);
}

// THE CONVERSATIONS COLUMN IS A kernel/lista. Fixed-height item;
// the list scrolls, clips, draws the exposed strip, the bar and responds to touch.
// The column is only repainted in full on a full pass (the only time when
// names, selection and unread counts change); otherwise, only the scrolling.
static const int CONV_PASSO = ITEM_A + 8;

int conv_altura(int, void*) { return CONV_PASSO; }

// What each item shows now (name and unread), so the column notices by
// itself what changed - before, the counter of a conversation that was not the open one
// only appeared on the next full pass.
typedef struct { char nome[MESH_NOME_CONVERSA]; char sub[MESH_NOME_LONGO]; int nao_lidas; } conv_visto_t;
static conv_visto_t s_conv_visto[MESH_MAX_CONVERSAS];

void conv_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int largura, void*)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(i, &c)) { return; }
  if (i < MESH_MAX_CONVERSAS) {
    snprintf(s_conv_visto[i].nome, sizeof(s_conv_visto[i].nome), "%s", c.nome);
    snprintf(s_conv_visto[i].sub, sizeof(s_conv_visto[i].sub), "%s", c.sub);
    s_conv_visto[i].nao_lidas = c.nao_lidas;
  }
  const uint16_t FUNDO = fundo_msg(d);
  // The item starts with the 8px spacing: the unread counter rises 6px above
  // the icon and needs to be INSIDE the item itself - the list only repaints the
  // item that crosses the exposed strip, and a piece outside it would have no owner.
  y += 8;
  const bool sel = (i == mesh_s_conv);

  // THE CONVERSATION TYPE, and not the name's initial. The initial at size 4 was the
  // enlarged built-in font, jagged, and said less than it seemed: the whole
  // name is written alongside, so the initial repeated information. What the
  // column did not say was whether it is a CHANNEL (everyone hears) or someone
  // specific - and that is what changes how you write.
  // Colour of the TYPE icon (channel/direct) kept saturated on purpose - it is a
  // functional indicator (telling the two conversation types apart at a glance),
  // not "the bubble colour"; I left it out of the recolouring for
  // lack of a clear reference in what was sent (the screenshot does not
  // show this screen). The per-sender coloured avatars (cor_do_no), which
  // ARE equivalent to what the screenshot shows, already follow the new palette.
  caixa_glifo(d, x + 10, y, ITEM_A, ITEM_A, c.canal ? GL_DIFUSAO : GL_DIRETO,
              sel ? TEMA_DESTAQUE : (c.canal ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA),
              sel ? TEMA_SOBRE_DESTAQUE : TEMA_TEXTO);

  // NAME WITH A WIDTH THAT FITS, actually measured: the text starts at
  // (10 + ITEM_A + 8) = 90px inside the column, the divider is at CONV_L-1 =
  // 199. Before this it was "12px per character" (Font0's fixed width);
  // Font4 is proportional, so the cut now measures with d.textWidth() and
  // removes one letter at a time until it fits - the same technique used in the rest
  // of the system since the font change. The padding with
  // spaces on the right went: the list clears the item strip before
  // drawing it, so no remains of an old name are left to erase.
  const int nome_x = x + 10 + ITEM_A + 8;
  const int nome_max_l = largura - (nome_x - x);
  // +60%. ACCEPTED
  // RISK: nome_max_l did not change (the column is still 200px), so a
  // name that used to fit whole may now truncate one more letter -
  // the cut by d.textWidth() just below already guards against overflowing the
  // column, it only shows fewer characters if needed.
  tab5_texto(d, 3.84f);   // +60%
  d.setTextDatum(top_left);
  d.setTextColor(sel ? TEMA_DESTAQUE : TEMA_TEXTO, FUNDO);
  char nome[24];
  snprintf(nome, sizeof(nome), "%s", c.nome);
  tab5_cortar_ao_caber(d, nome, nome_max_l);   // per character: the name may have accents
  d.drawString(nome, nome_x, y + 12);

  // SECOND LINE: the long name of whoever the direct conversation is with (or "channel N"). It is what
  // answers "who is it" when the short name is an acronym - without resorting to the id. In
  // the open conversation it stops before the delete button, which lives at this height.
  if (c.sub[0]) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TEMA_TEXTO_3, FUNDO);
    const int sub_max_l = sel ? (apagar_dx() - (nome_x - x) - 6) : nome_max_l;
    char sub[MESH_NOME_LONGO];
    snprintf(sub, sizeof(sub), "%s", c.sub);
    tab5_cortar_ao_caber(d, sub, sub_max_l);   // per character: the long name may have accents
    d.drawString(sub, nome_x, y + 46);
  }

  // Counter over the CORNER of the icon, as in a messaging app. Previously
  // it sat to the right of the name and competed with it for space - and the name always
  // won, because it is drawn later.
  if (c.nao_lidas > 0) {
    char nl[8]; snprintf(nl, sizeof(nl), "%d", c.nao_lidas);
    caixa(d, x + 10 + ITEM_A - 24, y - 6, 30, 26, nl, TEMA_PERIGO, TEMA_TEXTO, 2);
  }

  // DELETE: only on the OPEN conversation - channels TOO.
  //
  // Only on the open one so as not to fill the column with red buttons - you delete the
  // one you are looking at, which is the one you just realised you did not want. Previously
  // channels did not get this button (a deleted channel came back by itself) - that stopped being true when channels joined the
  // same deleted flag that already existed for nodes (see the big notice in
  // tab5_mesh.cpp, next to it). Reopening later: via the "CANAIS
  // DISPONIVEIS" (available channels) section on the Nodes page, or just by waiting for a new message.
  if (sel) {
    caixa(d, x + apagar_dx(), y + APAGAR_DY, APAGAR_L, APAGAR_A,
          "x", TEMA_PERIGO, TEMA_SOBRE_DESTAQUE, 2);
  }
}

// The column's window: INSIDE the frame (1px) and above the keyboard, which
// covers the full width when open. What the keyboard covers neither scrolls nor
// responds - the list only knows what it drew.
// The column width comes from here and is stored in s_col_l: the item (the delete
// "x") and touch read the same number the frame used.
static area_t janela_conversas(const area_t& a)
{
  s_col_l = em_pe(a) ? a.largura - TRILHO_L : CONV_L;
  const int alt = a.altura - (conversa_teclado_aberto(&s_c) ? tecvirt_altura(a) : 0);
  return area_t{ a.x + TRILHO_L + 1, a.y + 1, s_col_l - 2, alt - 2 };
}

void desenhar_conversas(lgfx::LovyanGFX& d, const area_t& a)
{
  const uint16_t FUNDO = fundo_msg(d);
  const area_t j = janela_conversas(a);
  const int n = tab5_mesh_num_conversas();

  if (TUDO || n != s_l_conv.n) {
    lista_itens_mudaram(&s_l_conv, n);
  } else {
    for (int i = 0; i < n && i < MESH_MAX_CONVERSAS; ++i) {
      mesh_conversa_t c;
      if (!tab5_mesh_conversa(i, &c)) { continue; }
      if (c.nao_lidas != s_conv_visto[i].nao_lidas || strcmp(c.nome, s_conv_visto[i].nome) != 0
       || strcmp(c.sub, s_conv_visto[i].sub) != 0) {
        lista_item_mudou(&s_l_conv, i);
      }
    }
  }
  const bool pintou = lista_desenhar(&s_l_conv, d, j, FUNDO, TUDO);

  // Empty list repainted (full pass, or the last conversation deleted):
  // the notice goes over the background the list has just painted.
  if (n == 0 && pintou) {
    tab5_texto(d, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, FUNDO);
    d.drawString(tr("sem conversas"), j.x + 11, j.y + 19);
  }
  // Frame OUTSIDE the list's window: nothing the list paints reaches it, and
  // drawing it on the full pass is enough. In the system frame grey.
  if (TUDO) { d.drawRect(a.x + TRILHO_L, a.y, s_col_l, j.altura + 2, SIS_BORDA); }
}

// "HOW LONG AGO", short.
//
// It is the question the phone app answers and our list did not:
// high SNR on a node nobody has heard for two days means nothing. Short format
// on purpose - the column is narrow and what matters is the order of
// magnitude, not the exact second.
static void idade_texto(int32_t s, char* saida, size_t tam)
{
  if      (s < 0)     { snprintf(saida, tam, "?");     }
  else if (s < 90)    { snprintf(saida, tam, tr("agora")); }
  else if (s < 3600)  { snprintf(saida, tam, "%dm", (int)(s / 60)); }
  else if (s < 86400) { snprintf(saida, tam, "%dh%02d",
                                 (int)(s / 3600), (int)((s % 3600) / 60)); }
  else                { snprintf(saida, tam, "%dd", (int)(s / 86400)); }
}

// Green up to 15 min, yellow up to 2h, grey after. The 2h is the boundary
// Meshtastic itself uses to call a node "online" - worth copying, so the
// list here and the one on the phone tell the same story.
static uint16_t idade_cor(int32_t s)
{
  if (s < 0)    { return TEMA_TEXTO_3; }
  if (s <= 900) { return TEMA_SUCESSO; }
  if (s <= 7200){ return TEMA_ALERTA; }
  return TEMA_TEXTO_3;
}

// ---------------------------------------------------------------------------
// THE COLUMN GRID
// ---------------------------------------------------------------------------
// Header and rows are built from THIS table, and only from it. It is the only
// way for the two not to drift apart - and they had drifted apart.
//
// Previously the row was a single format with the gaps counted by hand:
//
//   "%-22.22s  %-6.6s %5.1f %3u%%   %u   "
//
// Note the spaces: 2, 1, 1, 3, 3. Each column with a different gap, and the
// header was ANOTHER string, with its own count - so "snr" fell one
// character off the values and "visto" (seen) touched "saltos" (hops). Changing one of the
// two without changing the other misaligned everything again.
//
// Single rule now: a 2-character gap between columns, and the title aligns the
// same way as the value - numbers on the right, text on the left.
enum { C_NOME = 0, C_CURTO, C_ID, C_HW, C_SNR, C_BAT, C_SALTOS, C_VISTO, C_TOTAL };

// IDENTITY AND LABEL, SIDE BY SIDE.
//
// "nome" (name) and "curto" (short) are LABELS: whoever renames the node on the phone changes both.
// They do not say which device you are talking to, and that was all the table
// showed. The two new columns do:
//
//   id  - "!a1b2c3d4", derived from the MAC, stays with the radio for ever. It is the
//         identifier Meshtastic uses everywhere: log, map, phone.
//   hw  - which board it is (T-Beam, Heltec V3...). Explains by itself why one node
//         has a battery and another does not, why one moves and another does not.
//
// The screen is 1280px in landscape. The last column ends at character 85
// (with the 2-character gaps fixed), which gives 1126px with the margin and the
// rail's 90px x0 - it fits with ~150px to spare for the scroll
// indicator and still leaves room for a future column.
// THE GAPS WERE 1 CHARACTER, NOT 2 - the comment above described the
// right rule, but the numbers below did not follow it (name ends at 20,
// "curto" started at 21 - only a 1 gap, not 2). In a fixed-width table,
// a right-aligned field 12px from the next left-aligned field is
// exactly what reads as "crooked": one column's value almost touches the
// neighbour's text, without the visual spacing that separates "this is a column" from
// "this is the next one". Redone with the 2 characters the rule always asked for.
static const coluna_t COLUNA[C_TOTAL] = {
  {  0, 20, false, N_("nome")   },
  { 22,  5, false, N_("curto")  },
  { 29, 10, false, "id"     },
  { 41, 15, false, N_("modelo") },
  { 58,  5, true,  N_("snr")    },
  { 65,  4, true,  N_("bat")    },
  { 71,  6, true,  N_("saltos") },
  { 79,  6, false, N_("visto")  },
};

// THE SAME GRID IN PORTRAIT. In 720 px, minus the rail, ~48 characters fit -
// not the 85 above. The columns that stay are those that answer "can I talk to it
// now?": name, signal, battery, hops and how long ago it was heard. Out go
// short name, id and model (width 0: campo() writes nothing), which remain one
// tap away, on the node card. Same 2-character gap rule.
static const coluna_t COLUNA_PE[C_TOTAL] = {
  {  0, 16, false, N_("nome")   },
  {  0,  0, false, N_("curto")  },
  {  0,  0, false, "id"     },
  {  0,  0, false, N_("modelo") },
  { 18,  5, true,  N_("snr")    },
  { 25,  4, true,  N_("bat")    },
  { 31,  6, true,  N_("saltos") },
  { 39,  6, false, N_("visto")  },
};

// The grid in use: chosen by desenhar_nos() on each pass, from the area.
static const coluna_t* s_grade = COLUNA;

// 6x8 font at size 2 = 12px per character.
static const int NOS_CH = 12;
// The main row goes up to the end of "saltos"; "visto" is drawn separately,
// because it has its own colour. NOS_LINHA_CH is the LARGEST case (landscape) - the size
// of the buffers; the current length comes from the grid.
#define NOS_LINHA_CH  77        // COLUNA[C_SALTOS].col + larg
static int linha_ch(void) { return s_grade[C_SALTOS].col + s_grade[C_SALTOS].larg; }
// Left margin of the table: the star (favourite) column lives in it, and with
// 16 px the star touched the edge of the area.
static const int NOS_MARGEM  = 34;
static const int NOS_ESTRELA_X = 16;              // centre of the star, inside the margin
static int visto_x(void) { return NOS_MARGEM + s_grade[C_VISTO].col * NOS_CH; }

// THE CHANNELS AT THE TOP OF THE NODES PAGE - all the node's active ones, as buttons.
//
// Previously it was a list of "available channels": only those WITHOUT a conversation in the column,
// and only those the radio configuration had marked as active - and channel 0
// never got marked (see the channel reader in tab5_mesh.cpp). A
// LongFast deleted from the column had nowhere to be found. Now every active channel
// is always here; one not in the column appears dimmed, with "+".
//
// The geometry is STORED when drawing and READ on touch (s_chips): the two never
// disagree, even with names of different widths.
chip_t s_chips[MESH_MAX_CANAIS + 1];
int    s_n_chips = 0;
static int    s_chips_altura = 0;          // height of the channels strip, from the top
static char   s_chips_assin[(MESH_MAX_CANAIS + 1) * (MESH_NOME_CONVERSA + 4) + 48];

static const int CHIP_A = 36;

// THE SEARCH BOX, at the top of the page (where the network choice used to be). The
// table shows only the nodes matching the text, on every letter - the filter belongs to
// the service (tab5_mesh_no_busca). Tapping the box opens the virtual keyboard; the
// physical one types straight into it, no tap needed. The "x" at the end clears.
//
// ASCII only: the table is Font0, and the service's search already ignores accents ("jose"
// finds "José"), so there is nothing to type outside it.
char      s_busca[MESH_BUSCA_MAX] = "";
static bool      s_busca_mudou = false;    // the table goes back to the top and is rebuilt
tecvirt_t s_kb_busca;
chip_t    s_chip_busca;             // the whole box (local coordinates)
static char      s_busca_assin[MESH_BUSCA_MAX + 24];

// NODE CHOSEN IN THE TABLE: the page shows its card instead of the table.
// By NUMBER, not by index - the table reorders itself by "visto" (seen).
uint32_t s_no_sel = 0;

// Writes a field in its column inside 'linha', which arrives already full of
// spaces. memcpy and not snprintf: a NUL in the middle would cut the rest of the line.
// The strnlen limited to the width is also what stops a long name invading
// the next column.
void campo(char* linha, const coluna_t& c, const char* txt)
{
  const int n = (int)strnlen(txt, (size_t)c.larg);
  memcpy(linha + c.col + (c.direita ? c.larg - n : 0), txt, (size_t)n);
}

static void espacos(char* linha, size_t tam)
{
  memset(linha, ' ', tam - 1);
  linha[tam - 1] = 0;
}

// Replaces each multi-byte UTF-8 sequence (accent, emoji, anything
// outside pure ASCII) with A SINGLE '?'. It exists because campo()/montar_linha()
// build the row as a FIXED-WIDTH GRID PER BYTE (1 byte = 1
// screen column) - a name with an emoji (4 UTF-8 bytes drawing ONE single icon)
// or just an accent (2 bytes for 1 letter, "Jose" -> "José") throws the count off:
// the "extra" bytes push the following fields OF THAT ROW to the
// right, without pushing the others - and that was exactly the misalignment
// seen in the node table.
//
// A node name comes from ANOTHER radio, outside our control, so the defence sits
// here, at the table's entry - there is no point asking whoever sends the message
// not to use emoji. In CONVERSATIONS, where the text flows freely (no column
// grid), the emoji still really appears - see tab5_emoji_preparar().
static void sanitizar_ascii(const char* in, char* out, size_t out_tam)
{
  if (out_tam == 0) { return; }
  size_t o = 0;
  const unsigned char* p = (const unsigned char*)in;
  while (*p && o + 1 < out_tam) {
    if (*p < 0x80) { out[o++] = (char)*p; ++p; continue; }
    // Lead byte of a multi-byte sequence: skip ALL of it at once, otherwise
    // each continuation byte would become its own '?' - a single emoji would become
    // 4 question marks instead of 1.
    const int n = ((*p & 0xE0) == 0xC0) ? 2
                : ((*p & 0xF0) == 0xE0) ? 3
                : ((*p & 0xF8) == 0xF0) ? 4
                : 1;    // stray continuation byte (invalid UTF-8): just 1
    out[o++] = '?';
    p += n;
  }
  out[o] = 0;
}

static void montar_linha(char* linha, size_t tam, const mesh_no_t& no)
{
  char v[24];
  char nome_limpo[MESH_NOME_LONGO];
  char curto_limpo[MESH_NOME_CURTO];
  espacos(linha, tam);

  // LONG: same logic as the short name just below - prefers the LIVE entry
  // (NodeInfo from this session); without it, falls back to the persistent cache
  // (tab5_mesh_nome_longo_cache(), nos.txt). This column only
  // looked at no.longo - the same class of bug the short name had
  // (see its notice just below), just never fixed here: most
  // nodes are born from telemetry/position/text, not from NodeInfo (the ONLY source
  // of the long name), so the table showed "(sem nome)" (no name) almost always.
  const char* longo_fonte = no.longo;
  char longo_cache[MESH_NOME_LONGO];
  if (!no.longo[0] && tab5_mesh_nome_longo_cache(no.num, longo_cache, sizeof(longo_cache))) {
    longo_fonte = longo_cache;
  }
  sanitizar_ascii(longo_fonte[0] ? longo_fonte : tr("(sem nome)"), nome_limpo, sizeof(nome_limpo));

  // SHORT: prefers what THIS node reported live (NodeInfo from this session).
  // Without that, falls back to the persistent cache (nos.txt) - the same cache the
  // conversation already consults via tab5_mesh_nome_exibicao(), but here WITHOUT
  // its hex fallback (see tab5_mesh_nome_cache() and the full notice in the
  // .hpp): the ID column just below already shows the hex when the name is missing, so
  // repeating it here in the short name column would be redundant.
  //
  // FINDING (most rows came out with neither long nor short name): this table only looked at the LIVE entry (no.curto). Most
  // entries are born from achar_no() being called by a
  // TELEMETRY/POSITION/TEXT packet (see ler_packet()/ler_data() in the service), not
  // by a NodeInfo - and NodeInfo (the ONLY type carrying a name) is much rarer
  // on the mesh than the others. A node you had chatted with before (name
  // stored in the cache long ago) appeared blank again, just because the
  // live entry of NOW had not yet received a new NodeInfo - the same
  // problem the conversation had already solved, except the table never got
  // the same fix.
  const char* curto_fonte = no.curto;
  char curto_cache[MESH_NOME_CURTO];
  if (!no.curto[0] && tab5_mesh_nome_cache(no.num, curto_cache, sizeof(curto_cache))) {
    curto_fonte = curto_cache;
  }
  sanitizar_ascii(curto_fonte, curto_limpo, sizeof(curto_limpo));

  campo(linha, s_grade[C_NOME],  nome_limpo);
  campo(linha, s_grade[C_CURTO], curto_limpo);

  // The id comes from the radio itself as "!a1b2c3d4". When it has not come yet (its
  // node_info has not arrived), we build it from the number - it is the SAME calculation
  // Meshtastic does, so the value matches.
  if (no.id[0]) { campo(linha, s_grade[C_ID], no.id); }
  else {
    snprintf(v, sizeof(v), "!%08lx", (unsigned long)no.num);
    campo(linha, s_grade[C_ID], v);
  }

  // Known model by name; unknown by number, which can be looked up.
  // Inventing a name for a value the table does not have would be worse than the number.
  const char* hw = mesh_hw_nome(no.hw);
  if (hw) { campo(linha, s_grade[C_HW], hw); }
  else    { snprintf(v, sizeof(v), tr("modelo %u"), (unsigned)no.hw);
            campo(linha, s_grade[C_HW], v); }

  // THE DECIMAL POINT MUST ALWAYS FALL IN THE SAME COLUMN - that was the root of the
  // "crooked" look observed.
  //
  // campo() already aligns to the RIGHT correctly (the string always ends flush with
  // the field's right edge); the problem was that "%.1f" had no fixed width.
  // "-18.5" has 5 characters, "9.0" has 3 - both end in the same place
  // (right-aligned, the right way), but as they start in DIFFERENT
  // columns, one row's decimal point does not sit under the next row's
  // decimal point. In a column of numbers, it is the decimal point -
  // not the edge - that the eye uses to tell everything is aligned.
  //
  // "%+05.1f" fixes the sign, two integer digits (zero-padded when
  // short) and one decimal place - always 5 characters, always the same format:
  // "-18.5", "+09.0", "-05.5", "+00.0". The decimal point always falls in the same
  // position, for any reading between -99.9 and +99.9 (Meshtastic's real SNR
  // range never gets close to that).
  snprintf(v, sizeof(v), "%+05.1f", (double)no.snr);
  campo(linha, s_grade[C_SNR], (no.snr <= -128.0f) ? "-" : v);

  // 101 is Meshtastic's code for "on mains power", not a 101% battery.
  // It used to show "101%". And 0 means its telemetry has not arrived yet.
  //
  // ZERO-PADDED, for the same reason as the SNR above: "5%" and "100%" have
  // different widths, and without a fixed width one row's tens digit
  // does not fall under the next row's tens digit. "%03u%%" fixes
  // 4 characters always - "005%", "050%", "100%" - and the '%' and the digits stay
  // in the same column on every row that really has a battery.
  if      (no.bateria == 0)   { snprintf(v, sizeof(v), "-");  }
  else if (no.bateria > 100)  { snprintf(v, sizeof(v), "AC"); }
  else                        { snprintf(v, sizeof(v), "%03u%%", (unsigned)no.bateria); }
  campo(linha, s_grade[C_BAT], v);

  // "mqtt" in place of the number: a node arriving via the internet has no radio
  // hop at all, and showing "0" there would say it is a direct neighbour - the opposite
  // of the truth. It is the information that decides whether you reach that node with the
  // internet down.
  //
  // "-" FOR UNKNOWN (nodes appeared with no name and no hops): a node only created by achar_no() on seeing a packet
  // (no NodeInfo yet) had hops=0 from the memset, indistinguishable from a
  // real direct neighbour. Now the sentinel (MESH_SALTOS_DESCONHECIDO)
  // is explicit here, the way SNR (-128) already was.
  if (no.via_mqtt) { snprintf(v, sizeof(v), "mqtt"); }
  else if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(v, sizeof(v), "-"); }
  else             { snprintf(v, sizeof(v), "%u", (unsigned)no.saltos); }
  campo(linha, s_grade[C_SALTOS], v);
  // In portrait the row is shorter: it ends here, otherwise the spaces of the rest
  // would run past the edge of the screen.
  if ((int)tam > linha_ch()) { linha[linha_ch()] = 0; }
}

// What is already written on each row. It exists so as NOT to rewrite 32 lines of
// text per second: with the 1Hz period and the age ticking, almost every
// pass changed a handful of rows and redrew them all. In landscape this
// is expensive - M5GFX transposes the coordinates and each line of text becomes
// a column write in the panel memory.
EXT_RAM_BSS_ATTR static char s_nos_ant[MESH_MAX_NOS][NOS_LINHA_CH + 1];
EXT_RAM_BSS_ATTR static char s_nos_visto_ant[MESH_MAX_NOS][8];
EXT_RAM_BSS_ATTR static bool s_nos_fav_ant[MESH_MAX_NOS];   // the star currently drawn

// 44 and not 32: with scrolling, height is not short, and rows packed together tired the
// eye. The text (16 px) goes centred and a dark line separates one from the next.
static const int NOS_LINHA_A = 44;
static const int NOS_TEXTO_DY = (NOS_LINHA_A - 16) / 2;

int nos_altura(int, void*) { return NOS_LINHA_A; }

// A table row. Stores what it wrote (s_nos_ant/s_nos_visto_ant): that is the
// reference desenhar_nos() uses to decide, on the next pass, whether the row
// changed. Font0 is already active (see the exception in desenhar_nos).
// Five-pointed STAR, in primitives: no system font has the
// character, and drawn it comes out the same at any size. Filled = favourite;
// empty (outline only) = the button to mark it.
static void estrela(lgfx::LovyanGFX& d, int cx, int cy, int r, uint16_t cor, bool cheia)
{
  float px[10], py[10];
  for (int k = 0; k < 10; ++k) {
    const float ang = (float)M_PI * (-0.5f + k * 0.2f);
    const float rr = (k & 1) ? r * 0.42f : (float)r;
    px[k] = cx + rr * cosf(ang);
    py[k] = cy + rr * sinf(ang);
  }
  for (int k = 0; k < 10; ++k) {
    const int j = (k + 1) % 10;
    if (cheia) { d.fillTriangle(cx, cy, (int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
    else       { d.drawLine((int)px[k], (int)py[k], (int)px[j], (int)py[j], cor); }
  }
}

void nos_desenhar(lgfx::LovyanGFX& d, int i, int x, int y, int, void*)
{
  mesh_no_t no;
  if (!tab5_mesh_no_busca(s_busca, i, &no) || i >= MESH_MAX_NOS) { return; }

  // WHITE = our node. YELLOW = heard over the air (has SNR): pinned at the top,
  // the ones you can reach with the internet down. Via the internet, the blue of
  // MQTT in the conversations; without SNR, grey.
  const bool meu = (no.num == tab5_mesh_meu_numero());
  const uint16_t cor = meu ? TEMA_TEXTO
                     : (no.fixado || tab5_mesh_no_pelo_ar(&no)) ? TEMA_ALERTA
                     : no.via_mqtt ? TEMA_DESTAQUE : TEMA_TEXTO_3;
  d.setTextSize(2);
  d.setTextDatum(top_left);
  montar_linha(s_nos_ant[i], sizeof(s_nos_ant[i]), no);
  d.setTextColor(cor, TEMA_FUNDO);
  d.drawString(s_nos_ant[i], x + NOS_MARGEM, y + NOS_TEXTO_DY);
  // Favourite: star in the margin, before the name (the 16 px that were already spare).
  if (no.favorito) { estrela(d, x + NOS_ESTRELA_X, y + NOS_LINHA_A / 2, 8, TEMA_ALERTA, true); }
  // Faint, low-contrast divider: separates without competing with the text.
  d.drawFastHLine(x + 6, y + NOS_LINHA_A - 1, visto_x() + 6 * NOS_CH - 6, TEMA_LINHA);

  // Separate column because it has its own colour: the age answers "can I talk
  // to it NOW?", which is a different question from the signal quality.
  char curto[8];
  idade_texto(no.idade_s, curto, sizeof(curto));
  snprintf(s_nos_visto_ant[i], sizeof(s_nos_visto_ant[i]), "%-6.6s", curto);
  s_nos_fav_ant[i] = no.favorito;
  d.setTextColor(idade_cor(no.idade_s), TEMA_FUNDO);
  d.drawString(s_nos_visto_ant[i], x + visto_x(), y + NOS_TEXTO_DY);
}

// Signature of the offered channels: changes when a channel turns on, off, is
// renamed or enters/leaves the column - and then the strip is repainted in full.
static void canais_assinatura(char* out, size_t tam)
{
  out[0] = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    const size_t k = strlen(out);
    snprintf(out + k, tam - k, "%u%c%s|", (unsigned)idx, vis ? '+' : '-', nome);
  }
}

// Draws the channels strip and stores the geometry. Returns the height used.
//
// IN THE TABLE'S FONT (Font0 at size 2, 12 px per character), and not in
// tab5_botao's smooth one: the strip and the table are the same page, and two fonts
// stuck together read as two screens. Each button carries the channel number ("ch0") -
// it is what Meshtastic uses to say which is the primary, and what distinguishes
// two channels with similar names.
// One strip button, in the table's font. Returns the width.
static int chip(lgfx::LovyanGFX& d, int x, int y, const char* rot, uint16_t fundo, uint16_t frente)
{
  const int l = (int)strlen(rot) * NOS_CH + 28;
  d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
  d.setTextColor(frente, fundo);
  d.drawString(rot, x + 14, y + CHIP_A / 2);
  return l;
}

static int desenhar_canais(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  const int fim_x = a.x + a.largura - 16;
  const int ini_x = x0 + NOS_MARGEM + 8 * NOS_CH;   // after the "canais" label
  int x = ini_x, y = a.y + 8;
  d.setTextDatum(middle_left);

  // SEARCH: the label and the box, in the remaining width. The box's content belongs to
  // desenhar_busca, which repaints only it on each letter.
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("buscar"), x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_chip_busca = chip_t{ x - a.x, y - a.y, fim_x - x, CHIP_A, 0 };
  s_busca_assin[0] = 0;   // the box was (re)placed: the content comes next
  x = ini_x;
  y += CHIP_A + 8;
  d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
  d.drawString(tr("canais"), x0 + NOS_MARGEM, y + CHIP_A / 2);
  s_n_chips = 0;
  const int n = tab5_mesh_num_canais();
  for (int i = 0; i < n && s_n_chips < MESH_MAX_CANAIS + 1; ++i) {
    uint8_t idx; char nome[MESH_NOME_CONVERSA]; bool vis = false;
    if (!tab5_mesh_canal(i, &idx, nome, sizeof(nome), &vis)) { continue; }
    char limpo[MESH_NOME_CONVERSA];
    sanitizar_ascii(nome, limpo, sizeof(limpo));   // Font0: ASCII only (see sanitizar_ascii)
    char rot[MESH_NOME_CONVERSA + 12];
    snprintf(rot, sizeof(rot), "%sch%u %s", vis ? "" : "+ ", (unsigned)idx, limpo);
    const int l = (int)strlen(rot) * NOS_CH + 28;
    if (x + l > fim_x && x > ini_x) { x = ini_x; y += CHIP_A + 8; }
    const uint16_t fundo = vis ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(x, y, l, CHIP_A, 10, fundo);
    d.setTextColor(vis ? TEMA_TEXTO : TEMA_TEXTO_2, fundo);
    d.drawString(rot, x + 14, y + CHIP_A / 2);
    s_chips[s_n_chips++] = chip_t{ x - a.x, y - a.y, l, CHIP_A, idx };
    x += l + 10;
  }
  d.setTextDatum(top_left);
  return (y - a.y) + CHIP_A + 8;
}

// THE SEARCH BOX CONTENT: the text (with the cursor, if the keyboard is
// open) or the prompt, how many nodes matched and the "x". Paints only when any of
// that changes - on each letter, not every second.
static void desenhar_busca(lgfx::LovyanGFX& d, const area_t& a)
{
  const chip_t& k = s_chip_busca;
  if (!k.l) { return; }
  const bool kb = tecvirt_aberto(&s_kb_busca);
  const int n = tab5_mesh_num_nos_busca(s_busca);
  const int total = tab5_mesh_num_nos();
  char assin[sizeof(s_busca_assin)];
  snprintf(assin, sizeof(assin), "%d|%d|%d|%s", kb, n, total, s_busca);
  if (strcmp(assin, s_busca_assin) == 0) { return; }
  snprintf(s_busca_assin, sizeof(s_busca_assin), "%s", assin);

  const int x = a.x + k.x, y = a.y + k.y;
  const uint16_t fundo = TEMA_SUPERFICIE_ALTA;
  d.fillRoundRect(x, y, k.l, k.a, 10, fundo);
  d.drawRoundRect(x, y, k.l, k.a, 10, kb ? TEMA_DESTAQUE : TEMA_LINHA);
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  d.setTextDatum(middle_left);
  const int cy = y + k.a / 2;

  // The count on the right, before the "x": "12 de 87" (12 of 87) with a search, "87 nós" (87 nodes) without.
  char cont[24];
  if (s_busca[0]) { snprintf(cont, sizeof(cont), tr("%d de %d"), n, total); }
  else            { snprintf(cont, sizeof(cont), tr("%d nos"), total); }
  const int cont_l = (int)strlen(cont) * NOS_CH;
  const int fim_txt = x + k.l - BUSCA_LIMPAR_L - cont_l - 16;
  d.setTextColor(s_busca[0] && n == 0 ? TEMA_ALERTA : TEMA_TEXTO_3, fundo);
  d.drawString(cont, fim_txt + 8, cy);

  // The text: its end, if it does not fit (that is where you are typing).
  const int max_ch = (fim_txt - (x + 14)) / NOS_CH;
  char t[MESH_BUSCA_MAX + 2];
  if (s_busca[0] || kb) {
    snprintf(t, sizeof(t), "%s%s", s_busca, kb ? "_" : "");
    const int nt = (int)strlen(t);
    const char* ini = (max_ch > 0 && nt > max_ch) ? t + (nt - max_ch) : t;
    d.setTextColor(TEMA_TEXTO, fundo);
    d.drawString(ini, x + 14, cy);
  } else {
    snprintf(t, sizeof(t), "%s", tr("nome, curto ou !id"));
    if (max_ch > 0 && (int)strlen(t) > max_ch) { t[max_ch] = 0; }
    d.setTextColor(TEMA_TEXTO_3, fundo);
    d.drawString(t, x + 14, cy);
  }
  if (s_busca[0]) {
    d.setTextColor(TEMA_TEXTO_2, fundo);
    d.setTextDatum(middle_center);
    d.drawString("x", x + k.l - BUSCA_LIMPAR_L / 2, cy);
  }
  d.setTextDatum(top_left);
}

void busca_abrir_teclado(void)
{
  if (tecvirt_aberto(&s_kb_busca)) { return; }
  tecvirt_abrir(&s_kb_busca, TECVIRT_BUSCA);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();   // the table shrinks above the keyboard
}

void busca_fechar_teclado(void)
{
  if (!tecvirt_aberto(&s_kb_busca)) { return; }
  tecvirt_fechar(&s_kb_busca);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

void busca_limpar(void)
{
  if (!s_busca[0]) { return; }
  s_busca[0] = 0;
  s_busca_mudou = true;
  sistema_pedir_redesenho();
}

// A key (virtual or physical, already as a tecvirt action) in the search.
void busca_tecla(int ac)
{
  if (ac == TECVIRT_ENVIAR || ac == TECVIRT_FECHAR) { busca_fechar_teclado(); return; }
  char antes[sizeof(s_busca)];
  snprintf(antes, sizeof(antes), "%s", s_busca);
  tecvirt_editar(s_busca, sizeof(s_busca), ac);
  if (strcmp(antes, s_busca) != 0) { s_busca_mudou = true; sistema_pedir_redesenho(); }
}

static bool no_por_num(uint32_t num, mesh_no_t* out);

static bool no_por_num(uint32_t num, mesh_no_t* out)
{
  const int n = tab5_mesh_num_nos();
  for (int i = 0; i < n; ++i) {
    if (tab5_mesh_no(i, out) && out->num == num) { return true; }
  }
  return false;
}

// NODE CARD - everything known about it, as the web client shows. It fits
// what the table has no width to show (MAC, voltage, position) without
// squeezing yet another column into it.
static const int DET_LIN   = 50;
static const int DET_BTN_L = 300;
// IN PORTRAIT the fixed measurements run past the edge: two 300 buttons side by side do not
// fit in the ~630 px left after the rail. Drawing and touch go through the same
// functions, so they agree in both orientations. 'larg' = from column x0 to the
// end of the area.
int det_btn_l(const area_t& a)
{
  const int larg = a.largura - TRILHO_L;
  const int cabe = (larg - 3 * 24) / 2;
  return cabe < DET_BTN_L ? cabe : DET_BTN_L;
}
int det_fav_l(const area_t& a) { return em_pe(a) ? 210 : DET_FAV_L; }
static int det_val_dx(const area_t& a) { return em_pe(a) ? 170 : 220; }   // values column
static char s_det_assin[512];

static void desenhar_no_detalhe(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  mesh_no_t no;
  const bool achou = no_por_num(s_no_sel, &no);
  const bool meu = (s_no_sel == tab5_mesh_meu_numero());

  enum { R_ID = 0, R_MAC, R_HW, R_SINAL, R_SALTOS, R_BAT, R_VISTO, R_POS, R_TOTAL };
  static const char* const ROT[R_TOTAL] = { N_("id"), N_("MAC"), N_("modelo"), N_("sinal"), N_("saltos"), N_("bateria"), N_("visto"), N_("posição") };
  char val[R_TOTAL][64];
  char titulo[MESH_NOME_LONGO + 16], curto[MESH_NOME_CURTO * 2] = "";
  for (int k = 0; k < R_TOTAL; ++k) { snprintf(val[k], sizeof(val[k]), "-"); }

  if (!achou) {
    tab5_mesh_nome_exibicao(s_no_sel, titulo, sizeof(titulo));
    snprintf(val[R_ID], sizeof(val[R_ID]), tr("!%08lx  (saiu da tabela ao vivo)"), (unsigned long)s_no_sel);
  } else {
    // Title: the long name; without it, the display rule (short name, or the id).
    char longo[MESH_NOME_LONGO] = "";
    if (no.longo[0]) { snprintf(longo, sizeof(longo), "%s", no.longo); }
    else             { tab5_mesh_nome_longo_cache(no.num, longo, sizeof(longo)); }
    if (longo[0]) { snprintf(titulo, sizeof(titulo), "%s", longo); }
    else          { tab5_mesh_nome_exibicao(no.num, titulo, sizeof(titulo)); }
    if (no.curto[0]) { snprintf(curto, sizeof(curto), "%s", no.curto); }
    else             { tab5_mesh_nome_cache(no.num, curto, sizeof(curto)); }

    snprintf(val[R_ID], sizeof(val[R_ID]), "%s   nº %lu",
             no.id[0] ? no.id : "", (unsigned long)no.num);
    if (!no.id[0]) { snprintf(val[R_ID], sizeof(val[R_ID]), "!%08lx   nº %lu", (unsigned long)no.num, (unsigned long)no.num); }

    // The MAC only when the node sent it (User.macaddr). The id is derived from its
    // last 4 bytes, but filling in the first 2 would be making it up.
    if (no.tem_mac) {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), "%02X:%02X:%02X:%02X:%02X:%02X",
               no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      snprintf(val[R_MAC], sizeof(val[R_MAC]), tr("não enviado pelo nó"));
    }

    const char* hw = mesh_hw_nome(no.hw);
    if (no.hw == 0)  { snprintf(val[R_HW], sizeof(val[R_HW]), tr("não informado")); }
    else if (hw)     { snprintf(val[R_HW], sizeof(val[R_HW]), "%s", hw); }
    else             { snprintf(val[R_HW], sizeof(val[R_HW]), tr("modelo %u"), (unsigned)no.hw); }

    if (no.snr > -128.0f) { snprintf(val[R_SINAL], sizeof(val[R_SINAL]), tr("SNR %+.1f dB%s"), (double)no.snr, no.via_mqtt ? tr("  (do gateway)") : ""); }

    if (no.via_mqtt)                                   { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), tr("pela internet (MQTT)")); }
    else if (no.saltos == 0)                           { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), tr("0 - vizinho direto")); }
    else if (no.saltos != MESH_SALTOS_DESCONHECIDO)    { snprintf(val[R_SALTOS], sizeof(val[R_SALTOS]), "%u", (unsigned)no.saltos); }

    char bat[32] = "";
    if (no.bateria > 100)    { snprintf(bat, sizeof(bat), tr("na tomada")); }
    else if (no.bateria > 0) { snprintf(bat, sizeof(bat), "%u %%", (unsigned)no.bateria); }
    if (no.tensao > 0.1f) {
      const size_t k = strlen(bat);
      snprintf(bat + k, sizeof(bat) - k, "%s%.2f V", k ? "   " : "", (double)no.tensao);
    }
    if (bat[0]) { snprintf(val[R_BAT], sizeof(val[R_BAT]), "%s", bat); }

    char idade[12];
    idade_texto(no.idade_s, idade, sizeof(idade));
    if (no.idade_s < 0)       { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("não ouvido nesta sessão")); }
    else if (no.idade_s < 90) { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("agora")); }
    else                      { snprintf(val[R_VISTO], sizeof(val[R_VISTO]), tr("há %s"), idade); }

    if (no.tem_pos) {
      const float lat = no.lat_i / 1e7f, lon = no.lon_i / 1e7f;
      float rlat, rlon; bool ref = false;
      const nmea_posicao_t fix = tab5_gps_posicao();
      if (fix.valida && fix.qualidade > 0) { rlat = (float)fix.lat; rlon = (float)fix.lon; ref = true; }
      else { ref = ajustes_casa(&rlat, &rlon); }
      if (ref && !meu) {
        float km; const char* dir;
        distancia_direcao(rlat, rlon, lat, lon, &km, &dir);
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f   %.1f km %s", (double)lat, (double)lon, (double)km, dir);
      } else {
        snprintf(val[R_POS], sizeof(val[R_POS]), "%.5f, %.5f", (double)lat, (double)lon);
      }
    }
  }

  const bool fav = achou ? no.favorito : tab5_mesh_favorito(s_no_sel);

  // Only repaints when some text changed: the page runs at 1 Hz.
  char assin[sizeof(s_det_assin)];
  snprintf(assin, sizeof(assin), "%s|%s|%d|", titulo, curto, (int)fav);
  for (int k = 0; k < R_TOTAL; ++k) {
    const size_t n = strlen(assin);
    snprintf(assin + n, sizeof(assin) - n, "%s|", val[k]);
  }
  if (!pintura_mudou_s(&s_p, s_det_assin, sizeof(s_det_assin), assin)) { return; }

  tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO);
  d.setTextDatum(top_left);
  tab5_texto(d, 4.2f);
  d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  // The title stops before the star (and the short-name chip).
  const int fav_l = det_fav_l(a);
  const int fav_x = a.x + a.largura - DET_FAV_DIR - fav_l;
  const int tit_max = fav_x - (x0 + 24) - 160;
  tab5_cortar_ao_caber(d, titulo, tit_max);
  d.drawString(titulo, x0 + 24, a.y + 18);
  if (!meu) {
    const uint16_t fundo = fav ? TEMA_SUPERFICIE_ALTA : TEMA_SUPERFICIE_ALTA;
    d.fillRoundRect(fav_x, a.y + DET_FAV_Y, fav_l, DET_FAV_A, 12, fundo);
    estrela(d, fav_x + 34, a.y + DET_FAV_Y + DET_FAV_A / 2, 16, fav ? TEMA_ALERTA : TEMA_TEXTO_2, fav);
    d.setFont(tab5_fonte_suave(true, 26));
    d.setTextDatum(middle_left);
    d.setTextColor(fav ? TEMA_ALERTA : TEMA_TEXTO_2, fundo);
    d.drawString(fav ? tr("FAVORITO") : tr("FAVORITAR"), fav_x + 62, a.y + DET_FAV_Y + DET_FAV_A / 2);
    d.setTextDatum(top_left);
    tab5_texto(d, 4.2f);
    d.setTextColor(TEMA_TEXTO, TEMA_FUNDO);
  }
  if (curto[0]) {
    const int tx = x0 + 24 + (int)d.textWidth(titulo) + 20;
    tab5_botao(d, tx, a.y + 16, 120, 44, curto, TEMA_DESTAQUE_SUAVE, TEMA_TEXTO);
  }
  if (meu) {
    tab5_texto(d, 2.4f);
    d.setTextColor(TEMA_SUCESSO, TEMA_FUNDO);
    d.drawString(tr("este é o seu nó"), x0 + 24, a.y + 62);
  }

  for (int k = 0; k < R_TOTAL; ++k) {
    const int y = a.y + 96 + k * DET_LIN;
    tab5_texto(d, 2.8f);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr(ROT[k]), x0 + 24, y + 4);
    // Long value (the position with distance, especially in portrait): shrinks until
    // it fits and, at the limit, cuts - never runs past the edge.
    const int vx = x0 + det_val_dx(a);
    texto_que_cabe(d, val[k], a.x + a.largura - 16 - vx, 3.4f, 2.6f);
    d.setTextColor(k == R_MAC && !(achou && no.tem_mac) ? TEMA_TEXTO_3 : TEMA_TEXTO, TEMA_FUNDO);
    d.drawString(val[k], vx, y);
  }

  const int by = a.y + a.altura - DET_BTN_Y_DA_BASE;
  const int bl = det_btn_l(a);
  tab5_botao(d, x0 + 24, by, bl, DET_BTN_A, tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  if (!meu) {
    tab5_botao(d, x0 + 24 + bl + 24, by, bl, DET_BTN_A, tr("CONVERSAR"), TEMA_DESTAQUE, TEMA_SOBRE_DESTAQUE);
  }
}

void desenhar_nos(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  char linha[NOS_LINHA_CH + 1];
  char visto[8];
  // The grid for the current orientation. Changing grid only happens on a rotation,
  // and the rotation already comes with a full pass: everything is rebuilt below.
  s_grade = em_pe(a) ? COLUNA_PE : COLUNA;

  if (s_no_sel) {
    lista_esconder(&s_l_nos);   // the table is not on screen: it neither scrolls nor responds
    if (tecvirt_aberto(&s_kb_busca)) { tecvirt_fechar(&s_kb_busca); }
    desenhar_no_detalhe(d, a, x0);
    tab5_fonte_corpo(d);
    return;
  }

  // The channels changed (new config, conversation deleted/reopened): the strip changes
  // height and the table moves - full pass.
  char assin[sizeof(s_chips_assin)];
  canais_assinatura(assin, sizeof(assin));
  if (strcmp(assin, s_chips_assin) != 0) {
    snprintf(s_chips_assin, sizeof(s_chips_assin), "%s", assin);
    if (!TUDO) { pintura_pedir(&s_p); sistema_pedir_redesenho_completo(); return; }
  }

  if (TUDO) {
    tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO);
    s_chips_altura = desenhar_canais(d, a, x0);
  }

  // EXCEPTION TO FONT4, ON PURPOSE: this table builds ONE SINGLE STRING per row
  // (campo() writes each value at a fixed CHARACTER offset within
  // 'linha', see COLUNA[].col above) and draws the whole row at once. That
  // ONLY aligns in columns if every character has the SAME width in pixels -
  // exactly what Font0 guarantees (6px, always) and Font4 does NOT (it is
  // proportional). That is why it stays on Font0, and only it in this app.
  d.setFont(&fonts::Font0);

  if (TUDO) {
    // Header from the SAME table as the rows - that is what guarantees it never
    // drifts out of place by itself again.
    espacos(linha, sizeof(linha));
    for (int i = 0; i < C_VISTO; ++i) { campo(linha, s_grade[i], tr(s_grade[i].titulo)); }
    linha[linha_ch()] = 0;
    d.setTextSize(2);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(linha, x0 + NOS_MARGEM, a.y + s_chips_altura + 8);
    d.drawString(tr(s_grade[C_VISTO].titulo), x0 + visto_x(), a.y + s_chips_altura + 8);
  }
  d.setTextSize(2);
  d.setTextDatum(top_left);

  desenhar_busca(d, a);

  // FIXED HEADER ("sticky"): it does not scroll, only the rows. With the search
  // keyboard open, the window ends above it.
  const int cabecalho_a = s_chips_altura + 36;
  const int kb_a = tecvirt_aberto(&s_kb_busca) ? tecvirt_altura(a) : 0;
  const area_t janela_nos = { x0, a.y + cabecalho_a, a.x + a.largura - x0, a.altura - cabecalho_a - kb_a };

  // WHAT CHANGED VALUE (the age ticks every second, the SNR changes with the
  // next packet, the order changes when someone speaks): compares with what is
  // written and marks ONLY those rows. The COUNT changed: the whole list.
  const int n = tab5_mesh_num_nos_busca(s_busca);
  if (s_busca_mudou) { s_busca_mudou = false; lista_ao_inicio(&s_l_nos); lista_itens_mudaram(&s_l_nos, n); }
  else if (TUDO || n != s_l_nos.n) {
    lista_itens_mudaram(&s_l_nos, n);
  } else {
    for (int i = 0; i < n; ++i) {
      // Row never drawn (outside the window): scrolling draws it when
      // it appears. Marking it for having empty text would drag the visible ones
      // along (the mark is a range) - see s_cfg_pintada on the "No" page.
      if (i < MESH_MAX_NOS && !s_nos_ant[i][0]) { continue; }
      mesh_no_t no;
      if (!tab5_mesh_no_busca(s_busca, i, &no)) { continue; }
      montar_linha(linha, sizeof(linha), no);
      char curto[8];
      idade_texto(no.idade_s, curto, sizeof(curto));
      snprintf(visto, sizeof(visto), "%-6.6s", curto);
      if (strcmp(linha, s_nos_ant[i]) != 0 || strcmp(visto, s_nos_visto_ant[i]) != 0
       || no.favorito != s_nos_fav_ant[i]) {
        lista_item_mudou(&s_l_nos, i);
      }
    }
  }
  const bool pintou = lista_desenhar(&s_l_nos, d, janela_nos, TEMA_FUNDO, TUDO);
  if (n == 0 && s_busca[0] && pintou) {
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(tr("nenhum no com esse nome ou id"), janela_nos.x + NOS_MARGEM, janela_nos.y + 16);
  }

  if (tecvirt_aberto(&s_kb_busca)) {
    tecvirt_posicionar(&s_kb_busca, a);
    tecvirt_pintar(&s_kb_busca, d, TUDO);
  }

  tab5_fonte_corpo(d);   // gives back Font4 - see the exception above
}

void desenhar_malha(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  if (TUDO) { tab5_tela_limpar(x0, a.y, a.x + a.largura - x0, a.altura, TEMA_FUNDO); }
  mesh_saude_t s; tab5_mesh_saude(&s);
  char buf[160];

  // "-" WHEN IT HAS NOT ARRIVED. The health numbers only exist with OUR node's
  // LocalStats / DeviceMetrics; in internet mode (no node) or before the first
  // telemetry they are zero - and "0.0 %" channel utilisation is a plausible value,
  // which lied "empty mesh" when the truth was "I don't know".
  //
  // ONLINE NODES without LocalStats: counted here, with the same rule as
  // Meshtastic (heard in the last 2 h) - marked as such.
  struct { const char* rot; char val[48]; uint16_t cor; } campos[10];
  if (s.tem_local) {
    snprintf(campos[0].val, sizeof(campos[0].val), "%u / %u", (unsigned)s.nos_online, (unsigned)s.nos_total);
    campos[0].rot = tr("nós online (do rádio)");
  } else {
    int on = 0;
    const int n = tab5_mesh_num_nos();
    for (int i = 0; i < n; ++i) {
      mesh_no_t no;
      if (tab5_mesh_no(i, &no) && no.idade_s >= 0 && no.idade_s <= 7200) { ++on; }
    }
    snprintf(campos[0].val, sizeof(campos[0].val), "%d / %d", on, n);
    campos[0].rot = tr("nós online (contados aqui, 2 h)");
  }
  campos[0].cor = TEMA_SUCESSO;
  const bool tem_uso = s.tem_local || s.tem_metricas;
  if (tem_uso) { snprintf(campos[1].val, sizeof(campos[1].val), "%.1f %%", (double)s.uso_canal); }
  else         { snprintf(campos[1].val, sizeof(campos[1].val), "-"); }
  campos[1].rot = tr("uso do canal"); campos[1].cor = TEMA_DESTAQUE;
  if (tem_uso) { snprintf(campos[2].val, sizeof(campos[2].val), "%.2f %%", (double)s.airtime_tx); }
  else         { snprintf(campos[2].val, sizeof(campos[2].val), "-"); }
  campos[2].rot = tr("airtime tx"); campos[2].cor = TEMA_DESTAQUE;
  if (tem_uso) {
    snprintf(campos[3].val, sizeof(campos[3].val), "%lu h %lu min",
             (unsigned long)(s.uptime_s / 3600), (unsigned long)((s.uptime_s % 3600) / 60));
  } else {
    snprintf(campos[3].val, sizeof(campos[3].val), "-");
  }
  campos[3].rot = tr("uptime do nó"); campos[3].cor = TEMA_TEXTO;
  // LINK WITH THE PATH: "on air" alone did not say which way.
  const uint8_t via = tab5_mesh_via();
  const char* nome_via = !tab5_mesh_servico_ativo() ? tr("parado")
                       : via == AJUSTES_MESH_BLUETOOTH ? "Bluetooth"
                       : via == AJUSTES_MESH_INTERNET  ? tr("Internet") : tr("Grove");
  snprintf(campos[4].val, sizeof(campos[4].val), "%s · %s", nome_via, tab5_mesh_no_ar() ? tr("no ar") : tr("sem quadro"));
  campos[4].rot = tr("enlace"); campos[4].cor = tab5_mesh_no_ar() ? TEMA_SUCESSO : TEMA_PERIGO;

  // SECOND COLUMN: what the radio heard and the immediate surroundings. The packet
  // counters only exist with LocalStats ("-" without it, as above).
  if (s.tem_local) {
    snprintf(campos[5].val, sizeof(campos[5].val), "%lu / %lu", (unsigned long)s.pkt_rx, (unsigned long)s.pkt_tx);
    snprintf(campos[6].val, sizeof(campos[6].val), "%lu · %lu", (unsigned long)s.pkt_ruins, (unsigned long)s.pkt_repetidos);
  } else {
    snprintf(campos[5].val, sizeof(campos[5].val), "-");
    snprintf(campos[6].val, sizeof(campos[6].val), "-");
  }
  campos[5].rot = tr("pacotes recebidos / enviados"); campos[5].cor = TEMA_DESTAQUE;
  campos[6].rot = tr("pacotes ruins · repetidos");    campos[6].cor = TEMA_DESTAQUE;
  {
    const uint32_t meu_no = tab5_mesh_meu_numero();
    int vizinhos = 0, mais_recente = -1;
    int bat = -1;
    const int nn = tab5_mesh_num_nos();
    for (int i = 0; i < nn; ++i) {
      mesh_no_t no;
      if (!tab5_mesh_no(i, &no)) { continue; }
      if (meu_no && no.num == meu_no) { bat = no.bateria; continue; }
      if (no.idade_s < 0) { continue; }
      if (mais_recente < 0 || no.idade_s < mais_recente) { mais_recente = no.idade_s; }
      if (no.saltos == 0 && !no.via_mqtt && no.idade_s <= 7200) { ++vizinhos; }
    }
    snprintf(campos[7].val, sizeof(campos[7].val), "%d", vizinhos);
    campos[7].rot = tr("vizinhos diretos (2 h)"); campos[7].cor = TEMA_SUCESSO;
    if (mais_recente >= 0) {
      char t[8]; idade_texto(mais_recente, t, sizeof(t));
      snprintf(campos[8].val, sizeof(campos[8].val), tr("há %s"), t);
    } else {
      snprintf(campos[8].val, sizeof(campos[8].val), "-");
    }
    campos[8].rot = tr("último nó ouvido"); campos[8].cor = TEMA_TEXTO;
    if (bat < 0)        { snprintf(campos[9].val, sizeof(campos[9].val), "-"); }
    else if (bat > 100) { snprintf(campos[9].val, sizeof(campos[9].val), tr("na tomada")); }
    else                { snprintf(campos[9].val, sizeof(campos[9].val), "%d %%", bat); }
    campos[9].rot = tr("bateria do meu nó"); campos[9].cor = TEMA_TEXTO;
  }

  // TWO COLUMNS, vertical step shared out over the remaining height (never smaller
  // than before). The width of each value is measured: what does not fit in the column
  // drops to the smaller font instead of invading the neighbour.
  //
  // IN PORTRAIT, A SINGLE COLUMN: half of ~630 px does not fit even the longest
  // label, and the remaining height is enough for the ten rows one under the other.
  const int colunas = em_pe(a) ? 1 : 2;
  const int por_col = 10 / colunas;
  const int col_l = (a.x + a.largura - x0 - 48) / colunas;
  int passo = (a.altura - 24 - 40) / por_col;
  if (passo < 88) { passo = 88; }
  if (passo > 120) { passo = 120; }

  // Only repaints the value that changed: the page runs at 1 Hz.
  static char s_val_ant[10][48];
  for (int i = 0; i < 10; ++i) {
    const int x = x0 + 24 + (i / por_col) * col_l;
    const int y = a.y + 24 + (i % por_col) * passo;
    if (!pintura_mudou_s(&s_p, s_val_ant[i], sizeof(s_val_ant[i]), campos[i].val)) { continue; }
    if (!TUDO) { tab5_tela_limpar(x, y, col_l - 8, passo - 2, TEMA_FUNDO); }
    tab5_texto(d, 3.12f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_2, TEMA_FUNDO);
    d.drawString(campos[i].rot, x, y);
    tab5_texto(d, 5.85f);
    if (d.textWidth(campos[i].val) > col_l - 16) { tab5_texto(d, 4.2f); }
    d.setTextColor(campos[i].cor, TEMA_FUNDO);
    d.drawString(campos[i].val, x, y + 26);
  }

  // OUR NODE BY NAME, with the id alongside - it used to be just the number in hex.
  const uint32_t meu = tab5_mesh_meu_numero();
  char curto[MESH_NOME_CURTO * 2] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(meu, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(meu, longo, sizeof(longo));
  if (!meu) { snprintf(buf, sizeof(buf), tr("meu nó: ainda não apresentado")); }
  else {
    snprintf(buf, sizeof(buf), tr("meu nó: %s%s%s%s  !%08lx   firmware %s"),
             longo, (longo[0] && curto[0]) ? " (" : "", curto, (longo[0] && curto[0]) ? ")" : "",
             (unsigned long)meu, tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : "-");
  }
  static char s_rodape_ant[sizeof(buf)];
  if (pintura_mudou_s(&s_p, s_rodape_ant, sizeof(s_rodape_ant), buf)) {
    if (!TUDO) { tab5_tela_limpar(x0, a.y + a.altura - 34, a.x + a.largura - x0, 34, TEMA_FUNDO); }
    texto_que_cabe(d, buf, a.x + a.largura - 16 - (x0 + 24), 3.12f, 2.4f);
    d.setTextDatum(top_left);
    d.setTextColor(TEMA_TEXTO_3, TEMA_FUNDO);
    d.drawString(buf, x0 + 24, a.y + a.altura - 30);
  }
}
