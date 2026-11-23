// Console commands - MESHTASTIC: mesh, nos, enviar.
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "freertos/FreeRTOS.h"
#include "idioma.h"
#include "sdkconfig.h"
#include "tab5_mesh_mqtt.hpp"   // mesh_mqtt_no_resumo
#include "tab5_mesh_admin.hpp"  // mesh_cfg_ler (the node's Wi-Fi)
#include "tab5_tela.hpp"
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// MESHTASTIC
// ---------------------------------------------------------------------------

// Compact list with just number and name, to check before sending.
static void mesh_nos(void)
{
  const int n = tab5_mesh_num_nos();
  printf(tr("%d no(s) - use o numero ou o nome curto no 'mesh enviar para'\n"), n);
  for (int i = 0; i < n; ++i) {
    ceder(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }
    printf("  %08lX  %-6.6s  %s\n", (unsigned long)no.num, no.curto,
           no.longo[0] ? no.longo : tr("(sem nome)"));
  }
}

// argv[0] = "enviar". Called by 'mesh enviar ...' and by the 'enviar ...' shortcut.
static void mesh_enviar(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "para")) {
    // "enviar para <number|name> <text>" - DIRECT message.
    //
    // Accepts the number in hexadecimal (as 'mesh nos' prints it) or the short
    // name, because memorising F115C354 is not reasonable. The short name is compared
    // case-insensitively: Meshtastic uses upper case on some nodes and
    // not on others, and getting that wrong would give "node not found" with no explanation.
    if (argc < 4) { mostrar_uso("mesh"); return; }
    uint32_t alvo = 0;
    const char* quem = argv[2];

    char* fim = nullptr;
    const unsigned long v = strtoul(quem, &fim, 16);
    if (fim && *fim == 0 && strlen(quem) >= 6) { alvo = (uint32_t)v; }

    if (alvo == 0) {
      const int n = tab5_mesh_num_nos();
      for (int i = 0; i < n && alvo == 0; ++i) {
        mesh_no_t no;
        if (!tab5_mesh_no(i, &no)) { continue; }
        if (strcasecmp(no.curto, quem) == 0) { alvo = no.num; }
      }
    }
    if (alvo == 0) { printf(tr("no '%s' nao encontrado - veja 'mesh nos'\n"), quem); return; }

    char msg[160];
    juntar(msg, sizeof(msg), argc, argv, 3);
    printf(tab5_mesh_enviar(alvo, 0, msg)
           ? tr("enviado para %08lX\n") : tr("falhou (enlace mudo?) %08lX\n"), (unsigned long)alvo);
    return;
  }
  if (argc < 2) { mostrar_uso("mesh"); return; }
  // "enviar <text>" broadcasts on channel 0 (LongFast).
  char msg[160];
  juntar(msg, sizeof(msg), argc, argv, 1);
  printf(tab5_mesh_enviar(0xFFFFFFFFu, 0, msg) ? tr("enviado\n") : tr("falhou (enlace mudo?)\n"));
}

// TRUNCATION DIAGNOSTICS for Mesh messages. The text here comes out RAW - no
// line wrapping, no bubble clipping, no proportional font in between - and
// with the SIZE IN BYTES alongside, precisely to separate "the data that arrived
// is cut" (capture bug) from "the screen drawing it is cutting it" (drawing
// bug).
//
// JOINS ALL CONVERSATIONS (each lives in its own file on the SD card - see
// tab5_mesh_historico()) because there is no "open conversation" here in the console, only
// on the screen. Sorts by time, most recent first.
static void mesh_msgs(int quantidade)
{
  if (quantidade < 1)   { quantidade = 1; }
  if (quantidade > 200) { quantidade = 200; }   // same ceiling as tab5_mesh_historico()

  const int n_conv = tab5_mesh_num_conversas();
  const int CAP = 200;
  mesh_msg_t* todas = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * CAP, MALLOC_CAP_SPIRAM);
  mesh_msg_t* buf   = (mesh_msg_t*)heap_caps_malloc(sizeof(mesh_msg_t) * CAP, MALLOC_CAP_SPIRAM);
  if (!todas || !buf) {
    printf("%s", tr("sem memoria para juntar o historico\n"));
  } else {
    int n_todas = 0;
    for (int c = 0; c < n_conv && n_todas < CAP; ++c) {
      const int n = tab5_mesh_historico(c, buf, CAP);
      for (int i = 0; i < n && n_todas < CAP; ++i) { todas[n_todas++] = buf[i]; }
    }

    // Insertion sort: manual command, at most a few hundred items.
    for (int i = 1; i < n_todas; ++i) {
      mesh_msg_t chave = todas[i];
      int j = i - 1;
      while (j >= 0 && todas[j].quando < chave.quando) { todas[j + 1] = todas[j]; --j; }
      todas[j + 1] = chave;
    }

    const int mostrar = (quantidade < n_todas) ? quantidade : n_todas;
    printf(tr("%d mensagem(ns) em %d conversa(s), mostrando as %d mais recentes:\n"),
           n_todas, n_conv, mostrar);
    for (int i = 0; i < mostrar; ++i) {
      ceder(i);
      const mesh_msg_t& m = todas[i];
      printf(tr("[%3u bytes] de %08lX (%lus): %s\n"),
             (unsigned)strlen(m.texto), (unsigned long)m.de,
             (unsigned long)m.quando, m.texto);
    }
  }
  if (todas) { heap_caps_free(todas); }
  if (buf)   { heap_caps_free(buf); }
}

// The full record: link, health, each node and the messages.
static void mesh_completo(void)
{
  printf(tr("enlace: %s\n"), tab5_mesh_no_ar() ? tr("no ar") : tr("SEM QUADRO recente"));
  {
    static char r[640];
    mesh_mqtt_no_resumo(r, sizeof(r));
    printf("%s", r);
    mesh_cfg_t c;
    mesh_cfg_ler(&c);
    if (c.wifi_conhecido) { printf(tr("Wi-Fi do no: %s, rede \"%s\" (sem Wi-Fi o MQTT do no nao sai)\n"), c.wifi_ligado ? tr("LIGADO") : tr("desligado"), c.wifi_ssid); }
    else                  { printf("%s", tr("Wi-Fi do no: ainda nao veio no dump\n")); }
    // The stamp on THIS node's messages: without it, a public server gateway
    // discards what it sends (MESH_CFG_OK_MQTT).
    if (c.conhecido[MESH_CFG_OK_MQTT]) {
      printf(tr("OK para MQTT: %s\n"), c.v[MESH_CFG_OK_MQTT] ? tr("sim (outros gateways levam as mensagens deste no ao servidor)")
                                                         : tr("NAO (gateway de servidor publico descarta as mensagens deste no)"));
    }
  }
  printf(tr("meu no: %08lX   firmware do C6L: %s\n"),
         (unsigned long)tab5_mesh_meu_numero(),
         tab5_mesh_firmware()[0] ? tab5_mesh_firmware() : tr("(nao veio)"));

  mesh_saude_t sa; tab5_mesh_saude(&sa);
  printf(tr("canal %.1f%%  airtime %.2f%%  uptime %lus  nos %u/%u\n"),
         sa.uso_canal, sa.airtime_tx, (unsigned long)sa.uptime_s,
         (unsigned)sa.nos_online, (unsigned)sa.nos_total);

  // FULL RECORD FOR EACH NODE, and not the summary line. On screen space rules;
  // here there is no such pressure, and this is where "which device am I
  // talking to" is answered. Long and short names are labels the owner changes when
  // they like; id, MAC and model are the device.
  const int n = tab5_mesh_num_nos();
  printf(tr("\n%d no(s) (%lu despejo(s) da tabela desde o boot - ver achar_no):\n"),
         n, (unsigned long)tab5_mesh_despejos());
  for (int i = 0; i < n; ++i) {
    ceder(i);
    mesh_no_t no;
    if (!tab5_mesh_no(i, &no)) { continue; }

    const char* hw = mesh_hw_nome(no.hw);
    char hwtxt[20];
    if (!hw) { snprintf(hwtxt, sizeof(hwtxt), "modelo %u", (unsigned)no.hw); hw = hwtxt; }

    printf("  %-20.20s [%s]\n", no.longo[0] ? no.longo : tr("(sem nome)"), no.curto);
    printf(tr("      num %08lX   id %-10s   %s\n"),
           (unsigned long)no.num, no.id[0] ? no.id : tr("(nao veio)"), hw);

    // MAC only when it SENT it. The field is marked obsolete in the protobuf and
    // new firmware tends to omit it - printing 00:00:00:00:00:00 there would be
    // presenting the absence as if it were an address.
    if (no.tem_mac) {
      printf(tr("      mac %02X:%02X:%02X:%02X:%02X:%02X\n"),
             no.mac[0], no.mac[1], no.mac[2], no.mac[3], no.mac[4], no.mac[5]);
    } else {
      printf("%s", tr("      mac (nao enviado - campo obsoleto no protobuf; o id vem dele)\n"));
    }

    // saltos == 255 is the "unknown" sentinel (MESH_SALTOS_DESCONHECIDO)
    // - printing the raw number would look like real data instead of "don't know yet".
    char saltos_txt[8];
    if (no.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(saltos_txt, sizeof(saltos_txt), "?"); }
    else { snprintf(saltos_txt, sizeof(saltos_txt), "%u", (unsigned)no.saltos); }
    printf(tr("      snr %.1f  bat %u%%  saltos %s%s"),
           no.snr, (unsigned)no.bateria, saltos_txt, no.via_mqtt ? tr("  (via MQTT)") : "");
    if (no.tem_pos) { printf(tr("  pos %.5f,%.5f"), no.lat_i / 1e7, no.lon_i / 1e7); }
    printf("\n");
  }

  uint32_t q, pk, tel, ls, tx, ni; int var;
  tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
  printf(tr("\nrecebidos: %lu quadros, %lu pacotes, %lu nodeinfo, %lu telemetria "
         "(%lu localstats), %lu texto | ultima variante de Telemetry: %d\n"),
         (unsigned long)q, (unsigned long)pk, (unsigned long)ni, (unsigned long)tel,
         (unsigned long)ls, (unsigned long)tx, var);
  // Route quality thermometer: see tab5_mesh_routing_extra(). High is NOT an
  // error - it is "someone heard" arriving more easily than the radio's confirmation.
  printf(tr("routing extra apos confirmado: %lu (nao muda o estado, so o mostra)\n"),
         (unsigned long)tab5_mesh_routing_extra());

  const int m = tab5_mesh_num_msgs();
  printf(tr("\n%d mensagem(ns), %d nao lida(s):\n"), m, tab5_mesh_nao_lidas());
  for (int i = 0; i < m; ++i) {
    ceder(i);
    mesh_msg_t ms;
    if (!tab5_mesh_msg(i, &ms)) { continue; }
    // Delivery state only makes sense on OURS: a received message has nothing
    // to confirm. The failure reason comes from the radio's Routing.Error.
    char marca[40] = "";
    switch (ms.envio) {
      case MESH_ENVIANDO:     snprintf(marca, sizeof(marca), " [enviando]"); break;
      case MESH_ENTREGUE:     snprintf(marca, sizeof(marca), " [entregue]"); break;
      case MESH_SEM_RESPOSTA: snprintf(marca, sizeof(marca), tr(" [sem resposta]")); break;
      case MESH_FALHOU:       snprintf(marca, sizeof(marca), tr(" [FALHOU: %s]"),
                                       mesh_erro_nome(ms.erro)); break;
      default: break;
    }
    // RAW LINK FOR THIS MESSAGE (via_mqtt/hops of the MeshPacket itself, BEFORE
    // the fallback the screen does to the node's link - see tab5_mesh_no_enlace).
    char enlace[24];
    if (ms.via_mqtt) { snprintf(enlace, sizeof(enlace), "mqtt"); }
    else if (ms.saltos == MESH_SALTOS_DESCONHECIDO) { snprintf(enlace, sizeof(enlace), "saltos ?"); }
    else { snprintf(enlace, sizeof(enlace), "%u salto(s)", (unsigned)ms.saltos); }
    printf(tr("  de %08lX (snr %.1f, %s): %s%s\n"),
           (unsigned long)ms.de, ms.snr, enlace, ms.texto, marca);
  }
}

void c_mesh(int argc, char** argv)
{
  if (argc < 2) { mesh_completo(); return; }
  if (eh(argv[1], "no")) { mesh_admin_despejar(); return; }   // the node's raw configuration, field by field
  if (eh(argv[1], "grove")) { static char g[400]; tab5_mesh_grove_diag(g, sizeof(g)); printf("%s", g); return; }
  if (eh(argv[1], "rastro")) {
    const bool on = argc >= 3 && eh(argv[2], "on");
    tab5_mesh_rastro(on);
    printf(tr("rastro do Grove %s\n"), on ? tr("LIGADO (cada quadro no log, tag grove)") : tr("desligado"));
    return;
  }
  const char* o = argv[1];
  if (eh(o, "nos"))    { mesh_nos(); return; }
  if (eh(o, "msgs"))   { mesh_msgs(argc >= 3 ? atoi(argv[2]) : 20); return; }
  if (eh(o, "enviar")) { mesh_enviar(argc - 1, argv + 1); return; }
  if (eh(o, "casa")) {
    // REFERENCE POSITION for the conversation to show distance/direction to a
    // received location - see achar_coord() in app_mesh.cpp. It lives in
    // ajustes.cpp (NVS), not in Mesh: it belongs to the user, not the service.
    if (argc < 4) {
      float lat, lon;
      if (ajustes_casa(&lat, &lon)) { printf(tr("casa: %.6f, %.6f\n"), (double)lat, (double)lon); }
      else { printf("%s", tr("casa: nao configurada. uso: mesh casa <latitude> <longitude>\n")); }
      return;
    }
    const float lat = strtof(argv[2], nullptr);
    const float lon = strtof(argv[3], nullptr);
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
      printf("%s", tr("fora da faixa (latitude -90..90, longitude -180..180)\n"));
    } else {
      ajustes_definir_casa(lat, lon);
      printf(tr("casa gravada: %.6f, %.6f\n"), (double)lat, (double)lon);
    }
    return;
  }
  if (eh(o, "pausar")) {
    // STOPS TALKING TO THE RADIO WITHOUT TURNING ANYTHING OFF - the UART stops, the Grove is
    // returned, but the 5V rail stays on and the module stays on air.
    // It isolates one question: does the instability come from what M5tabOS sends over the
    // serial port, or from the module's POSITION? With this it stays where it was, and only the
    // traffic stops.
    if (tab5_mesh_parar()) {
      printf("%s", tr("servico da malha parado - Grove liberado, mas o 5V continua ligado\n"
             "e o modulo continua no ar (o BLE dele nao e afetado por isto).\n"
             "use 'mesh retomar' para o M5tabOS voltar a conversar com ele.\n"));
    } else {
      printf("%s", tr("nao consegui parar agora (a tarefa nao confirmou em 1s) - tente de novo\n"));
    }
    return;
  }
  if (eh(o, "retomar")) {
    tab5_mesh_iniciar();
    printf("%s", tr("pedido de retomada enviado - rode 'mesh' em alguns segundos para conferir.\n"));
    return;
  }
  mostrar_uso("mesh");
}

// Old shortcuts: 'nos' and 'enviar ...' without 'mesh' in front.
void c_nos(int, char**)               { mesh_nos(); }
void c_enviar(int argc, char** argv)  { mesh_enviar(argc, argv); }
