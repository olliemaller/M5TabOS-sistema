// APP: Meshtastic - the WINDOW for the tab5_mesh service.
//
// The app does not own the link: the one talking to the C6L is the service, which
// stays up with this screen closed. Here we only draw what it already knows. It is
// the same design as Settings for WiFi, and it is what allows notifying of a new
// message without the app open.
//
// FOLLOWS THE DEVICE (ORI_SEGUE_IMU in the descriptor): landscape (1280x720) or
// portrait (720x1280). With the keyboard attached, always landscape - the natural
// posture, and the message list fits a lot more.
//
// LANDSCAPE LAYOUT
//   [rail]   [conversations]   main area
//     90px       200px         the rest
//
// The rail is always visible. The conversations column ONLY appears on the
// messages page - on the others it would be dead space. And the compose bar sits
// inside the messages area, not under the columns.
//
// PORTRAIT LAYOUT - three columns do not fit in 720 px. On the messages page the
// conversations column takes the rest of the width (with more room for the name),
// and tapping a conversation opens it ON TOP of everything, full width, with a
// BACK strip at the top. Back returns the list - the same back-and-forth as
// a phone messaging app. On the other pages the rail stays, and the
// content adapts: the node table hides columns, the mesh becomes a single
// column, side-by-side buttons shrink.
//
// THE FILE WAS SPLIT BY SUBJECT (it only moved; what the parts
// share is in app_mesh_interno.hpp):
//   app_mesh.cpp           state, the lists, the loading work, the app cycle
//                          (draw, touch, open, close) and the app_t registration
//   app_mesh_desenho.cpp   drawing, glyphs, the Meshtastic logo, the column grid,
//                          the node table, search, the detail view and the mesh
//   app_mesh_conversa.cpp  location inside a message and the conversation
//   app_mesh_no.cpp        the "No" (Node) page (configuring the node, the internet, the journal)

#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - large buffers in PSRAM
#include "trabalho.hpp"
#include "tab5_teclado.hpp"
#include "rolagem.hpp"
#include "tab5_ui.hpp"
#include "tab5_tema.hpp"
#include "esp_heap_caps.h"
#include "tab5_mesh_web.hpp"
#include <string.h>
#include "app_mesh_interno.hpp"

// WIDTH OF THE CONVERSATIONS COLUMN: 200 in landscape; in portrait, everything left
// after the rail. Stored in the pass that draws the column and read by the item and
// by touch - all three agree because they read the same number.
int s_col_l = CONV_L;
// The "x" sits 50 px from the end of the column (outside the scroll bar strip),
// from the column's x.
int apagar_dx(void) { return s_col_l - 50; }

// IN PORTRAIT, THE OPEN CONVERSATION OVER THE COLUMN. false = the list; true = the
// conversation at full width, with BACK. Not consulted in landscape (the two
// appear side by side).
static bool s_conv_frente = false;

bool em_pe(const area_t& a) { return a.largura < a.altura; }

// Cuts 't' at the end, per character (UTF-8), until it fits in max_l px in the
// active font. In portrait almost all long text goes through here.
void cortar_ao_caber(lgfx::LovyanGFX& d, char* t, int max_l) { tab5_cortar_ao_caber(d, t, max_l); }

// Picks the smooth font from size 'tam' downwards (step 0.2) until 't'
// fits in max_l; at the smallest size, cuts. Leaves the chosen font active.
void texto_que_cabe(lgfx::LovyanGFX& d, char* t, int max_l, float tam, float tam_min)
{
  tab5_texto(d, tam);
  while (tam > tam_min && (int)d.textWidth(t) > max_l) {
    tam -= 0.2f;
    tab5_texto(d, tam);
  }
  cortar_ao_caber(d, t, max_l);
}

pagina_t s_pag = PAG_MSGS;

int  mesh_s_conv = -1;              // open conversation (index in the column); -1 = none
chave_t  s_ab = { };            // the open conversation (ok = false: none)
static uint32_t s_conv_ver = 0;        // the order version mesh_s_conv follows

bool chave_igual(const chave_t& a, const chave_t& b)
{
  if (a.ok != b.ok) { return false; }
  if (!a.ok)        { return true; }
  return a.canal == b.canal && (a.canal ? a.idx == b.idx : a.no == b.no);
}

static chave_t chave_de(int c)
{
  mesh_conversa_t m;
  if (c < 0 || !tab5_mesh_conversa(c, &m)) { return chave_t{ }; }
  return chave_t{ true, m.canal, m.idx, m.no };
}

// THE CONVERSATION - bubbles, compose bar, keyboard and scrolling are the system
// component (kernel/conversa). See conv_ler(), further down.
EXT_RAM_BSS_ATTR conversa_t s_c;   // ~1 KB, only the UI touches it: PSRAM

// History of the open conversation. In RAM so the screen draws fast; the source of
// truth is the SD card.
// HISTORY IN PSRAM, and 200 instead of 40.
//
// It was 7.6KB of INTERNAL RAM held even with the app closed - the largest static
// symbol in the whole system. In PSRAM, allocated on open and returned on
// close, it costs nothing when nobody is chatting; and 200 messages are
// 38KB, irrelevant in the 29MB free.
//
// The larger number is not a whim: without scrolling, 40 were enough because only
// about 8 fitted on screen and the rest were unreachable. With scrolling, the limit
// stops being the screen and becomes this array - so it has to be worth scrolling up.
#define HIST_MAX 200
mesh_msg_t* s_hist = nullptr;   // what the screen shows (only the UI reads it)
chave_t     s_hist_chave = { }; // which conversation s_hist belongs to

// LOADING THE HISTORY, OUTSIDE THE UI (kernel/trabalho). The
// worker reads the SD card into THIS buffer - never the one the screen draws - and
// the UI only swaps the two in ao_aviso(AVISO_TRABALHO). Previously the read was
// done on the UI task for every new message: mounting the SD and reading the
// whole conversation with the screen and touch frozen.
static mesh_msg_t*   s_carga = nullptr;
static volatile int  s_carga_n = 0;
static chave_t       s_carga_chave = { };   // by key: the order may change before the worker runs
static bool          s_carga_cartao = false;   // the request includes carregar_do_cartao
static volatile bool s_carga_pronta = false;
static bool          s_carga_pedida = false;
static bool          s_recarregar   = false;   // asked again during the load
int  s_n_hist = 0;

// ---------------------------------------------------------------------------
// THE APP'S THREE LISTS - all kernel/lista
// ---------------------------------------------------------------------------
// Messages, conversations and nodes each scroll in their own window; the rail and
// the navigation stay still. That is why it is not the system scrolling
// (altura_conteudo), which scrolls the whole area.
//
//   bubbles  - the conversation component's list (s_c.lista).
//   s_l_conv - conversations column (up to MESH_MAX_CONVERSAS; about 7 fit).
//   s_l_nos  - node table (up to MESH_MAX_NOS; about 16 fit), below the
//              fixed header.
//
// The list takes care of scrolling, clipping, drawing only what is exposed, the bar,
// and touch (drag and "which item"). The app only says the height and how to draw an
// item. On a page change, ir_para_pagina() hides the lists of the page
// being left - a list that is not on screen does not respond to touch.
lista_t s_l_conv;
lista_t s_l_nos;
bool s_hist_pronto = false;

// Full repaint pending. Without this the app flickers: it redraws at 1Hz
// (periodo_ms = 1000) and each page cleared the whole area before writing.
// Text with a background colour OVERWRITES, and does not flicker; what flickers is fillRect.
// Same lesson as Settings and the status bar.
pintura_t s_p;

// How many messages the service had when we last read the SD card.
//
// It exists so the history is NOT re-read on every repaint. Reading means mounting
// the card, walking the file and unmounting - expensive, and unnecessary when only
// the drawing changed. We reload only when the conversation changes or a new
// message arrives.
// int32_t, and not uint32_t: -1 has to fit as "never read yet" and be
// different from any real tab5_mesh_total_msgs() (which starts at 0)
// - see the big notice next to its use, further down.
static int64_t s_msgs_quando_li = -1;

// Version of the send states the screen has already drawn.
static uint32_t s_envios_ver = 0;

// Runs ON THE WORKER. Only touches s_carga*: the screen reads nothing from here.
static void trabalho_carregar(void*)
{
  if (s_carga_cartao) { tab5_mesh_carregar_do_cartao(); }
  const chave_t k = s_carga_chave;
  s_carga_n = k.ok ? tab5_mesh_historico_de(k.canal, k.idx, k.no, s_carga, HIST_MAX) : 0;
  s_carga_pronta = true;
}

static bool alocar_hist(void)
{
  if (!s_hist)  { s_hist  = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  if (!s_carga) { s_carga = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * HIST_MAX, MALLOC_CAP_SPIRAM); }
  return s_hist && s_carga;
}

// REQUESTS the load (reads nothing here). A conversation different from the one on
// screen: the screen empties at once, instead of showing the previous conversation
// until the load arrives.
void carregar_hist(void)
{
  s_hist_pronto = true;   // request made; the result arrives via notice
  if (!alocar_hist()) { s_n_hist = 0; return; }
  if (!chave_igual(s_hist_chave, s_ab)) {
    s_n_hist = 0;
    s_hist_chave = s_ab;
    atualizar_eh_canal();
    conversa_mudou(&s_c);
  }
  if (s_carga_pedida) { s_recarregar = true; return; }
  s_carga_chave  = s_ab;
  s_carga_pronta = false;
  s_carga_pedida = sistema_trabalho(trabalho_carregar, nullptr);
  if (s_carga_pedida) { s_carga_cartao = false; }
  else                { s_hist_pronto = false; }   // queue full: try on the next pass
}

// On the UI. Load finished -> swap the buffers; any other notice
// (message, send, nodes) -> redraw, and desenhar() checks the counters.
static void ao_aviso(uint32_t av)
{
  if ((av & AVISO_TRABALHO) && s_carga_pronta) {
    s_carga_pronta = false;
    s_carga_pedida = false;
    if (chave_igual(s_carga_chave, s_ab)) {
      mesh_msg_t* t = s_hist; s_hist = s_carga; s_carga = t;
      s_n_hist     = s_carga_n;
      s_hist_chave = s_carga_chave;
      conversa_mudou(&s_c);
    }
    // Conversation changed during the load, or a message arrived midway: again.
    if (!chave_igual(s_carga_chave, s_ab) || s_recarregar) {
      s_recarregar = false;
      s_hist_pronto = false;
    }
  // The first load also brought names and conversations from the SD card: the
  // conversations column needs to be repainted in full.
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  // The web server turned on, off or someone connected: the PC sub-page (and
  // the group summary) compares the panel signature when drawing.
  sistema_pedir_redesenho();
}

static void desenhar(lgfx::LovyanGFX& d, const area_t& a)
{

  // pintura_iniciar() already checks by itself whether the system cleared the area - that
  // was the step that, when forgotten, left the screen blank on waking.
  //
  // Repainting does NOT imply re-reading the SD card: the RAM copy stays valid.
  // Confusing the two made the app mount the SD on every tap.
  // THE COLUMN ORDER CHANGED: find the open conversation again by key. If it
  // left the column (channel disabled on the node, service switching path), the screen
  // goes back to "pick one" but the key stays: when it reappears, it reopens.
  // BEFORE pintura_iniciar, so this pass already comes out complete - the whole
  // column moved (see the full-repaint rule).
  const uint32_t cv = tab5_mesh_conversas_versao();
  if (cv != s_conv_ver) {
    s_conv_ver = cv;
    if (s_ab.ok) {
      const int novo = tab5_mesh_conversa_indice(s_ab.canal, s_ab.idx, s_ab.no);
      if (novo != mesh_s_conv) {
        if ((novo < 0) != (mesh_s_conv < 0)) { conversa_trocar(&s_c); s_hist_pronto = false; }
        mesh_s_conv = novo;
      }
    }
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
  }

  pintura_iniciar(&s_p);

  // The first load also reads the names and conversations from the SD card - and it has to
  // happen even with no conversation open, otherwise the column is born empty without the node.
  if (s_carga_cartao && !s_carga_pedida) { carregar_hist(); }

  // New message in the service: now it is worth re-reading.
  //
  // tab5_mesh_total_msgs(), NOT tab5_mesh_num_msgs() - symptom: a new
  // message only appeared after switching screens and coming back. The
  // latter saturates at MESH_MAX_MSGS (8) and stays put forever after
  // the eighth message since the service booted - the comparison below never
  // saw a difference again, so the app only reloaded through other paths
  // (switching conversation, switching page) that reset s_hist_pronto directly,
  // never by itself while the screen stayed open. See the big notice in the
  // .hpp next to tab5_mesh_total_msgs().
  const int64_t nm = (int64_t)tab5_mesh_total_msgs();
  // DIRECT COMPARISON, and not pintura_mudou_i(). The helper returns true on every
  // FULL pass, by design - and a full pass is what makes the app repaint everything.
  // Using it to decide I/O creates a loop: full pass -> "changed" -> reload
  // -> pintura_pedir() -> next full pass -> ... The keyboard repainted every
  // cycle and flickered by itself.
  //
  // Rule: pintura_mudou_* is for deciding DRAWING. Deciding to read the SD card,
  // open a file or talk to hardware compares by hand.
  if (nm != s_msgs_quando_li) {
    // Only REQUESTS the new load (the worker reads the SD card). When it arrives,
    // ao_aviso swaps the buffers and requests the repaint - which here would be too
    // early (the new history does not exist yet) and duplicated.
    //
    // The reading position belongs to the list (kernel/lista): stuck to the end, the
    // new message appears; reading the history, the screen does not move.
    s_hist_pronto = false;
    s_msgs_quando_li = nm;
    sistema_pedir_redesenho();
  }


  // ACK arrived (or the deadline expired): they are the SAME bubbles, only the mark changed.
  // Do not re-read the SD card for this - the content did not change, and mounting the SD on
  // every confirmation would be expensive for nothing. Direct comparison for the same reason as
  // the block above.
  const uint32_t ev = tab5_mesh_envios_versao();
  if (ev != s_envios_ver) { s_envios_ver = ev; conversa_mudou(&s_c); }   // the send marks

  // PORTRAIT: either the list (rail + wide column), or the conversation on top of
  // everything. Landscape: rail, column and conversation side by side, as always.
  const bool pe = em_pe(a);
  const bool frente = pe && s_pag == PAG_MSGS && s_conv_frente && mesh_s_conv >= 0;

  if (TUDO && !frente) { desenhar_trilho(d, a); }
  const int x0 = frente ? a.x
               : a.x + TRILHO_L + ((s_pag == PAG_MSGS && !pe) ? CONV_L : 0);

  switch (s_pag) {
    case PAG_NOS:   desenhar_nos(d, a, x0);   break;
    case PAG_MALHA: desenhar_malha(d, a, x0); break;
    case PAG_NO:    desenhar_config(d, a, x0); break;
    default:
      if (pe && !frente) {
        // In portrait, only the list: the bubbles are not on screen, so they do not scroll
        // nor respond, and the keyboard (if it was open) closes.
        conversa_fechar_teclado(&s_c);
        lista_esconder(&s_c.lista);
      } else {
        desenhar_msgs(d, a, x0);
      }
      break;
  }
  // The column AFTER the messages: desenhar_msgs marks the open conversation as read,
  // and its counter in the column already comes out zeroed in this same pass.
  // With the conversation in front the column is covered: it neither scrolls nor responds.
  if (s_pag == PAG_MSGS && !frente) { desenhar_conversas(d, a); }
  else                              { lista_esconder(&s_l_conv); }
}

// THE SCREEN ROTATED (kernel/orientacao). Landscape -> portrait with a conversation
// open: it comes to the front, and whoever was typing keeps typing.
// Portrait -> landscape: list and conversation go back to being side by side. The rest
// adapts by itself in the full pass the system has already requested.
static void ao_girar(void)
{
  s_conv_frente = (mesh_s_conv >= 0);
  pintura_pedir(&s_p);
}

// In portrait: the conversation leaves the front and the list returns.
static void voltar_a_lista(void)
{
  s_conv_frente = false;
  conversa_fechar_teclado(&s_c);
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

// Physical keyboard on ExtPort1: the conversation component handles it.
static void ao_teclar(const char* texto, uint8_t codigo, uint8_t mod)
{
  // The physical keyboard also edits the node name.
  if (s_pag == PAG_NO && s_editando) {
    if      (codigo == TAB5_TECLA_ENTER)  { cfg_fechar_edicao(true); }
    else if (codigo == TAB5_TECLA_ESC)    { cfg_fechar_edicao(false); }
    else if (codigo == TAB5_TECLA_APAGAR) { cfg_tecla(TECVIRT_APAGAR); }
    else if (texto && texto[0]) {
      // Node name and password are ASCII: the letter composed by the dead key
      // goes back to being the two keys pressed.
      char cru[8];
      tab5_teclado_ascii(texto, cru, sizeof(cru));
      for (const char* c = cru; *c && s_editando; ++c) { cfg_tecla((unsigned char)*c); }
    }
    return;
  }
  // In the node table the physical keyboard writes straight into the search, without tapping the box.
  if (s_pag == PAG_NOS && !s_no_sel) {
    if      (codigo == TAB5_TECLA_ENTER)  { busca_fechar_teclado(); }
    else if (codigo == TAB5_TECLA_ESC)    { busca_limpar(); busca_fechar_teclado(); }
    else if (codigo == TAB5_TECLA_APAGAR) { busca_tecla(TECVIRT_APAGAR); }
    else if (texto && texto[0]) {
      char cru[8];
      tab5_teclado_ascii(texto, cru, sizeof(cru));
      for (const char* c = cru; *c; ++c) { busca_tecla((unsigned char)*c); }
    }
    return;
  }
  if (s_pag != PAG_MSGS || mesh_s_conv < 0) { return; }
  conversa_teclar(&s_c, texto, codigo, mod);
}

// PAGE CHANGE in one place only. The lists of the page being left stop
// responding to touch (lista_esconder) - they come back when drawn.
static void ir_para_pagina(pagina_t p)
{
  s_pag = p;
  s_no_sel = 0;   // tapping the rail always returns to the table
  conversa_fechar_teclado(&s_c);   // gives back the navigation, if it was locked
  lista_esconder(&s_c.lista);
  lista_esconder(&s_l_conv);
  lista_esconder(&s_l_nos);
  lista_esconder(&s_l_cfg);
  if (s_editando) { s_editando = 0; tecvirt_fechar(&s_kb_cfg); }
  if (tecvirt_aberto(&s_kb_busca)) { tecvirt_fechar(&s_kb_busca); }   // the search itself stays
  s_diario = false;
  s_web_pg = false;
  s_redes_pg = false;
  s_net_pg = false;
  s_net_ed = false;
  memset(&s_net_p, 0, sizeof(s_net_p));
  s_grupo = -1;
  s_cfg_msg[0] = 0;
  pintura_pedir(&s_p);
  sistema_pedir_redesenho_completo();
}

// OPEN A CONVERSATION (c = -1: none, "pick one") - from the column, from a node or
// from an available channel. A conversation opens at the most recent message.
static void abrir_conversa(int c)
{
  mesh_s_conv = c;
  s_conv_frente = (c >= 0);   // in portrait, the chosen one comes to the front (in landscape, nothing changes)
  s_ab = chave_de(c);
  s_conv_ver = tab5_mesh_conversas_versao();
  s_hist_pronto = false;
  atualizar_eh_canal();
  conversa_trocar(&s_c);   // closes the keyboard, goes to the end, re-measures
  if (c >= 0) { tab5_mesh_marcar_conversa_lida(c); }
  if (s_pag == PAG_MSGS && c >= 0) {
    // Switching conversation via the column: the keyboard (and the draft) stay.
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }
  ir_para_pagina(PAG_MSGS);
}

static void ao_tocar_nivel(int x, int y, bool pressionado)
{
  // Key under the finger, release and bubble scrolling: the conversation component.
  if (s_pag == PAG_MSGS && mesh_s_conv >= 0) { conversa_tocar_nivel(&s_c, x, y, pressionado); }

  // The other two lists each scroll in their own window; the one not on screen
  // (lista_esconder) ignores touch.
  // Node name keyboard: the keys confirmed in this sample.
  if (s_pag == PAG_NO && s_editando) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb_cfg, acoes, 8);
    for (int k = 0; k < n && s_editando; ++k) { cfg_tecla(acoes[k]); }
    return;
  }
  // Search keyboard: the confirmed keys; the table above it still scrolls.
  if (s_pag == PAG_NOS && tecvirt_aberto(&s_kb_busca)) {
    int acoes[8];
    const int n = tecvirt_nivel(&s_kb_busca, acoes, 8);
    for (int k = 0; k < n && tecvirt_aberto(&s_kb_busca); ++k) { busca_tecla(acoes[k]); }
  }

  bool rolou = false;
  rolou |= lista_arrastar(&s_l_conv, x, y, pressionado);
  rolou |= lista_arrastar(&s_l_nos,  x, y, pressionado);
  rolou |= lista_arrastar(&s_l_cfg,  x, y, pressionado);
  if (s_redes_pg) { rolou |= lista_arrastar(&s_l_redes, x, y, pressionado); }
  if (rolou) { sistema_pedir_redesenho(); }
}

static void ao_tocar(int x, int y)
{
  const area_t a = sistema_area();

  // Keyboard open: its strip (full width) belongs to it alone.
  if (conversa_teclado_aberto(&s_c) && s_pag == PAG_MSGS && y >= a.altura - tecvirt_altura(a)) {
    conversa_tocar(&s_c, x, y);
    return;
  }

  // PORTRAIT, CONVERSATION IN FRONT: there is no rail nor column under the finger -
  // only the BACK strip and the conversation.
  if (em_pe(a) && s_pag == PAG_MSGS && s_conv_frente && mesh_s_conv >= 0) {
    if (y < FRENTE_TOPO_A) {
      if (x >= 16 && x < 16 + FRENTE_VOLTAR_L
       && y >= FRENTE_VOLTAR_Y && y < FRENTE_VOLTAR_Y + FRENTE_VOLTAR_A) {
        voltar_a_lista();
      }
      return;
    }
    conversa_tocar(&s_c, x, y);
    return;
  }

  if (x < TRILHO_L) {
    for (int i = 0; i < PAG_TOTAL; ++i) {
      const int iy = 16 + i * (ITEM_A + 12);
      if (y >= iy && y < iy + ITEM_A) {
        s_conv_frente = false;   // in portrait, the rail always leads to the list
        ir_para_pagina((pagina_t)i);
        return;
      }
    }
    return;
  }

  // NODES PAGE: a channel opens the conversation; a node opens its card.
  if (s_pag == PAG_NOS) {
    // A NODE'S CARD open: STAR, BACK and CHAT.
    if (s_no_sel) {
      const int fav_l = det_fav_l(a);
      const int fx = a.largura - DET_FAV_DIR - fav_l;
      if (s_no_sel != tab5_mesh_meu_numero()
       && x >= fx && x < fx + fav_l && y >= DET_FAV_Y && y < DET_FAV_Y + DET_FAV_A) {
        tab5_mesh_definir_favorito(s_no_sel, !tab5_mesh_favorito(s_no_sel));
        sistema_pedir_redesenho();
        return;
      }
      const int by = a.altura - DET_BTN_Y_DA_BASE;
      if (y >= by && y < by + DET_BTN_A) {
        const int bx = TRILHO_L + 24;
        const int bl = det_btn_l(a);
        if (x >= bx && x < bx + bl) {
          s_no_sel = 0;
          pintura_pedir(&s_p);
          sistema_pedir_redesenho_completo();
        } else if (x >= bx + bl + 24 && x < bx + 2 * bl + 24
                && s_no_sel != tab5_mesh_meu_numero()) {
          // -1 when the number is not usable (zero, or the node itself).
          const int c = tab5_mesh_conversa_com(s_no_sel);
          s_no_sel = 0;
          if (c >= 0) { abrir_conversa(c); }
        }
      }
      return;
    }

    // THE SEARCH KEYBOARD open: its strip belongs to it alone.
    if (tecvirt_aberto(&s_kb_busca) && tecvirt_contem(&s_kb_busca, a.x + x, a.y + y)) {
      const int ac = tecvirt_clique(&s_kb_busca, a.x + x, a.y + y);
      if (ac) { busca_tecla(ac); }
      return;
    }

    // THE SEARCH BOX: the "x" clears; the rest opens the keyboard.
    {
      const chip_t& k = s_chip_busca;
      if (k.l && x >= k.x && x < k.x + k.l && y >= k.y && y < k.y + k.a) {
        if (s_busca[0] && x >= k.x + k.l - BUSCA_LIMPAR_L) { busca_limpar(); }
        else { busca_abrir_teclado(); }
        return;
      }
    }

    // A CHANNEL: opens its conversation (and brings it back, if it was deleted).
    for (int i = 0; i < s_n_chips; ++i) {
      const chip_t& k = s_chips[i];
      if (x < k.x || x >= k.x + k.l || y < k.y || y >= k.y + k.a) { continue; }
      const int c = tab5_mesh_conversa_do_canal(k.idx);
      if (c >= 0) { abrir_conversa(c); }
      return;
    }

    // A NODE: its card. You chat from there - previously the tap opened the conversation
    // directly, and there was nowhere to see MAC, voltage or position.
    const int i = lista_item_tocado(&s_l_nos, x, y, nullptr);
    if (i < 0) { return; }
    mesh_no_t no;
    if (!tab5_mesh_no_busca(s_busca, i, &no)) { return; }
    s_no_sel = no.num;
    pintura_pedir(&s_p);
    sistema_pedir_redesenho_completo();
    return;
  }

  if (s_pag == PAG_NO) { tocar_config(x, y); return; }
  if (s_pag == PAG_MALHA) { return; }

  if (s_pag != PAG_MSGS) { return; }

  // Conversations column.
  if (x < TRILHO_L + s_col_l) {
    int dy = 0;
    const int i = lista_item_tocado(&s_l_conv, x, y, &dy);
    if (i < 0) { return; }
    dy -= 8;                          // the item starts with the spacing (conv_desenhar)
    if (dy < 0 || dy >= ITEM_A) { return; }   // in the spacing between items: nothing

    // The "x" comes BEFORE selecting: it is inside the item, and handling the
    // selection first would turn the button tap into an item tap.
    const int ax = TRILHO_L + 1 + apagar_dx();
    if (i == mesh_s_conv
     && x >= ax && x < ax + APAGAR_L
     && dy >= APAGAR_DY && dy < APAGAR_DY + APAGAR_A
     && tab5_mesh_esquecer_conversa(i)) {
      // No conversation is selected in its place: the indices above this one shifted
      // by one, and inheriting the index would open the NEIGHBOURING conversation - one the
      // person did not ask for. Better to go back to "pick one".
      abrir_conversa(-1);
      return;
    }
    abrir_conversa(i);
    return;
  }

  // Compose bar and send button.
  // Conversation column: compose bar, ENV and scrolling - the component.
  if (mesh_s_conv >= 0) { conversa_tocar(&s_c, x, y); }
}

static bool ao_abrir(void)
{
  // THE FILE FIRST. Without the C6L plugged in the conversations list was born empty -
  // it was only populated by incoming messages - and the app opened saying "no
  // conversations" with the SD card full of them.
  //
  // Applies with the radio on air too: old conversations appear from opening,
  // and not only when someone speaks again.
  // Names, conversations and history come from the SD card VIA THE WORKER, together with the
  // first load (see trabalho_carregar) - the screen opens immediately.
  s_carga_cartao = true;
  s_carga_pedida = false;
  s_carga_pronta = false;
  s_recarregar   = false;
  s_hist_chave   = chave_t{ };

  s_hist_pronto = false;
  conversa_abrir(&s_c, &s_p, conv_contar, conv_ler, conv_enviar, nullptr);
  s_c.limite = MESH_PAYLOAD_MAX;   // the protocol ceiling, not the buffer's (see tab5_mesh.hpp)
  s_c.cor_enviar = TEMA_DESTAQUE;   // the theme's only strong colour (the button text is TEMA_SOBRE_DESTAQUE)
  lista_definir(&s_l_conv, ROLAGEM_INICIO, 0, conv_altura, conv_desenhar, nullptr);
  s_l_conv.topo = 4;   // the 1st item already has 8px of spacing: 12 from the top, as always
  lista_definir(&s_l_nos, ROLAGEM_INICIO, 0, nos_altura, nos_desenhar, nullptr);
  lista_definir(&s_l_cfg, ROLAGEM_INICIO, 0, cfg_altura, cfg_desenhar, nullptr);
  lista_definir(&s_l_redes, ROLAGEM_INICIO, 0, rede_altura, rede_desenhar, nullptr);
  pintura_pedir(&s_p);
  sistema_travar_navegacao(false);
  return true;
}

static void ao_fechar(void)
{
  // Returns the history. It is 38KB of PSRAM nobody misses with the
  // app closed - and the reason it is no longer static.
  // (The system has already waited for the worker before calling this.)
  if (s_hist)  { heap_caps_free(s_hist);  s_hist  = nullptr; }
  if (s_carga) { heap_caps_free(s_carga); s_carga = nullptr; }
  s_hist_chave = chave_t{ };
  if (s_editando) { s_editando = 0; tecvirt_fechar(&s_kb_cfg); }
  if (tecvirt_aberto(&s_kb_busca)) { tecvirt_fechar(&s_kb_busca); }
  s_diario = false;
  s_web_pg = false;
  s_redes_pg = false;
  s_net_pg = false;
  s_net_ed = false;
  memset(&s_net_p, 0, sizeof(s_net_p));
  s_grupo = -1;
  conversa_fechar(&s_c);   // returns the bubble cache and the navigation
  s_n_hist = 0;
  s_hist_pronto = false;
}

static uint32_t periodo_agora(void)
{
  return (s_pag == PAG_NOS || s_pag == PAG_MALHA || s_pag == PAG_NO) ? 1000 : 0;
}

extern const app_t APP_MESH = {
  .nome  = "Mesh",
  .cor   = TFT_GREENYELLOW,
  // Grove is already kept on by the system setting. The IMU is so the screen follows
  // the device.
  .perfil = { .imu = true },
  // The history belongs to the Mesh services (services/mesh), which mount the SD card
  // only while they read or write: the app does not hold the card open (with it
  // open, the USB flash drive was refused).
  .usa_sd = false,
  // Portrait or landscape following the device; with the keyboard attached, landscape -
  // the natural posture for typing (see LAYOUT at the top of the file).
  .orientacao = ORI_SEGUE_IMU,
  .ao_abrir  = ao_abrir,
  .ao_fechar = ao_fechar,
  .desenhar  = desenhar,
  .ao_tocar  = ao_tocar,
  .ao_tocar_nivel = ao_tocar_nivel,   // lights the key under the finger
  .ao_teclar = ao_teclar,             // physical keyboard on ExtPort1
  .periodo_ms = 0,
  // Clock ONLY on the pages that show age (Nodes, Mesh); on the messages page the app
  // only wakes by notice - new message, send confirmed, new node, history load
  // ready.
  .periodo_agora = periodo_agora,
  .avisos = AVISO_MESH_MSG | AVISO_MESH_ENVIO | AVISO_MESH_NOS | AVISO_TRABALHO | AVISO_WEB,
  .ao_aviso = ao_aviso,
  // The home screen icon is the Meshtastic logo itself: whoever looks for this
  // app looks for its symbol, not the letter M.
  .icone = marca_mesh,
  .ao_girar = ao_girar,
  .comandos = nullptr,
  .fora_da_home = false,
  .no_pc = tab5_mesh_web_ligar,
  .fundo_tema = true,   // the system clears the area with the theme background
};
