// APP Mesh - the CONVERSATION: the location inside a message (a coordinate
// turned into distance and bearing) and the conversation itself (bubbles, strip, sending).
// Split from app_mesh.cpp (the app cycle); what the parts share is in
// app_mesh_interno.hpp.

#include "idioma.h"
#include "tab5_tela.hpp"
#include "tab5_ui.hpp"
#include "ajustes.h"
#include "tab5_gps.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "app_mesh_interno.hpp"
#include "tab5_tema.hpp"

static bool s_eh_canal = false;       // open conversation is a channel one (sender label)

// SENDER LABEL, INSIDE the bubble (it was ABOVE, outside it;
// changed) - only in a CHANNEL conversation, where several different people
// write in the same place. In a direct conversation the side and colour of the bubble
// already say who sent it; a label there would only repeat information. The HEIGHT
// reserved for it (NOME_A) is actually measured in desenhar_msgs() - see the
// notice there - no longer a fixed number here.

// Sender name for the label - see tab5_mesh_nome_exibicao() in the service:
// shortname first, ID in hex if the shortname is unusable (empty, "?" or
// emoji) or the node has not introduced itself yet. Only a thin wrapper here; the
// rule itself lives in the service because the direct conversation (tab5_mesh.cpp)
// needs the SAME rule, and two copies of it is exactly the kind of duplication
// tab5_ui.hpp exists to avoid (see the notice at the top of that file).
static void nome_do_no(uint32_t num, char* out, size_t out_tam)
{
  tab5_mesh_nome_exibicao(num, out, out_tam);
}

// COLOUR PER SENDER, only for the name label in a CHANNEL conversation - requested
// after seeing the official Meshtastic phone app, which
// paints each participant in their own colour instead of the same uniform grey
// for everyone. Simple hash (the node number itself, modulo the palette
// size): determinism is all that is needed here - the same person always
// lands on the same colour IN THIS session, and that is enough for the eye to learn to
// tell who wrote what in a window full of people. It does not need to
// survive a restart, it is only visual identification, not stored data.
static uint16_t cor_do_no(uint32_t num)
{
  static const uint16_t PALETA[] = {
    TEMA_APP_AZUL, TEMA_APP_LARANJA, TEMA_APP_ROSA, TEMA_APP_AMARELO,
    TEMA_APP_CIANO, TEMA_APP_ROXO, TEMA_APP_LIMA, TEMA_APP_VERDE,
  };
  return PALETA[num % (sizeof(PALETA) / sizeof(PALETA[0]))];
}

// -----------------------------------------------------------------------
// LOCATION INSIDE A MESSAGE
// -----------------------------------------------------------------------
// Whoever shares a location via the Meshtastic phone app almost
// always sends a normal TEXT message with the coordinates embedded
// (a link like "https://maps.google.com/?q=-12.97,-38.50", or just the raw
// pair) - there is no separate packet type the conversation needs to
// understand. So the detection is about the TEXT, not the protocol.
//
// achar_coord() looks, ANYWHERE in the string, for a "number,
// number" pair (with the first fitting as latitude and the second as longitude) -
// it works for both the link and the raw pair, regardless of the
// rest of the surrounding text (URL, caption, whatever comes).
static bool achar_coord(const char* txt, float* lat, float* lon)
{
  for (const char* p = txt; *p; ++p) {
    if (*p != '-' && (*p < '0' || *p > '9')) { continue; }

    char* fim1 = nullptr;
    const double a = strtod(p, &fim1);
    if (fim1 == p) { continue; }

    const char* q = fim1;
    while (*q == ' ') { ++q; }
    if (*q != ',') { continue; }
    ++q;
    while (*q == ' ') { ++q; }

    char* fim2 = nullptr;
    const double b = strtod(q, &fim2);
    if (fim2 == q) { continue; }

    // The FIRST number has to fit as latitude and the SECOND as longitude -
    // that is what distinguishes a coordinate pair from any two numbers
    // separated by a comma that happen to appear in a text.
    if (a < -90.0 || a > 90.0)   { continue; }
    if (b < -180.0 || b > 180.0) { continue; }

    *lat = (float)a;
    *lon = (float)b;
    return true;
  }
  return false;
}

// Distance (haversine formula, correct at any scale) and direction
// (initial bearing, rounded to the 8 compass points) from the reference
// position ("home", ajustes.h) to the received location.
void distancia_direcao(float lat1, float lon1, float lat2, float lon2,
                              float* dist_km, const char** direcao)
{
  const double GRAU = M_PI / 180.0;
  const double R_TERRA = 6371.0;   // km

  const double dlat = (lat2 - lat1) * GRAU;
  const double dlon = (lon2 - lon1) * GRAU;
  const double s = sin(dlat / 2) * sin(dlat / 2)
                 + cos(lat1 * GRAU) * cos(lat2 * GRAU) * sin(dlon / 2) * sin(dlon / 2);
  *dist_km = (float)(2.0 * R_TERRA * atan2(sqrt(s), sqrt(1.0 - s)));

  const double y = sin(dlon) * cos(lat2 * GRAU);
  const double x = cos(lat1 * GRAU) * sin(lat2 * GRAU)
                  - sin(lat1 * GRAU) * cos(lat2 * GRAU) * cos(dlon);
  double rumo = atan2(y, x) * 180.0 / M_PI;
  if (rumo < 0.0) { rumo += 360.0; }

  static const char* NOMES[8] = { "N", "NE", "L", "SE", "S", "SO", "O", "NO" };
  *direcao = NOMES[((int)((rumo + 22.5) / 45.0)) % 8];
}

// ---------------------------------------------------------------------------
// THE CONVERSATION - the system component (kernel/conversa),
// ---------------------------------------------------------------------------
// Bubbles, compose bar, virtual keyboard and scrolling used to be a whole copy
// here (~700 lines). The component was born FROM THEM, for ESP-NOW; now Mesh
// goes back to using the original, and a fix to the keyboard or the bubble applies to
// both. What is Mesh-only lives in the three answers below: sender label
// on a channel, "N hops"/MQTT, coordinate turned into distance and bearing, and the four
// send marks.
// Background of the messages area (the same as the component - rail and columns use it).
uint16_t fundo_msg(lgfx::LovyanGFX&) { return TEMA_FUNDO; }

static float s_ref_lat = 0, s_ref_lon = 0;   // distance reference (GPS or home)
static bool  s_tem_ref = false;

void atualizar_eh_canal(void)
{
  s_eh_canal = s_ab.ok && s_ab.canal;
}

int conv_contar(void*) { return (s_hist && s_ab.ok && chave_igual(s_hist_chave, s_ab)) ? s_n_hist : 0; }

bool conv_ler(int i, conversa_msg_t* o, void*)
{
  if (!s_hist || i < 0 || i >= s_n_hist) { return false; }
  const mesh_msg_t* m = &s_hist[i];

  // The component reads in order, from 0 onwards: the distance reference holds
  // for the whole pass (one GPS reading, not two hundred).
  if (i == 0) {
    const nmea_posicao_t fix = tab5_gps_posicao();
    s_tem_ref = false;
    if (fix.valida && fix.qualidade > 0) { s_ref_lat = (float)fix.lat; s_ref_lon = (float)fix.lon; s_tem_ref = true; }
    else if (ajustes_casa(&s_ref_lat, &s_ref_lon)) { s_tem_ref = true; }
  }

  o->minha  = (m->de == tab5_mesh_meu_numero());
  o->quando = m->quando;

  // A coordinate in the text becomes "pin, lat, lon, distance and bearing".
  float lat, lon;
  if (achar_coord(m->texto, &lat, &lon)) {
    if (s_tem_ref) {
      float km; const char* dir;
      distancia_direcao(s_ref_lat, s_ref_lon, lat, lon, &km, &dir);
      snprintf(o->texto, sizeof(o->texto), "\xF0\x9F\x93\x8D %.4f, %.4f  %.1fkm %s",
               (double)lat, (double)lon, (double)km, dir);
    } else {
      snprintf(o->texto, sizeof(o->texto), "\xF0\x9F\x93\x8D %.4f, %.4f", (double)lat, (double)lon);
    }
  } else {
    snprintf(o->texto, sizeof(o->texto), "%s", m->texto);
  }

  // Label only on a CHANNEL (several people in the same place), and only on the first of
  // a run from the same sender.
  if (s_eh_canal && !o->minha && (i == 0 || s_hist[i - 1].de != m->de)) {
    nome_do_no(m->de, o->nome, sizeof(o->nome));
    o->cor_nome = cor_do_no(m->de);
  }

  if (!o->minha) {
    // How it arrived: MQTT (internet) or how many radio hops. Without the data
    // in the packet, what the node table knows about the sender.
    bool via_mqtt = m->via_mqtt;
    uint8_t saltos = m->saltos;
    if (!via_mqtt && saltos == MESH_SALTOS_DESCONHECIDO) {
      bool nm = false; uint8_t ns = MESH_SALTOS_DESCONHECIDO;
      if (tab5_mesh_no_enlace(m->de, &nm, &ns)) { via_mqtt = nm; saltos = ns; }
    }
    if (via_mqtt)                                { snprintf(o->meta, sizeof(o->meta), "MQTT"); }
    else if (saltos != MESH_SALTOS_DESCONHECIDO) { snprintf(o->meta, sizeof(o->meta), tr("%u salto%s"), (unsigned)saltos, saltos == 1 ? "" : "s"); }
    else                                         { snprintf(o->meta, sizeof(o->meta), "--"); }
    o->cor_meta = via_mqtt ? (uint16_t)TEMA_DESTAQUE
                : (saltos == MESH_SALTOS_DESCONHECIDO) ? (uint16_t)TEMA_TEXTO_3
                : (saltos <= 1) ? (uint16_t)TEMA_SUCESSO
                : (saltos <= 3) ? (uint16_t)TEMA_APP_LIMA : (uint16_t)TEMA_ALERTA;
  } else if (m->id != 0) {
    // The live send state (the ACK arrives later), or what was stored.
    uint8_t erro = 0;
    uint8_t est = tab5_mesh_estado_envio(m->id, &erro);
    if (est == MESH_ENVIO_NA) { est = m->envio; }
    o->envio = (est == MESH_ENVIANDO)     ? CONVERSA_ENVIO_PENDENTE
             : (est == MESH_ENTREGUE)     ? CONVERSA_ENVIO_OK
             : (est == MESH_FALHOU)       ? CONVERSA_ENVIO_FALHOU
             : (est == MESH_SEM_RESPOSTA) ? CONVERSA_ENVIO_SEM_RESPOSTA
             : (est == MESH_OUVIDO)       ? CONVERSA_ENVIO_OUVIDO
                                          : CONVERSA_ENVIO_NADA;
  }
  return true;
}

bool conv_enviar(const char* texto, void*)
{
  // BY KEY: it is for whoever is on screen, even if the column was
  // reordered between the drawing and the tap on SEND.
  if (mesh_s_conv < 0 || !s_ab.ok || !tab5_mesh_servico_ativo()) { return false; }
  const bool ok = tab5_mesh_enviar(s_ab.canal ? 0xFFFFFFFFu : s_ab.no, s_ab.canal ? s_ab.idx : 0, texto);
  s_hist_pronto = false;          // re-read including the one that just went out
  return ok;
}

// THE TOP STRIP OF THE CONVERSATION IN PORTRAIT: BACK, the type (channel or direct) and the
// name. In landscape it does not exist - the column alongside already says which conversation it is.
// Repainted only when the name changes (or in the full pass).
static void desenhar_faixa_frente(lgfx::LovyanGFX& d, const area_t& a, const mesh_conversa_t& c)
{
  static char s_ant[MESH_NOME_CONVERSA + MESH_NOME_LONGO + 4];
  char sig[sizeof(s_ant)];
  snprintf(sig, sizeof(sig), "%s|%s", c.nome, c.sub);
  if (!pintura_mudou_s(&s_p, s_ant, sizeof(s_ant), sig)) { return; }

  const uint16_t FUNDO = fundo_msg(d);
  tab5_tela_limpar(a.x, a.y, a.largura, FRENTE_TOPO_A, FUNDO);
  tab5_botao(d, a.x + 16, a.y + FRENTE_VOLTAR_Y, FRENTE_VOLTAR_L, FRENTE_VOLTAR_A,
             tr("VOLTAR"), TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);
  const int gl = FRENTE_TOPO_A - 24;
  const int gx = a.x + 16 + FRENTE_VOLTAR_L + 20;
  caixa_glifo(d, gx, a.y + 12, gl, gl, c.canal ? GL_DIFUSAO : GL_DIRETO,
              c.canal ? TEMA_DESTAQUE_SUAVE : TEMA_SUPERFICIE_ALTA, TEMA_TEXTO);

  // Name and, below it, the long name (or "channel N") - cut per character until
  // it fits, as in the column.
  const int tx = gx + gl + 16;
  const int max_l = a.x + a.largura - 16 - tx;
  char nome[MESH_NOME_CONVERSA], sub[MESH_NOME_LONGO];
  snprintf(nome, sizeof(nome), "%s", c.nome);
  snprintf(sub, sizeof(sub), "%s", c.sub);
  d.setTextDatum(top_left);
  tab5_texto(d, 3.84f);
  cortar_ao_caber(d, nome, max_l);
  d.setTextColor(TEMA_TEXTO, FUNDO);
  d.drawString(nome, tx, a.y + (sub[0] ? 10 : 24));
  if (sub[0]) {
    tab5_texto(d, 2.4f);
    cortar_ao_caber(d, sub, max_l);
    d.setTextColor(TEMA_TEXTO_3, FUNDO);
    d.drawString(sub, tx, a.y + 50);
  }
  d.drawFastHLine(a.x, a.y + FRENTE_TOPO_A - 1, a.largura, SIS_BORDA);
}

void desenhar_msgs(lgfx::LovyanGFX& d, const area_t& a, int x0)
{
  const int larg = a.x + a.largura - x0;
  if (mesh_s_conv < 0) {
    conversa_fechar_teclado(&s_c);
    lista_esconder(&s_c.lista);   // no conversation, no bubbles to scroll
    if (TUDO) {
      const uint16_t FUNDO = fundo_msg(d);
      tab5_tela_limpar(x0, a.y, larg, a.altura, FUNDO);
      tab5_texto(d, 3.4f);
      d.setTextDatum(middle_center);
      d.setTextColor(TEMA_TEXTO_2, FUNDO);
      d.drawString(tr("escolha uma conversa"), x0 + larg / 2, a.y + a.altura / 2);
      d.setTextDatum(top_left);
    }
    return;
  }
  mesh_conversa_t c = {};
  if (tab5_mesh_conversa(mesh_s_conv, &c) && c.nao_lidas > 0) { tab5_mesh_marcar_conversa_lida(mesh_s_conv); }
  if (!s_hist_pronto) { carregar_hist(); }

  // In portrait the conversation is ON TOP of the column: the BACK strip takes the top.
  int topo = 0;
  if (em_pe(a)) {
    topo = FRENTE_TOPO_A;
    desenhar_faixa_frente(d, a, c);
  }

  // The conversation is the right-hand column (landscape) or the whole area below the
  // strip (portrait); the keyboard, the full width of the area in both cases.
  const area_t r = { x0, a.y + topo, larg, a.altura - topo };
  conversa_area_teclado(&s_c, a);
  conversa_desenhar(&s_c, d, r, TUDO, tab5_mesh_servico_ativo(),
                    tr("sem nó Meshtastic - só consulta"));
}
