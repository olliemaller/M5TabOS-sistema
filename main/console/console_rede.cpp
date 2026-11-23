// Comandos do console - REDE: wifi, ble, espnow, c6, farejo, radio, ping, ip/dns/arp/traceroute/tcp/http, busca, ssh, web.
// (Partido de console.cpp; a tabela COMANDOS e a ajuda continuam la.)

#include "tab5_wifi_cochilo.hpp"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "tab5_espnow.hpp"      // sistema_diag_toque
#include "tab5_hora.hpp"        // tab5_hora_local
#include "tab5_arquivos.hpp"
#include "tab5_rede.hpp"
#include "tab5_ssh.hpp"
#include "tab5_web.hpp"
#include "tab5_farejo.hpp"
#include "tab5_csi.hpp"
#include "tab5_wifi.hpp"
#include "esp_hosted.h"
#include "esp_random.h"
#include "driver/sdmmc_host.h"   // sdmmc_host_get_state (c6 laco)
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "ajustes.h"
#include "tab5_c6_sono.hpp"
#include "tab5_ble.hpp"
#include "tab5_mesh_ble.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// RECONHECIMENTO DO C6
// ---------------------------------------------------------------------------
// SOMENTE LEITURA. Nao muda nada no coprocessador - so pergunta quem ele e.
//
// Existe por causa de uma linha que aparece no boot desde sempre:
//
//   esp-hosted fw versions: host=3.0.6 coprocessor=0.0.0
//
// Um coprocessador que se declara 0.0.0 ou nao preenche o campo, ou nao e o
// firmware esp_hosted de referencia. Isso importa por dois motivos concretos:
//
//   1. E o suspeito do RSSI=0 que o app Ajustes ja trata como "indisponivel";
//   2. E decide se da para rodar ESP-NOW no C6. O ESP-NOW nao existe pelo
//      esp_hosted (esp_now_init nao linka neste alvo - conferido), entao a
//      unica via seria firmware proprio no C6 usando o canal peer_data. Antes
//      de cogitar substituir o firmware que HOJE faz o WiFi funcionar, e
//      preciso saber o que esta la.
//
// O app descriptor traz nome do projeto, versao, IDF usado e data da
// compilacao - e o suficiente para dizer se e um build da M5Stack ou o
// esp_hosted de fabrica.
static void cmd_c6(void)
{
  if (!tab5_wifi_c6_enlace()) {
    printf("%s", tr("o enlace com o C6 nao esta de pe.\n"
           "ligue o WiFi no app Ajustes (ou o ajuste) e tente de novo -\n"
           "sem esp_hosted_init nao ha a quem perguntar.\n"));
    return;
  }

  printf("%s", tr("--- coprocessador C6 ---\n"));

  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) == ESP_OK) {
    printf(tr("  firmware   : %d.%d.%d  (rev %d, build %d)\n"),
           (int)v.major1, (int)v.minor1, (int)v.patch1,
           (int)v.revision, (int)v.build);
  } else {
    printf("%s", tr("  firmware   : nao respondeu\n"));
  }

  // SO A VERSAO VIAJA.
  //
  // O esp_hosted_app_desc_t tem project_name, date, time e idf_ver, e a
  // primeira versao deste comando imprimia os quatro. Vem vazios, e o
  // proprio header diz por que:
  //
  //   /* App descriptor; wire carries version + idf_target only. */
  //
  // Os campos existem no struct porque ele espelha o esp_app_desc_t do IDF,
  // mas o RPC nao transporta o resto. Imprimir campo que nunca chega e pior que
  // nao imprimir: parece defeito do coprocessador.
  esp_hosted_app_desc_t d = { };
  if (eh_host_sys_get_cp_app_desc(&d) == ESP_OK) {
    printf(tr("  versao app : %.*s\n"), (int)sizeof(d.version), d.version);
  } else {
    printf("%s", tr("  versao app : nao respondeu\n"));
  }


  // O MAC e a IDENTIDADE no ESP-NOW - la nao existe nome, so endereco de 6
  // bytes. Se um dia o ESP-NOW rodar no C6, e este numero que o outro aparelho
  // vai cadastrar como par.
  uint8_t mac[6] = { };
  if (eh_host_sys_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    printf(tr("  MAC (STA)  : %02X:%02X:%02X:%02X:%02X:%02X\n"),
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  if (eh_host_sys_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
    printf(tr("  MAC (AP)   : %02X:%02X:%02X:%02X:%02X:%02X\n"),
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // Aviso por ULTIMO, depois dos dados. Ele estava no meio do relatorio e
  // partia a lista em duas - os MACs apareciam depois de um bloco de alerta,
  // como se fossem parte dele.
  //
  // A tabela do docs/migration.md do esp_hosted:
  //   host 3.x + coprocessador 3.x    Suportado
  //   host 3.x + coprocessador 1.x    Retrocompativel, atualizacao RECOMENDADA
  //   host < 3.x + coprocessador 3.x  Nao suportado
  if (v.major1 != 0 && v.major1 < 3) {
    printf(tr("\n  ATENCAO: host 3.x com coprocessador %d.x.\n"
           "  Combinacao retrocompativel, mas o esp_hosted RECOMENDA atualizar\n"
           "  o coprocessador. E o principal suspeito do RSSI=0 que o app Ajustes ja\n"
           "  trata como indisponivel.\n"), (int)v.major1);
  }
}

// ---------------------------------------------------------------------------
// wifi gravar <ssid> [senha]
// ---------------------------------------------------------------------------
static int wifi_gravar(const char* ssid, const char* senha_ou_null)
{
  const char* senha = senha_ou_null ? senha_ou_null : "-";   // "-" = rede aberta

  return com_cartao([&]() -> int {
    // tab5_wifi_gravar_credenciais() cifra a senha antes de escrever (AES-128-CBC,
    // ver tab5_wifi.cpp) e ja atualiza a copia em RAM tambem - reler o arquivo
    // depois, com o C6 no ar, e justamente o que nao pode acontecer.
    if (!tab5_wifi_gravar_credenciais(ssid, senha))
    {
      printf(tr("erro: nao consegui gravar %s\n"), TAB5_WIFI_ARQUIVO);
      return 1;
    }
    printf(tr("gravado em %s (senha cifrada no arquivo)\n  SSID:  %s\n  senha: %s\n"),
           TAB5_WIFI_ARQUIVO, ssid,
           strcmp(senha, "-") == 0 ? tr("(rede aberta)") : tr("definida"));
    printf(tr("para usar agora: wifi conectar \"%s\"\n"), ssid);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// REDE
// ---------------------------------------------------------------------------

void c_wifi(int argc, char** argv)
{
  if (argc < 2) {
    printf(tr("wifi     : %s\n"), tab5_wifi_descricao());
    if (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
      const int r = (int)tab5_wifi_rssi();
      printf(tr("rede     : %s   IP %s   "), tab5_wifi_ssid(), tab5_wifi_ip());
      if (r == TAB5_WIFI_RSSI_DESCONHECIDO) { printf("%s", tr("sinal --\n")); }
      else                                   { printf(tr("sinal %d dBm\n"), r); }
    }
    printf(tr("guardadas: %d rede(s) no wifi.txt%s\n"), tab5_wifi_n_conhecidas(),
           tab5_wifi_so_radio() ? tr("   (radio ligado so para ESP-NOW)") : "");
    // O modo de economia que o C6 diz estar usando AGORA (RPC) - nao o que o
    // P4 pediu. Foi o que faltou para entender os 28 mA do Wi-Fi em repouso.
    if (tab5_wifi_pilha_de_pe()) {
      wifi_ps_type_t ps;
      if (esp_wifi_get_ps(&ps) == ESP_OK) {
        printf(tr("economia : %s (no C6, agora)\n"), ps == WIFI_PS_NONE ? tr("nenhuma - receptor sempre ligado")
                                               : ps == WIFI_PS_MIN_MODEM ? tr("modem sleep minimo") : tr("modem sleep maximo"));
      }
    }
    {
      char c[96];
      tab5_wifi_cochilo_resumo(c, sizeof(c));
      printf(tr("repouso  : %s\n"), c);
    }
    printf("%s", tr("\n'help wifi' mostra como varrer, gravar, conectar e esquecer redes\n"));
    return;
  }
  const char* o = argv[1];
  // Ligar/desligar o Wi-Fi como no Ajustes (grava a vontade e age).
  // Uma rede chamada "ligar" ou "desligar" se grava com 'wifi gravar'.
  if (eh(o, "ligar") || eh(o, "desligar")) {
    const bool ligar = eh(o, "ligar");
    sistema_wifi(ligar);
    printf("%s", ligar ? tr("ligando o Wi-Fi - confira com 'wifi' em alguns segundos\n")
                       : tr("desligando o Wi-Fi\n"));
    return;
  }
  if (eh(o, "redes")) {
    // Exige a pilha de pe: sem esp_hosted no ar nao ha radio a quem pedir.
    if (!tab5_wifi_pilha_de_pe()) {
      printf("%s", tr("o WiFi ainda nao foi ligado neste boot - ligue em Ajustes e tente de novo\n"));
      return;
    }
    // A MESMA PROCURA DE Ajustes > Wi-Fi (hal/tab5_wifi): conectado, procura
    // sem soltar a rede. O resultado e lido e impresso AQUI, na tarefa de quem
    // digitou - antes a tarefa do Wi-Fi imprimia, e no Terminal nada aparecia.
    const uint32_t v0 = tab5_wifi_redes_versao();
    tab5_wifi_procurar();
    printf("%s", tr("procurando (a rede atual continua conectada)...\n"));
    fflush(stdout);
    for (int i = 0; i < 150 && !console_interrompido(); ++i) {
      vTaskDelay(pdMS_TO_TICKS(100));
      if (!tab5_wifi_procurando() && tab5_wifi_redes_versao() - v0 >= 2) { break; }
    }
    if (tab5_wifi_procurando()) { printf("%s", tr("a procura ainda nao terminou (conectando?) - tente de novo\n")); return; }
    const int n = tab5_wifi_n_vistas();
    if (!n) {
      printf("%s", tr("NENHUMA rede encontrada.\n"
             "  Se ha redes 2.4GHz por perto, o problema e o radio do C6, nao a sua rede.\n"));
      return;
    }
    printf(tr("%d rede(s), do sinal mais forte ao mais fraco (* = guardada):\n"), n);
    printf("  %-32s %5s %3s %s\n", tr("SSID"), tr("dBm"), tr("ch"), tr("seguranca"));
    for (int i = 0; i < n; ++i) {
      tab5_wifi_visto_t v;
      if (!tab5_wifi_vista(i, &v)) { continue; }
      printf("%c %-32s %5d %3d %s\n", v.conhecida ? '*' : ' ', v.ssid, (int)v.rssi, (int)v.canal,
             v.aberta ? tr("aberta") : tr("com senha"));
    }
    printf("%s", tr("\nO Tab5 so enxerga 2.4GHz (o radio e um ESP32-C6). Se a sua rede nao esta\n"
           "na lista, ela e 5GHz ou o nome esta diferente (maiuscula conta).\n"));
    return;
  }
  if (eh(o, "conectar")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    if (!tab5_wifi_conhecida(argv[2])) {
      printf(tr("'%s' nao esta guardada - use: wifi gravar \"%s\" <senha>\n"), argv[2], argv[2]);
      return;
    }
    // Pedir uma rede tira o ESP-NOW do canal fixo (ver conectar_rede no Ajustes).
    if (tab5_espnow_canal_fixo()) { tab5_espnow_definir_canal(0); }
    tab5_wifi_conectar(argv[2]);
    printf(tr("trocando para '%s' - confira com 'wifi' em alguns segundos\n"), argv[2]);
    return;
  }
  if (eh(o, "esquecer")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    const char* ssid = argv[2];
    com_cartao([&]() -> int {
      const bool ok = tab5_wifi_esquecer(ssid);
      printf(ok ? tr("'%s' esquecida\n") : tr("'%s' nao estava guardada\n"), ssid);
      return ok ? 0 : 1;
    });
    return;
  }
  if (eh(o, "gravar")) {
    if (argc < 3) { mostrar_uso("wifi"); return; }
    wifi_gravar(argv[2], argc >= 4 ? argv[3] : nullptr);
    return;
  }
  // Forma curta, a original: 'wifi <ssid> [senha]'.
  wifi_gravar(argv[1], argc >= 3 ? argv[2] : nullptr);
}

// Atalho antigo: 'redes' = 'wifi redes'.
void c_redes(int, char**)
{
  char a0[] = "wifi", a1[] = "redes";
  char* a[] = { a0, a1 };
  c_wifi(2, a);
}

// BLUETOOTH: estado da stack, do enlace do Mesh e quem foi visto.
void c_ble(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "varrer")) {
    printf(tab5_ble_varrer(6000) ? tr("varrendo por 6 s... rode 'ble' depois\n")
                                 : tr("nao deu para varrer agora (%s)\n"), tab5_ble_descricao());
    return;
  }
  if (argc >= 2 && eh(argv[1], "desligar")) {
    sistema_bluetooth(false);
    printf("%s", tr("desligando - confira com 'ble' em alguns segundos\n"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "ligar")) {
    // Ligar com o C6 desligado pede o trilho (I2C), que so a interface fala.
    if (!tab5_wifi_pilha_de_pe()) { printf("%s", tr("o C6 ainda nao subiu neste boot - ligue em Ajustes\n")); return; }
    sistema_bluetooth(true);
    printf("%s", tr("ligando - confira com 'ble' em alguns segundos\n"));
    return;
  }
  if (argc >= 2) { mostrar_uso("ble"); return; }
  printf(tr("bluetooth: %s\n"), tab5_ble_descricao());
  printf(tr("mesh ble:  %s%s\n"), mesh_ble_descricao(), mesh_ble_esperando_pin() ? tr(" (esperando PIN)") : "");
  const int n = tab5_ble_n_vistos();
  if (n == 0) { printf("%s", tr("nenhum aparelho visto ainda - 'ble varrer' procura por 6 s\n")); }
  tab5_ble_visto_t v;
  for (int i = 0; i < n; ++i) {
    ceder(i);
    if (!tab5_ble_visto(i, &v)) { continue; }
    char end[18];
    tab5_ble_endereco_txt(v.addr, end);
    printf(tr("  %s  %4d dBm  %-22s%s\n"), end, (int)v.rssi, v.nome[0] ? v.nome : tr("(sem nome)"),
           v.meshtastic ? tr("  MESHTASTIC") : "");
  }
}

// O estado real do ESP-NOW e do radio - sem depender da tela. O canal e
// perguntado AO C6 agora (nao o que o servico acha), que e o que decide se
// dois aparelhos se ouvem.
void c_espnow(int argc, char** argv)
{
  if (argc >= 3 && eh(argv[1], "canal")) {
    const int c = atoi(argv[2]);
    printf(tab5_espnow_definir_canal((uint8_t)c) ? tr("pedido: canal %d\n") : tr("recusado (canal %d)\n"), c);
    return;
  }
  if (argc >= 3 && eh(argv[1], "enviar")) {
    // A frase inteira: antes so a primeira palavra ia, o resto sumia calado.
    char msg[200];
    juntar(msg, sizeof(msg), argc, argv, 2);
    printf(tab5_espnow_enviar(msg) ? tr("na fila: '%s'\n") : tr("recusado: '%s'\n"), msg);
    return;
  }
  if (argc >= 4 && eh(argv[1], "para")) {
    uint8_t dest[6];
    if (!tab5_espnow_mac_de_texto(argv[2], dest)) { mostrar_uso("espnow"); return; }
    char msg[200];
    juntar(msg, sizeof(msg), argc, argv, 3);
    printf(tab5_espnow_enviar_para(dest, msg) ? tr("na fila: '%s'\n") : tr("recusado: '%s'\n"), msg);
    return;
  }
  if (argc >= 3 && eh(argv[1], "todos")) {
    // "todos <MAC> <texto>" direto; "todos <texto>" difunde. Um MAC sozinho
    // (sem texto depois) e tratado como texto.
    uint8_t dest[6];
    const bool direto = argc >= 4 && tab5_espnow_mac_de_texto(argv[2], dest);
    char msg[200];
    juntar(msg, sizeof(msg), argc, argv, direto ? 3 : 2);
    printf(tab5_espnow_enviar_todos_canais(direto ? dest : nullptr, msg)
               ? tr("na fila, varrendo os canais 1-13 (o Wi-Fi cai por alguns segundos): '%s'\n")
               : tr("recusado: '%s'\n"), msg);
    return;
  }
  if (argc >= 2 && eh(argv[1], "msgs")) {
    // As ultimas n (padrao 10), da mais antiga para a mais nova: hora, quem
    // (seta + MAC), os bytes em hexa e o texto legivel (fora de 0x20-0x7E = '.').
    int quer = argc >= 3 ? atoi(argv[2]) : 10;
    const int total = tab5_espnow_n();
    if (quer < 1) { quer = 10; }
    if (quer > total) { quer = total; }
    if (quer == 0) { printf("%s", tr("nenhuma mensagem guardada\n")); return; }
    for (int i = total - quer; i < total; ++i) {
      ceder(i);
      espnow_msg_t m;
      if (!tab5_espnow_msg(i, &m)) { continue; }
      char hora[12] = "--:--:--";
      if (m.quando) {
        struct tm t;
        tab5_hora_local(m.quando, &t);
        snprintf(hora, sizeof(hora), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
      }
      printf("%s %s %02X:%02X:%02X:%02X:%02X:%02X  ", hora, m.minha ? "->" : "<-",
             m.mac[0], m.mac[1], m.mac[2], m.mac[3], m.mac[4], m.mac[5]);
      for (int k = 0; k < m.n; ++k) { printf("%02X ", m.bruto[k]); }
      printf(" |");
      for (int k = 0; k < m.n; ++k) { putchar(m.bruto[k] >= 0x20 && m.bruto[k] <= 0x7E ? m.bruto[k] : '.'); }
      printf("|\n");
    }
    return;
  }
  if (argc >= 2) { mostrar_uso("espnow"); return; }
  uint8_t mac[6]; tab5_espnow_meu_mac(mac);
  uint8_t c = 0; wifi_second_chan_t sec;
  const bool leu = tab5_wifi_pilha_de_pe() && esp_wifi_get_channel(&c, &sec) == ESP_OK;
  printf(tr("C6 firmware nosso: %s   enlace: %s   ESP-NOW no ar: %s\n"),
         tab5_espnow_c6_compativel() ? tr("sim") : tr("NAO"), tab5_wifi_c6_enlace() ? tr("sim") : tr("NAO"),
         tab5_espnow_pronto() ? tr("sim") : tr("NAO"));
  printf(tr("WiFi: %s  (so radio: %s)\n"), tab5_wifi_descricao(), tab5_wifi_so_radio() ? tr("sim") : tr("nao"));
  printf(tr("canal no C6 agora: %s%u   pedido: %s%u\n"), leu ? "" : "? ", (unsigned)c,
         tab5_espnow_canal_fixo() ? tr("fixo ") : tr("roteador "), (unsigned)tab5_espnow_canal_fixo());
  printf(tr("meu MAC: %02X:%02X:%02X:%02X:%02X:%02X   msgs: %d   ouvidos: %d\n"),
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], tab5_espnow_n(), tab5_espnow_vistos());
}

// GRAVAR O C6 PELO ENLACE ESTA DESLIGADO. A OTA pelo proprio SDIO nao tem
// volta quando a imagem nova nao fala - foi o que ja deixou o C6 mudo. Com o
// M5Stack ESP32 Downloader, gravar e restaurar e FISICO e sempre tem volta. As
// funcoes antigas (cmd_c6_ota/cmd_c6_gravar) estao no historico do git.
// ---------------------------------------------------------------------------
// GRAVAR O C6 PELO ENLACE (OTA pelo SDIO)
// ---------------------------------------------------------------------------
// So existe com o firmware do C6 da tabela nova (bootloader com rollback, dois
// slots de 1,875 MB, GUARDA DE IMAGEM NOVA no c6/main/app_main.c). O caminho:
//
//   1. le apps/Sistema/tab5_c6.bin INTEIRO para a PSRAM e confere antes de
//      tocar no C6: e imagem do IDF, e para ESP32-C6, e e o projeto tab5_c6;
//   2. begin (apaga o slot LIVRE) -> write em pedacos de 1536 -> end (o C6
//      valida a imagem inteira) -> activate (troca o boot; reinicia em 2 s).
//      Erro antes do activate: nada mudou, o firmware atual segue rodando;
//   3. o P4 se reinicia 4 s depois. No boot ele reseta o C6 (2o boot da
//      imagem nova - a guarda conta e tolera) e manda o VIVO, que a confirma.
//
// Se o VIVO nao vier (enlace nao fecha, imagem trava em laco), o proprio C6
// volta para a imagem anterior: 120 s, ou no 5o boot sem confirmacao.
#define C6_FW_APP     "Sistema"
#define C6_FW_ARQUIVO "tab5_c6.bin"
#define C6_PEDACO     1536          // EH_RPC_OTA_CHUNK_MAX
#define C6_FW_MAX     0x1E0000      // slot do c6/partitions.csv

static void cmd_c6_gravar(bool confirmado)
{
  if (!tab5_wifi_c6_enlace()) {
    printf("%s", tr("o enlace com o C6 nao esta de pe - ligue o WiFi primeiro.\n"));
    return;
  }
  if (!confirmado) {
    printf(tr("--- gravar o C6 pelo enlace ---\n"
           "1. mande o arquivo para o cartao (do Mac, monitor fechado):\n"
           "     receber %s %s <bytes>\n"
           "2. c6 gravar sim\n"
           "\n"
           "O C6 reinicia e o P4 reinicia logo depois, sozinho. Se a imagem\n"
           "nova nao for confirmada, o C6 volta para a anterior sem ajuda.\n"
           "NAO desligue o aparelho ate o P4 voltar.\n"),
           C6_FW_APP, C6_FW_ARQUIVO);
    return;
  }

  // RPC vivo? Sem ele, cada pedido da OTA esperaria o tempo esgotado.
  eh_host_coprocessor_fwver_t v = { };
  if (eh_host_sys_get_cp_fw_version(&v) != ESP_OK) {
    printf("%s", tr("o C6 aceita o enlace mas nao responde a RPC - so pelo Downloader.\n"));
    return;
  }

  if (!tab5_sd_montar()) { printf(tr("cartao nao montou (%s)\n"), tab5_sd_descricao()); return; }
  FILE* f = tab5_sd_abrir(C6_FW_APP, C6_FW_ARQUIVO, "rb");
  if (!f) { printf(tr("nao achei apps/%s/%s\n"), C6_FW_APP, C6_FW_ARQUIVO); tab5_sd_desmontar(); return; }
  const long total = tab5_sd_tamanho(f);
  uint8_t* img = (total > 0 && total <= C6_FW_MAX)
                   ? (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM) : nullptr;
  const size_t lidos = img ? fread(img, 1, (size_t)total, f) : 0;
  fclose(f);
  tab5_sd_desmontar();
  if (!img || lidos != (size_t)total) {
    printf(tr("tamanho %ld invalido (max %u) ou leitura curta (%u)\n"), total, (unsigned)C6_FW_MAX, (unsigned)lidos);
    heap_caps_free(img);
    return;
  }

  // CONFERENCIA ANTES DE TOCAR NO C6. Cabecalho do IDF: 0xE9 no byte 0,
  // chip_id no 12 (0x000D = ESP32-C6). Logo depois do cabecalho (24 B) e do
  // 1o segmento (8 B) vem o esp_app_desc_t: magic 0xABCD5432, versao no +16,
  // nome do projeto no +48.
  const uint32_t magic_desc = (uint32_t)img[32] | ((uint32_t)img[33] << 8) |
                              ((uint32_t)img[34] << 16) | ((uint32_t)img[35] << 24);
  char versao[33] = { 0 }, projeto[33] = { 0 };
  if (total > 112) { memcpy(versao, img + 48, 32); memcpy(projeto, img + 80, 32); }
  const unsigned chip = (unsigned)img[12] | ((unsigned)img[13] << 8);
  if (total <= 112 || img[0] != 0xE9 || chip != 0x000D || magic_desc != 0xABCD5432u
      || strcmp(projeto, "tab5_c6") != 0) {
    printf(tr("%s nao e firmware do C6 deste projeto (magic 0x%02X, chip %u, projeto '%s')\n"),
           C6_FW_ARQUIVO, img[0], chip, projeto);
    heap_caps_free(img);
    return;
  }
  printf(tr("imagem %s, %ld bytes - gravando no slot livre do C6\n"), versao, total);

  esp_err_t r = eh_host_cp_ota_begin();
  if (r != ESP_OK) {
    printf(tr("begin falhou: %s - nada mudou\n"), esp_err_to_name(r));
    heap_caps_free(img);
    return;
  }
  int pct_ant = -1;
  for (long feito = 0; feito < total; ) {
    const uint32_t n = (uint32_t)((total - feito) > C6_PEDACO ? C6_PEDACO : (total - feito));
    r = eh_host_cp_ota_write(img + feito, n);
    if (r != ESP_OK) {
      printf(tr("\nwrite falhou em %ld/%ld: %s - ABORTADO, o firmware atual segue\n"),
             feito, total, esp_err_to_name(r));
      heap_caps_free(img);
      return;
    }
    feito += n;
    const int pct = (int)(feito * 100 / total);
    if (pct / 10 != pct_ant) { printf("  %d%%\n", pct); pct_ant = pct / 10; }
    ceder((int)(feito / C6_PEDACO));
  }
  heap_caps_free(img);

  r = eh_host_cp_ota_end();
  if (r != ESP_OK) {
    printf(tr("end falhou: %s - o C6 recusou a imagem, nada mudou\n"), esp_err_to_name(r));
    return;
  }
  r = eh_host_cp_ota_activate();
  if (r != ESP_OK) {
    printf(tr("activate falhou: %s - a imagem esta no slot mas o boot nao mudou\n"), esp_err_to_name(r));
    return;
  }
  printf("%s", tr("gravado e ativado. O C6 reinicia em 2 s; o P4 reinicia em 4 s para\n"
         "confirmar a imagem nova. Depois: 'farejo info'.\n"));
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(4000));
  esp_restart();
}

// c6 dormir | acordar | sono - a prova do C6 dormindo associado (hal/tab5_c6_sono).
#if CONFIG_ESP_HOSTED_HOST_PKT_STATS
// Os contadores do transporte do esp_hosted (mesma forma de eh_host_pkt_stats.h,
// como em hal/tab5_wifi.cpp).
struct pkt_stats_espelho_t { uint32_t tx_in, tx_trans, tx_out, tx_drop, rx_in, rx_out, stalls; };
extern "C" pkt_stats_espelho_t pkt_stats;
#endif
static void c6_sono_mostrar(void)
{
  tab5_c6_sono_estado_t e;
  tab5_c6_sono_estado(&e);
  static const char* const N[] = { "awake (link up)", "ASLEEP (link down)", "FAILED - turn Wi-Fi off and on" };
  printf(tr("enlace: %s\n"), N[e.estado]);
  printf(tr("  desmontado %lu vez(es); voltou %lu pelo P4, %lu pelo C6; %lu falha(s)\n"),
         (unsigned long)e.entradas, (unsigned long)e.pelo_p4, (unsigned long)e.pelo_c6, (unsigned long)e.falhas);
  printf(tr("  ultima remontagem %lu ms; tempo desmontado %lu s\n"),
         (unsigned long)e.ultimo_religar_ms, (unsigned long)(e.dormindo_ms / 1000));
#if CONFIG_ESP_HOSTED_HOST_PKT_STATS
  {
    printf(tr("  pacotes STA no P4: tx aceitos %lu, enviados %lu, perdidos %lu | rx lidos %lu, entregues %lu\n"),
           (unsigned long)pkt_stats.tx_in, (unsigned long)pkt_stats.tx_out, (unsigned long)pkt_stats.tx_drop,
           (unsigned long)pkt_stats.rx_in, (unsigned long)pkt_stats.rx_out);
  }
#endif
  {
    uint32_t a, i, o, f;
    tab5_c6_sono_rx(&a, &i, &o, &f);
    printf(tr("  rx no P4 por tipo: ARP %lu, ICMP %lu, outro %lu, recusados pela pilha %lu\n"),
           (unsigned long)a, (unsigned long)i, (unsigned long)o, (unsigned long)f);
  }
  if (e.estado != TAB5_C6_SONO_ACORDADO) { return; }
  c6_info_t in = { };
  if (!tab5_farejo_info_c6(&in, 1500)) { printf("%s", tr("  (o C6 nao respondeu ao INFO)\n")); return; }
  static const char* const S[] = { "not supported (old firmware)", "awake", "asleep" };
  printf(tr("  C6: %s; light sleep %lu vez(es), %lu s dormindo de %lu s no ar; %u entrada(s), %u despertar(es)\n"),
         S[in.sono_estado < 3 ? in.sono_estado : 0], (unsigned long)in.sono_n, (unsigned long)(in.sono_ms / 1000),
         (unsigned long)in.no_ar_s, (unsigned)in.sono_entradas, (unsigned)in.sono_despertares);
  printf(tr("  C6 -> Wi-Fi: %lu quadro(s) do P4, %lu erro(s) (ultimo %d); estacao %s\n"),
         (unsigned long)in.tx_wifi_n, (unsigned long)in.tx_wifi_erros, (int)in.tx_wifi_ultimo_err,
         in.sta_conectada ? tr("associada") : tr("NAO associada"));
  printf(tr("  filtro do C6 (P4 dormindo): %lu ARP respondido(s), %lu descartado(s), %lu quadro(s) acordaram o P4\n"),
         (unsigned long)in.filtro_arp, (unsigned long)in.filtro_descartes, (unsigned long)in.filtro_acordou);
}

static SemaphoreHandle_t s_c6_pm_sem = nullptr;
static char              s_c6_pm_txt[2400];
static void c6_pm_rx(uint32_t, const uint8_t* d, size_t n, void*)
{
  if (n >= sizeof(s_c6_pm_txt)) { n = sizeof(s_c6_pm_txt) - 1; }
  memcpy(s_c6_pm_txt, d, n);
  s_c6_pm_txt[n] = 0;
  if (s_c6_pm_sem) { xSemaphoreGive(s_c6_pm_sem); }
}

void c_c6(int argc, char** argv)
{
  if (argc >= 4 && eh(argv[1], "janela")) {
    // c6 janela <intervalo ms> <janela ms>: quanto o ESP-NOW deixa o radio acordado.
    const int iv = atoi(argv[2]), jn = atoi(argv[3]);
    const uint8_t m[5] = { C6_AJ_ESPNOW_JANELA, (uint8_t)iv, (uint8_t)(iv >> 8), (uint8_t)jn, (uint8_t)(jn >> 8) };
    printf(eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, m, sizeof(m)) == ESP_OK ? tr("janela pedida\n") : tr("nao saiu\n"));
    return;
  }
  if (argc >= 3 && eh(argv[1], "laco")) {
    // TESTE DE BANCADA: 'c6 laco N' - o C6 dorme e acorda N vezes, com esperas
    // sorteadas (50 a 1500 ms) para varrer a fase das corridas da desmontagem
    // e da volta (SDIO lendo na janela do aviso; slot 1 x cartao no mesmo
    // controlador). Rode com trafego chegando (ping do PC) e o cartao em uso.
    // Mostra o controlador SDMMC (slots iniciados) a cada volta: com o cartao
    // montado e o C6 acordado deve dar 2; nunca 0 com alguem usando.
    const int n = atoi(argv[2]);
    int ok = 0, recusou = 0, falhou = 0;
    for (int i = 0; i < n; ++i) {
      ceder(i);
      char m[96];
      if (!tab5_c6_sono_dormir(m, sizeof(m))) { ++recusou; printf(tr("%d: nao dormiu: %s\n"), i + 1, m); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 1450));
      if (!tab5_c6_sono_acordar()) { ++falhou; printf(tr("%d: o enlace NAO voltou - parando\n"), i + 1); break; }
      ++ok;
      sdmmc_host_state_t st = { };
      sdmmc_host_get_state(&st);
      printf(tr("%d/%d ok - controlador SDMMC: %s, %d slot(s)\n"), i + 1, n,
             st.host_initialized ? tr("ligado") : tr("DESLIGADO"), st.num_of_init_slots);
      vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 1450));
    }
    printf(tr("laco: %d ok, %d recusado(s), %d falha(s)\n"), ok, recusou, falhou);
    return;
  }
  if (argc >= 2 && eh(argv[1], "travas")) {
    // Quem impede o light sleep do C6 (a tabela do PM dele, pelo enlace).
    if (!s_c6_pm_sem) { s_c6_pm_sem = xSemaphoreCreateBinary(); eh_host_peer_data_register(C6_MSG_PM, c6_pm_rx, nullptr); }
    xSemaphoreTake(s_c6_pm_sem, 0);
    const uint8_t k = C6_AJ_PM;
    eh_host_peer_data_send(C6_MSG_HOST_AJUSTE, &k, 1);
    if (xSemaphoreTake(s_c6_pm_sem, pdMS_TO_TICKS(1500)) == pdTRUE) { printf("%s\n", s_c6_pm_txt); }
    else { printf("%s", tr("o C6 nao respondeu (firmware antigo?)\n")); }
    return;
  }
  if (argc >= 2 && eh(argv[1], "dormir")) {
    char m[96];
    if (tab5_c6_sono_dormir(m, sizeof(m))) { printf("%s", tr("enlace desmontado: o C6 pode dormir. 'c6 acordar' traz de volta\n")); }
    else                                   { printf(tr("nao: %s\n"), m); }
    return;
  }
  if (argc >= 2 && eh(argv[1], "acordar")) {
    printf(tab5_c6_sono_acordar() ? tr("enlace de pe\n") : tr("o enlace NAO voltou - 'wifi desligar' e 'wifi ligar'\n"));
    c6_sono_mostrar();
    return;
  }
  if (argc >= 4 && eh(argv[1], "sono") && eh(argv[2], "auto")) {
    tab5_c6_sono_automatico(eh(argv[3], "ligar"));
    printf(tr("C6 dormindo no repouso: %s\n"), ajustes_c6_sono() ? tr("LIGADO (tela apagada e quieto: o enlace desmonta)") : tr("desligado"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "sono")) { c6_sono_mostrar(); return; }
  if (argc >= 2 && (eh(argv[1], "ota") || eh(argv[1], "gravar"))) {
    cmd_c6_gravar(argc >= 3 && eh(argv[2], "sim"));
    return;
  }
  cmd_c6();
}

// ---------------------------------------------------------------------------
// FAREJO (modo promiscuo no C6) - e a janela para as capacidades de radio que
// a gravacao do C6 trouxe. 'info' mostra o que este C6 sabe fazer (CSI, FTM,
// 802.15.4): o bit so acende se o modulo foi compilado no firmware gravado.
// ---------------------------------------------------------------------------
static void farejo_estado(void)
{
  printf(tr("C6 sabe farejar: %s   farejo ativo: %s\n"),
         tab5_farejo_c6_suporta() ? tr("sim") : tr("NAO (firmware v<3 ou sem a flag)"),
         tab5_farejo_ativo() ? tr("sim") : tr("nao"));
  const int n = tab5_farejo_n();
  printf(tr("MACs ouvidos: %d   lotes: %lu   perdidos no C6: %lu\n"),
         n, (unsigned long)tab5_farejo_lotes(), (unsigned long)tab5_farejo_perdidos());
  for (int i = 0; i < n && i < 20; ++i) {
    farejo_item_t it;
    if (!tab5_farejo_item(i, &it)) { continue; }
    ceder(i);
    printf(tr("  %02X:%02X:%02X:%02X:%02X:%02X  ch%-2u  rssi %4d (max %4d)  tipos 0x%02X  quadros %lu\n"),
           it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5],
           it.canal, it.rssi_ult, it.rssi_max, it.tipos, (unsigned long)it.quadros);
  }
  if (n > 20) { printf(tr("  ... e mais %d\n"), n - 20); }
}

static void farejo_info(void)
{
  c6_info_t in;
  if (!tab5_farejo_info_c6(&in, 1500)) {
    printf("%s", tr("o C6 nao respondeu (sem enlace, firmware de fabrica, ou v<3).\n"));
    return;
  }
  printf(tr("C6 contrato v%u   canal %u   farejando: %s"), in.proto, in.canal, in.farejando ? tr("sim") : tr("nao"));
  if (in.chip_rev & 0x80) {
    printf(tr("   chip ESP32-C6 v%u.%u"), (unsigned)((in.chip_rev >> 4) & 0x7), (unsigned)(in.chip_rev & 0xF));
  }
  printf("\n");
  printf(tr("capacidades: ESP-NOW %s | farejo %s | cru %s | CSI %s | FTM %s | 802.15.4 %s\n"),
         (in.caps & C6_CAP_ESPNOW) ? "s" : "-", (in.caps & C6_CAP_FAREJO) ? "s" : "-",
         (in.caps & C6_CAP_CRU)    ? "s" : "-", (in.caps & C6_CAP_CSI)    ? "s" : "-",
         (in.caps & C6_CAP_FTM)    ? "s" : "-", (in.caps & C6_CAP_154)    ? "s" : "-");
  printf(tr("heap livre %lu (min %lu)   no ar %lu s   quadros farejados %lu\n"),
         (unsigned long)in.heap_livre, (unsigned long)in.heap_min,
         (unsigned long)in.no_ar_s, (unsigned long)in.quadros);
  // Firmware do C6 m5tabos-c6-3 em diante: um caminho so; csi_modo 1 = ligado.
  // ESP_FAIL aqui = o WiFi do C6 subiu com csi_enable 0 (ver tab5_wifi.cpp).
  printf(tr("CSI: %s (etapa %u: 1 config, 2 callback, 3 ligar)   ligado: %s\n"),
         in.csi_err ? esp_err_to_name(in.csi_err) : tr("OK"), (unsigned)in.csi_etapa,
         in.csi_modo ? tr("sim") : tr("nao"));
  if (in.csi_err == ESP_FAIL) {
    printf("%s", tr("     ESP_FAIL = o WiFi do C6 nasceu com csi_enable 0 (P4 antigo?)\n"));
  }
  static const char* const Z154[] = { "never on", "listening", "asleep" };
  printf(tr("saida do C6: %lu mensagem(ns) descartada(s) por fila cheia\n"), (unsigned long)in.saida_perdas);
  if (in.flags & C6_INFO_F_COREDUMP) { printf("%s", tr("ATENCAO: o C6 guardou um retrato de travamento (coredump)\n")); }
  if (in.flags & C6_INFO_F_EM_PROVA) { printf("%s", tr("imagem do C6 EM PROVA: ainda nao confirmada pelo P4\n")); }
  printf(tr("     callbacks %lu   enviados ao P4 %lu\n"),
         (unsigned long)in.csi_cb, (unsigned long)in.csi_env);
  printf(tr("802.15.4: %s   erro %s   quadros recebidos pelo radio %lu\n"),
         in.z154_estado < 3 ? Z154[in.z154_estado] : "?",
         in.z154_err ? esp_err_to_name(in.z154_err) : tr("nenhum"), (unsigned long)in.z154_rx);
}


void c_farejo(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "info"))  { farejo_info();     return; }
  if (argc >= 2 && eh(argv[1], "off"))   { tab5_farejo_desligar(); printf("%s", tr("farejo desligado\n")); return; }
  if (argc >= 2 && eh(argv[1], "limpar")){ tab5_farejo_limpar(); printf("%s", tr("tabela zerada\n")); return; }
  if (argc >= 2 && eh(argv[1], "on")) {
    c6_farejo_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.filtro = 0;                              // os dois tipos
    cfg.canais = 0;                              // canal atual (associado ao roteador)
    cfg.lote_ms = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 250;
    // Mascara de canais em hex opcional: 'farejo on <lote_ms> <mask>'. Com
    // mais de um canal, SOLTE o roteador antes ('espnow canal 1' ja faz isso),
    // senao o C6 fica preso no canal do AP.
    if (argc >= 4) { cfg.canais = (uint16_t)strtoul(argv[3], nullptr, 16); }
    if (!tab5_farejo_ligar(&cfg)) {
      printf("%s", tr("nao liguei: o C6 tem o firmware nosso (v3)? WiFi ligado? ('farejo info')\n"));
      return;
    }
    printf(tr("farejo ligado (lote %u ms, canais 0x%04X=0 e o atual). Rode 'farejo' para ver.\n"),
           (unsigned)cfg.lote_ms, (unsigned)cfg.canais);
    return;
  }
  if (argc >= 2) { mostrar_uso("farejo"); return; }
  farejo_estado();
}

// ---------------------------------------------------------------------------
// RADIO - teste dos extras do C6 (CSI, FTM, 802.15.4)
// ---------------------------------------------------------------------------
// Liga cada extra por alguns segundos e conta o que volta. Serve para provar a
// gravacao do C6 (ligar, receber e desligar sem derrubar o C6 nem o WiFi) - o
// que so se conserta com o aparelho aberto. Os contadores sao escritos na
// tarefa de eventos do esp_hosted e lidos aqui: volateis, sem trava.
//
// O CSI passa pelo servico tab5_csi (o mesmo que o Radar usa): o peer_data
// guarda UM ouvinte por mensagem, e registrar o CSI aqui roubaria o do servico.
static std::atomic<uint32_t> s_rx_csi = 0, s_rx_154 = 0, s_rx_ftm = 0;
static volatile int      s_rx_rssi = 0, s_rx_canal = 0, s_rx_len = 0;
static c6_ftm_rep_t      s_rx_ftm_rep;
static bool              s_radio_registrado = false;
// CSI por formato de quadro (C6_CSI_FORMATO: 0 11b ... 7 HE TB) e os avisos
// que mudam a leitura dos bytes.
static std::atomic<uint32_t> s_csi_fmt[16], s_csi_w0_ruim = 0, s_csi_cortado = 0, s_csi_len_max = 0;

static void radio_csi_rx(const c6_csi_cab_t* c, const uint8_t*, uint16_t)
{
  s_rx_rssi = c->rssi; s_rx_canal = c->canal; s_rx_len = c->len;
  ++s_csi_fmt[C6_CSI_FORMATO(c->flags)];
  if (c->flags & C6_CSI_F_W0_RUIM)  { ++s_csi_w0_ruim; }
  if (c->flags & C6_CSI_F_CORTADO)  { ++s_csi_cortado; }
  if (c->len > s_csi_len_max)       { s_csi_len_max = c->len; }
  ++s_rx_csi;
}

static void radio_rx(uint32_t msg_id, const uint8_t* d, size_t n, void*)
{
  if (msg_id == C6_MSG_154 && n >= sizeof(c6_154_cab_t)) {
    c6_154_cab_t c; memcpy(&c, d, sizeof(c));
    s_rx_rssi = c.rssi; s_rx_canal = c.canal; s_rx_len = c.len;
    ++s_rx_154;
  } else if (msg_id == C6_MSG_FTM && n >= sizeof(c6_ftm_rep_t)) {
    memcpy(&s_rx_ftm_rep, d, sizeof(s_rx_ftm_rep));
    ++s_rx_ftm;
  }
}

static bool radio_preparar(void)
{
  if (!tab5_wifi_c6_nosso() || !tab5_wifi_c6_enlace()) {
    printf("%s", tr("sem o C6 nosso no ar (WiFi ligado? 'farejo info')\n"));
    return false;
  }
  if (!s_radio_registrado) {
    eh_host_peer_data_register(C6_MSG_FTM, radio_rx, nullptr);
    eh_host_peer_data_register(C6_MSG_154, radio_rx, nullptr);
    s_radio_registrado = true;
  }
  return true;
}

static void radio_mandar(uint8_t sub, const void* cfg, size_t n)
{
  uint8_t b[1 + 16];
  b[0] = sub;
  memcpy(b + 1, cfg, n);
  eh_host_peer_data_send(C6_MSG_HOST_RADIO, b, 1 + n);
}

// Liga, renova a cada 3 s (o C6 desliga sozinho em 10 s sem renovar), mostra
// a contagem a cada segundo e desliga no fim.
//
// ping_meio: pinga o roteador na metade da janela, com o recurso LIGADO - e o
// teste que separa "associado" de "passando dados".
static void ping_roteador(const char* rotulo);

static void radio_janela(uint8_t sub, const void* cfg_on, const void* cfg_off, size_t n,
                         int segundos, std::atomic<uint32_t>* cont, bool ping_meio = false)
{
  const uint32_t antes = *cont;
  for (int s = 0; s < segundos; ++s) {
    if (ping_meio && s > 0 && s == segundos / 2) { ping_roteador("  WiFi DURANTE"); }
    if (s % 3 == 0) { radio_mandar(sub, cfg_on, n); }
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf(tr("  %2d s: %lu recebidos   ultimo: canal %d  rssi %d  %d B\n"), s + 1,
           (unsigned long)(*cont - antes), s_rx_canal, s_rx_rssi, s_rx_len);
  }
  radio_mandar(sub, cfg_off, n);
  printf("%s", tr("desligado. O que o C6 diz (ve tambem se o 'no ar' nao zerou):\n"));
  farejo_info();
}

void c_radio(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("radio"); return; }
  if (!radio_preparar()) { return; }
  // csi [s] | 154 [canal] [s]
  const bool z154 = eh(argv[1], "154");
  const int  i_seg = z154 ? 3 : 2;
  int seg = (argc > i_seg) ? atoi(argv[i_seg]) : 10;
  if (seg < 1 || seg > 120) { seg = 10; }

  if (eh(argv[1], "csi")) {
    // Pelo servico tab5_csi: ele pede ao C6 so o CSI do roteador, pinga o
    // roteador a 20 Hz como fonte (o beacon de 2,4 GHz, a 1 Mbps, nao gera
    // CSI) e mantem o receptor do C6 acordado. Aqui so se conta o que chega e
    // se mostra a medida de movimento - parado, sem ninguem andando perto, a
    // razao fica perto de 1.
    printf(tr("CSI por %d s (1 record a cada 100 ms, so do roteador; farejo %s).\n"
           "Deixe o Tab5 apoiado: os 3 primeiros segundos aprendem o 'parado'.\n"),
           seg, tab5_farejo_ativo() ? tr("LIGADO") : tr("desligado"));
    memset((void*)s_csi_fmt, 0, sizeof(s_csi_fmt));
    s_csi_w0_ruim = 0; s_csi_cortado = 0; s_csi_len_max = 0;
    const uint32_t antes = s_rx_csi;
    tab5_csi_ouvinte(radio_csi_rx);
    tab5_csi_ligar(TAB5_CSI_CONSOLE);
    static const char* const NIVEL[3] = { "parado", "leve", "FORTE" };
    for (int i = 0; i < seg; ++i) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      tab5_csi_estado_t e;
      tab5_csi_estado(&e);
      printf(tr("  %2d s: %lu recebidos  rssi %d  %d B   "), i + 1, (unsigned long)(s_rx_csi - antes),
             s_rx_rssi, s_rx_len);
      switch (e.fase) {
        case TAB5_CSI_SEM_C6:          printf("%s", tr("C6 sem CSI\n")); break;
        case TAB5_CSI_SEM_ROTEADOR:    printf("%s", tr("sem roteador\n")); break;
        case TAB5_CSI_APRENDENDO:      printf("%s", tr("aprendendo o parado...\n")); break;
        case TAB5_CSI_APARELHO_MEXENDO:printf("%s", tr("Tab5 mexendo\n")); break;
        case TAB5_CSI_MEDINDO:         printf(tr("movimento: %s (x%.1f)\n"), NIVEL[e.nivel], (double)e.razao); break;
        default:                       printf("\n"); break;
      }
    }
    tab5_csi_desligar(TAB5_CSI_CONSOLE);
    tab5_csi_ouvinte(nullptr);
    static const char* const FMT[8] = { "11b", "11g", "HT", "VHT", "HE-SU", "HE-MU", "HE-ERSU", "HE-TB" };
    printf("%s", tr("por formato:"));
    for (int f = 0; f < 16; ++f) {
      if (s_csi_fmt[f]) { printf("  %s %lu", f < 8 ? FMT[f] : "?", (unsigned long)s_csi_fmt[f]); }
    }
    printf(tr("\nmaior record %lu B   1a palavra invalida em %lu   cortados %lu\n"),
           (unsigned long)s_csi_len_max, (unsigned long)s_csi_w0_ruim, (unsigned long)s_csi_cortado);
    return;
  }
  if (eh(argv[1], "154")) {
    c6_154_cfg_t on, off;
    memset(&on, 0, sizeof(on)); memset(&off, 0, sizeof(off));
    on.liga = 1;
    on.canal = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 15;
    if (on.canal < 11 || on.canal > 26) { printf("%s", tr("canal 802.15.4 vai de 11 a 26\n")); return; }
    on.promiscuo = 1;
    printf(tr("802.15.4 no canal %u por %d s (so ouvindo; sem Zigbee/Thread por perto, 0 e normal):\n"),
           (unsigned)on.canal, seg);
    // O 802.15.4 divide a antena com o WiFi: o teste mede o WiFi antes, com
    // ele ligado e depois. Os tres em 4/4 = a coexistencia esta funcionando.
    ping_roteador("  WiFi ANTES");
    radio_janela(C6_RX_154, &on, &off, sizeof(on), seg, &s_rx_154, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    ping_roteador("  WiFi DEPOIS");
    return;
  }
  if (eh(argv[1], "ftm")) {
    // Alvo: o roteador conectado (MAC e canal dele).
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { printf("%s", tr("sem roteador conectado\n")); return; }
    c6_ftm_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.peer, ap.bssid, 6);
    c.canal = ap.primary;
    c.frm_count = 16;
    printf(tr("FTM contra %02X:%02X:%02X:%02X:%02X:%02X (canal %u, FTM %s no anuncio)...\n"),
           c.peer[0], c.peer[1], c.peer[2], c.peer[3], c.peer[4], c.peer[5], (unsigned)c.canal,
           ap.ftm_responder ? tr("SIM") : tr("nao"));
    const uint32_t antes = s_rx_ftm;
    radio_mandar(C6_RX_FTM, &c, sizeof(c));
    for (int i = 0; i < 50 && s_rx_ftm == antes; ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
    if (s_rx_ftm == antes) { printf("%s", tr("sem resposta do C6 em 5 s\n")); return; }
    const c6_ftm_rep_t& r = s_rx_ftm_rep;
    if (r.status == 0) {
      printf(tr("ok: distancia %lu cm, rtt %lu ns (%u quadros)\n"),
             (unsigned long)r.dist_est_cm, (unsigned long)r.rtt_est_ns, (unsigned)r.num);
    } else {
      if (r.status == C6_FTM_NAO_INICIOU) {
        // O C6 nem mandou a requisicao: o motivo e o esp_err_t do driver dele.
        const esp_err_t e = (esp_err_t)(int32_t)r.rtt_raw_ns;
        printf(tr("o C6 nao iniciou a sessao: %s\n"), esp_err_to_name(e));
        if (e == ESP_ERR_NOT_SUPPORTED) {
          // esp_wifi/src/wifi_init.c apaga o bit do FTM initiator nos C6 v0.0
          // e v0.1 (errata do silicio). Nao ha contorno honesto: forcar o bit
          // daria distancias erradas.
          printf("%s", tr("  o ESP32-C6 das revisoes v0.0/v0.1 nao faz FTM initiator (errata do chip;\n"
                 "  o proprio ESP-IDF desliga). 'farejo info' mostra a revisao deste C6.\n"));
        }
      } else {
        static const char* const ST[] = { "success", "the peer does not speak FTM", "refused",
                                          "no response", "failed", "no valid measurement", "ended" };
        printf(tr("sessao feita, status %u (%s)%s\n"), (unsigned)r.status,
               r.status < 7 ? ST[r.status] : "?",
               ap.ftm_responder ? "" : tr(" - o roteador nao anuncia FTM responder"));
      }
    }
    return;
  }
  mostrar_uso("radio");
}

// ---------------------------------------------------------------------------
// PING - separa "o WiFi nao passa dados" de "a internet/servidor nao responde"
// ---------------------------------------------------------------------------
static std::atomic<int> s_ping_ok = 0, s_ping_perdidos = 0;
static volatile uint32_t s_ping_ms_soma = 0;

static void ping_ok(esp_ping_handle_t h, void*)
{
  uint32_t ms = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
  ++s_ping_ok;
  s_ping_ms_soma = s_ping_ms_soma + ms;
}
static void ping_perdido(esp_ping_handle_t, void*) { ++s_ping_perdidos; }

static void ping_um(const char* rotulo, const ip_addr_t& alvo)
{
  s_ping_ok = 0; s_ping_perdidos = 0;
  s_ping_ms_soma = 0;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = alvo;
  cfg.count = 4;
  cfg.interval_ms = 300;
  cfg.timeout_ms = 1500;
  esp_ping_callbacks_t cb = { };
  cb.on_ping_success = ping_ok;
  cb.on_ping_timeout = ping_perdido;
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { printf(tr("%s: nao deu para criar o ping\n"), rotulo); return; }
  esp_ping_start(h);
  for (int i = 0; i < 80 && s_ping_ok + s_ping_perdidos < 4 && !console_interrompido(); ++i) { vTaskDelay(pdMS_TO_TICKS(100)); }
  esp_ping_stop(h);
  esp_ping_delete_session(h);
  if (s_ping_ok) {
    printf(tr("%-22s %d/4 ok   media %lu ms\n"), rotulo, s_ping_ok.load(),
           (unsigned long)(s_ping_ms_soma / (uint32_t)s_ping_ok));
  } else {
    printf(tr("%-22s 0/4 - SEM RESPOSTA\n"), rotulo);
  }
}

static void ping_roteador(const char* rotulo)
{
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.gw.addr == 0) {
    printf(tr("%-22s sem IP - WiFi desconectado\n"), rotulo);
    return;
  }
  ip_addr_t a = { };
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_um(rotulo, a);
}

void c_ping(int argc, char** argv)
{
  esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
    printf("%s", tr("sem IP: o WiFi nao esta conectado\n"));
    return;
  }
  ip_addr_t a = { };
  if (argc >= 2) {
    // Um alvo so: IP ou nome.
    struct addrinfo* res = nullptr;
    if (getaddrinfo(argv[1], nullptr, nullptr, &res) != 0 || !res) {
      printf(tr("%s: o DNS nao resolveu (sem internet ou nome errado)\n"), argv[1]);
      return;
    }
    a.type = IPADDR_TYPE_V4;
    a.u_addr.ip4.addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    ping_um(argv[1], a);
    return;
  }
  // Sem alvo: o roteador (o WiFi passa dados?) e a internet (passa do roteador?).
  a.type = IPADDR_TYPE_V4;
  a.u_addr.ip4.addr = ip.gw.addr;
  ping_um("roteador (gateway)", a);
  a.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
  ping_um("internet (8.8.8.8)", a);
}

// ---------------------------------------------------------------------------
// FERRAMENTAS DE REDE (services/tab5_rede): ip, dns, arp, traceroute, tcp,
// http, busca. O motor e do servico; aqui so se imprime.
// ---------------------------------------------------------------------------
static void mac_txt(const uint8_t* m, char* out) { sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }

void c_ip(int, char**)
{
  tab5_rede_ip_t r;
  if (!tab5_rede_ip(&r)) { printf("%s", tr("sem interface de Wi-Fi\n")); return; }
  char mac[18];
  mac_txt(r.mac, mac);
  printf(tr("wifi  %s\n"), r.conectado ? tr("conectado") : tr("SEM IP (o Wi-Fi nao esta conectado)"));
  if (r.conectado) {
    printf(tr("  ip       %s / %s\n  gateway  %s\n"), r.ip, r.mascara, r.gateway);
  }
  if (r.dns[0][0]) { printf(tr("  dns      %s%s%s\n"), r.dns[0], r.dns[1][0] ? "  " : "", r.dns[1]); }
  if (r.ipv6[0])   { printf(tr("  ipv6     %s\n"), r.ipv6); }
  printf(tr("  mac      %s\n"), mac);
  if (r.nome[0])   { printf(tr("  nome     %s\n"), r.nome); }
}

void c_dns(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("dns"); return; }
  char ips[4][TAB5_REDE_IP_TXT];
  uint32_t ms = 0;
  const int n = tab5_rede_dns(argv[1], ips, 4, &ms);
  if (!n) { printf(tr("%s: nao resolveu (%lu ms) - nome errado ou sem internet\n"), argv[1], (unsigned long)ms); return; }
  printf(tr("%s  (%lu ms)\n"), argv[1], (unsigned long)ms);
  for (int i = 0; i < n; ++i) { printf("  %s\n", ips[i]); }
}

static void arp_achou(const tab5_rede_vizinho_t* v, void* ctx)
{
  char mac[18];
  mac_txt(v->mac, mac);
  printf("  %-15s  %s\n", v->ip, mac);
  fflush(stdout);
  ++*(int*)ctx;
}

void c_arp(int argc, char** argv)
{
  if (argc >= 2 && (eh(argv[1], "varrer") || eh(argv[1], "-a"))) {
    printf("%s", tr("varrendo a rede local (/24), ~15 s...\n"));
    int n = 0;
    if (tab5_rede_arp_varrer(arp_achou, &n, console_interrompido) < 0) { printf("%s", tr("sem rede\n")); return; }
    printf(tr("%d aparelho(s) responderam\n"), n);
    return;
  }
  tab5_rede_vizinho_t v[16];
  const int n = tab5_rede_arp(v, 16);
  int k = 0;
  for (int i = 0; i < n; ++i) { arp_achou(&v[i], &k); }
  printf(tr("%d na tabela ARP ('arp varrer' pergunta a rede inteira)\n"), n);
}

static void salto(const tab5_rede_salto_t* s, void*)
{
  printf("%2d  %-15s", s->ttl, s->ip[0] ? s->ip : "*");
  for (int k = 0; k < 3; ++k) {
    if (s->ms[k] >= 0) { printf(tr("  %4d ms"), s->ms[k]); } else { printf("       *"); }
  }
  printf("\n");
  fflush(stdout);
}

void c_traceroute(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("traceroute"); return; }
  const int max = argc >= 3 ? atoi(argv[2]) : 20;
  printf(tr("caminho ate %s (ate %d saltos, 3 sondas de 1 s)\n"), argv[1], max);
  const int r = tab5_rede_traceroute(argv[1], max > 0 && max <= 64 ? max : 20, salto, nullptr, console_interrompido);
  if (r < 0)       { printf(tr("%s: nao resolveu, ou sem rede\n"), argv[1]); }
  else if (r == 0) { printf("%s", tr("o alvo nao respondeu (firewall que corta ICMP e comum)\n")); }
  else             { printf(tr("chegou em %d salto(s)\n"), r); }
}

void c_tcp(int argc, char** argv)
{
  if (argc < 3) { mostrar_uso("tcp"); return; }
  uint32_t ms = 0;
  const int r = tab5_rede_tcp(argv[1], atoi(argv[2]), 5000, &ms);
  if (r == 0)               { printf(tr("%s:%s ABERTA (%lu ms)\n"), argv[1], argv[2], (unsigned long)ms); }
  else if (r == -1)         { printf(tr("%s: nao resolveu\n"), argv[1]); }
  else if (r == -ECONNREFUSED) { printf(tr("%s:%s fechada (recusou)\n"), argv[1], argv[2]); }
  else if (r == -ETIMEDOUT) { printf(tr("%s:%s sem resposta em 5 s (filtrada?)\n"), argv[1], argv[2]); }
  else                      { printf(tr("%s:%s erro %d (%s)\n"), argv[1], argv[2], -r, strerror(-r)); }
}

static bool tipo_texto(const char* t)
{
  return !t[0] || strncasecmp(t, "text/", 5) == 0 || strcasestr(t, "json") || strcasestr(t, "xml")
      || strcasestr(t, "javascript");
}

// O corpo vai para a tela so se for texto (o tipo chega nos cabecalhos, antes
// dele), ate 'resta' bytes; binario para de ler na hora.
typedef struct { int64_t resta; const tab5_rede_http_t* info; bool binario; } http_mostra_t;
static bool http_mostrar(const char* d, int n, void* ctx)
{
  http_mostra_t* m = (http_mostra_t*)ctx;
  if (!tipo_texto(m->info->tipo)) { m->binario = true; return false; }
  const int k = n < m->resta ? n : (int)m->resta;
  fwrite(d, 1, (size_t)k, stdout);
  m->resta -= k;
  return m->resta > 0;
}

// BAIXAR PARA O DISCO: o corpo vai direto para um arquivo novo
// (tab5_arq_novo_*: nome repetido vira "x (2)", e o que nao terminou bem e
// apagado - nunca fica arquivo pela metade).
static bool http_gravar(const char* d, int n, void* ctx) { return tab5_arq_novo_escrever((tab5_arq_novo_t*)ctx, d, (size_t)n); }

static void http_baixar(const char* url, const char* saida)
{
  char nome[TAB5_ARQ_NOME_MAX];
  if (saida) {
    snprintf(nome, sizeof(nome), "%s", saida);
  } else {
    // O nome do fim da URL (sem ?...), como o wget; sem nome, index.html.
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    const size_t fim = strcspn(p, "?#");
    const char* barra = nullptr;
    for (size_t k = 0; k < fim; ++k) { if (p[k] == '/') { barra = p + k; } }
    const int n = barra ? (int)(p + fim - barra - 1) : 0;
    if (n > 0) { snprintf(nome, sizeof(nome), "%.*s", n, barra + 1); }
    else       { snprintf(nome, sizeof(nome), "index.html"); }
  }
  char caminho[TAB5_ARQ_CAMINHO_MAX];
  resolver(nome, caminho, sizeof(caminho));
  char* b = strrchr(caminho, '/');
  if (!b || b == caminho || !b[1]) { printf("%s", tr("salvar onde? use -o <arquivo> (ex.: -o Downloads/x.zip)\n")); return; }
  *b = 0;
  tab5_arq_novo_t a;
  const tab5_arq_erro_t r = tab5_arq_novo_abrir(&a, caminho, b + 1);
  if (r != TAB5_ARQ_OK) { printf(tr("nao da para gravar em %s: %s\n"), caminho, tab5_arq_erro_texto(r)); return; }
  printf(tr("baixando para %s ...\n"), a.caminho);
  fflush(stdout);
  tab5_rede_http_t info;
  const bool ok = tab5_rede_http(url, false, http_gravar, &a, &info, console_interrompido);
  const bool inteiro = ok && info.status == 200 && !console_interrompido()
                    && (info.tamanho < 0 || info.recebido == info.tamanho);
  const bool gravou = tab5_arq_novo_fechar(&a, inteiro) == TAB5_ARQ_OK;
  if (gravou) {
    printf(tr("gravado: %s  (%lld bytes, %lu ms)\n"), a.caminho, (long long)info.recebido, (unsigned long)info.ms);
  } else if (!info.status) {
    printf(tr("nao conectou: %s\n"), info.erro);
  } else {
    printf(tr("nao gravado (HTTP %d, %lld de %lld bytes) - o pedaco foi apagado\n"), info.status,
           (long long)info.recebido, (long long)info.tamanho);
  }
}

void c_http(int argc, char** argv)
{
  bool so_cab = false;
  int64_t max = 4096;
  const char* url = nullptr;
  const char* saida = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (eh(argv[i], "-I"))                      { so_cab = true; }
    else if (eh(argv[i], "-n") && i + 1 < argc) { max = atoll(argv[++i]); }
    else if (eh(argv[i], "-o") && i + 1 < argc) { saida = argv[++i]; }
    else                                        { url = argv[i]; }
  }
  if (!url) { mostrar_uso("http"); return; }
  char u[256];
  snprintf(u, sizeof(u), "%s%s", strstr(url, "://") ? "" : "http://", url);
  // 'wget' grava (como o de sempre); 'http'/'curl' mostram, a nao ser com -o.
  if (saida || eh(argv[0], "wget")) { http_baixar(u, saida); return; }
  tab5_rede_http_t info;
  http_mostra_t m = { max, &info, false };
  tab5_rede_http(u, so_cab, max > 0 ? http_mostrar : nullptr, &m, &info, console_interrompido);
  if (!info.status) { printf(tr("nao conectou: %s\n"), info.erro); return; }
  if (info.recebido && !m.binario) { printf("\n"); }
  char tam[32] = "";
  const int64_t b = info.tamanho >= 0 ? info.tamanho : info.recebido;
  if (b > 0) { snprintf(tam, sizeof(tam), "  %lld byte(s)", (long long)b); }
  printf(tr("--- HTTP %d  %s%s  %lu ms\n"), info.status, info.tipo[0] ? info.tipo : tr("(sem tipo)"), tam, (unsigned long)info.ms);
  if (info.redirecionou) { printf(tr("--- foi para %s\n"), info.final); }
  if (m.binario) { printf("%s", tr("--- binario: o conteudo nao e mostrado\n")); }
  else if (!so_cab && m.resta <= 0 && max > 0) { printf(tr("--- cortado em %lld bytes ('-n <bytes>' mostra mais)\n"), (long long)max); }
}

// O texto de "chave" num JSON (a primeira depois de 'de'), com os escapes
// desfeitos (\uXXXX vira UTF-8). Devolve onde parou, ou nullptr.
static const char* json_texto(const char* de, const char* chave, char* out, size_t n)
{
  char pad[40];
  snprintf(pad, sizeof(pad), "\"%s\":\"", chave);
  const char* p = strstr(de, pad);
  if (!p) { out[0] = 0; return nullptr; }
  p += strlen(pad);
  size_t k = 0;
  for (; *p && *p != '"' && k + 4 < n; ++p) {
    if (*p != '\\') { out[k++] = *p; continue; }
    ++p;
    if (*p == 'n') { out[k++] = ' '; }
    else if (*p == 'u' && isxdigit((unsigned char)p[1])) {
      char h[5] = { p[1], p[2], p[3], p[4], 0 };
      const unsigned cp = (unsigned)strtoul(h, nullptr, 16);
      p += 4;
      if (cp < 0x80)       { out[k++] = (char)cp; }
      else if (cp < 0x800) { out[k++] = (char)(0xC0 | (cp >> 6)); out[k++] = (char)(0x80 | (cp & 0x3F)); }
      else { out[k++] = (char)(0xE0 | (cp >> 12)); out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[k++] = (char)(0x80 | (cp & 0x3F)); }
    }
    else if (*p) { out[k++] = *p; }
  }
  out[k] = 0;
  return p;
}

// BUSCA: o resumo da Wikipedia em portugues (achar o artigo, depois o resumo);
// sem artigo, a resposta instantanea do DuckDuckGo. Sem chave de API.
void c_busca(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("busca"); return; }
  char termo[160], q[480], url[600];
  juntar(termo, sizeof(termo), argc, argv, 1);
  tab5_rede_url_codificar(termo, q, sizeof(q));
  tab5_rede_http_t info;
  char* titulo = (char*)heap_caps_malloc(200, MALLOC_CAP_SPIRAM);
  char* txt = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  if (!titulo || !txt) { heap_caps_free(titulo); heap_caps_free(txt); printf("%s", tr("sem memoria\n")); return; }
  bool achou = false;

  snprintf(url, sizeof(url), "https://pt.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=1&srsearch=%s", q);
  char* j = tab5_rede_http_texto(url, 16384, &info, console_interrompido);
  if (!info.status) { printf(tr("sem internet: %s\n"), info.erro); }
  if (j && json_texto(j, "title", titulo, 200) && titulo[0]) {
    free(j);
    char t[400];
    tab5_rede_url_codificar(titulo, t, sizeof(t));
    for (char* c = t; *c; ++c) { if (*c == '+') { *c = '_'; } }   // espaco no titulo e '_' na URL
    snprintf(url, sizeof(url), "https://pt.wikipedia.org/api/rest_v1/page/summary/%s", t);
    j = tab5_rede_http_texto(url, 32768, &info, console_interrompido);
    if (j && json_texto(j, "extract", txt, 2048) && txt[0]) {
      printf(tr("%s\n\n%s\n\n(https://pt.wikipedia.org/wiki/%s)\n"), titulo, txt, t);
      achou = true;
    }
  }
  free(j);
  j = nullptr;
  if (!achou && info.status) {
    snprintf(url, sizeof(url), "https://api.duckduckgo.com/?format=json&no_html=1&skip_disambig=1&q=%s", q);
    j = tab5_rede_http_texto(url, 65536, &info, console_interrompido);
    if (j) {
      const char* CH[3] = { "Answer", "AbstractText", "Definition" };
      for (int i = 0; i < 3 && !achou; ++i) {
        if (json_texto(j, CH[i], txt, 2048) && txt[0]) { printf("%s\n", txt); achou = true; }
      }
      // Sem resumo: os primeiros assuntos relacionados.
      const char* p = j;
      int n = 0;
      while (!achou && n < 5 && (p = json_texto(p, "Text", txt, 2048)) != nullptr) {
        if (txt[0]) { printf("  - %s\n", txt); ++n; }
      }
      if (n) { achou = true; }
    }
    free(j);
  }
  if (!achou && info.status) { printf(tr("nada encontrado para '%s'\n"), termo); }
  heap_caps_free(titulo);
  heap_caps_free(txt);
}

// SSH (services/tab5_ssh): 'ssh [-p porta] usuario@host [comando]'.
static void ssh_saida(const char* d, int n, void*) { fwrite(d, 1, (size_t)n, stdout); fflush(stdout); }

void c_ssh(int argc, char** argv)
{
  int porta = 22, i = 1;
  if (argc >= 3 && eh(argv[1], "esquecer")) {
    // 'ssh esquecer host[:porta]': tira a chave guardada (servidor reinstalado).
    char h[96];
    snprintf(h, sizeof(h), "%s", argv[2]);
    char* dois = strrchr(h, ':');
    if (dois) { *dois = 0; porta = atoi(dois + 1); }
    char* arroba = strchr(h, '@');
    const char* host = arroba ? arroba + 1 : h;
    printf(tab5_ssh_esquecer(host, porta) ? tr("chave de %s:%d esquecida - a proxima conexao guarda a nova\n")
                                          : tr("%s:%d nao estava guardado\n"), host, porta);
    return;
  }
  if (argc == 2 && eh(argv[1], "fechar")) {
    tab5_ssh_sessao_info_t si;
    tab5_ssh_sessao_info(&si);
    const bool havia = si.estado >= TAB5_SSH_CONECTANDO && si.estado <= TAB5_SSH_ABERTA;
    tab5_ssh_sessao_fechar();
    printf(havia ? tr("sessao com %s@%s encerrada\n") : tr("nenhuma sessao aberta\n"), si.usuario, si.host);
    return;
  }
  if (argc >= 3 && eh(argv[1], "-p")) { porta = atoi(argv[2]); i = 3; }
  if (i >= argc || !strchr(argv[i], '@')) { mostrar_uso("ssh"); return; }
  char quem[96];
  snprintf(quem, sizeof(quem), "%s", argv[i]);
  char* arroba = strchr(quem, '@');
  *arroba = 0;
  char* host = arroba + 1;
  char* dois = strrchr(host, ':');   // usuario@host:porta tambem vale
  if (dois) { *dois = 0; porta = atoi(dois + 1); }
  char cmd[192] = "";
  juntar(cmd, sizeof(cmd), argc, argv, i + 1);
  if (!cmd[0] && sessao()->interativo) {
    // UM TERMINAL DE VERDADE NA FRENTE (o app Terminal, a pagina /terminal/):
    // a sessao interativa compartilhada. A tela troca sozinha para ela e volta
    // aqui quando ela terminar; o tamanho e o de quem digitar primeiro.
    tab5_ssh_sessao_esquecer_fim();
    if (!tab5_ssh_sessao_abrir(quem, host, porta, 80, 24)) {
      printf("%s", tr("ssh: ja ha uma sessao aberta ('ssh fechar' encerra)\n"));
      return;
    }
    printf(tr("conectando a %s@%s:%d...\n"), quem, host, porta);
    return;
  }
  tab5_ssh_t s = { };
  s.usuario = quem;
  s.host = host;
  s.porta = porta;
  s.comando = cmd;
  s.ler_linha = console_ler_linha;
  s.saida = ssh_saida;
  s.parar = console_interrompido;
  char erro[96];
  const int r = tab5_ssh_rodar(&s, erro, sizeof(erro));
  if (r < 0)       { printf(tr("\nssh: %s\n"), erro); }
  else if (r != 0) { printf(tr("(o comando saiu com %d)\n"), r); }
}

// ACESSO PELO PC (services/tab5_web): 'web', 'web ligar [app]', 'web desligar'.
void c_web(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "desligar")) {
    printf(tab5_web_ligado() ? tr("desligando o acesso pelo PC\n") : tr("ja estava desligado\n"));
    tab5_web_desligar();
    return;
  }
  const char* cat[SISTEMA_APPS_MAX];
  const int nc = tab5_web_catalogo(cat, SISTEMA_APPS_MAX);
  if (argc >= 2 && eh(argv[1], "ligar")) {
    // Sem nome: o Terminal (quem digita esta nele), ou o primeiro do catalogo.
    const char* app = argc >= 3 ? argv[2] : nullptr;
    for (int i = 0; !app && i < nc; ++i) { if (eh(cat[i], "Terminal")) { app = cat[i]; } }
    if (!app && nc) { app = cat[0]; }
    if (!app) { printf("%s", tr("nenhum app desta variante abre no PC\n")); return; }
    if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { printf("%s", tr("conecte o Wi-Fi primeiro ('wifi')\n")); return; }
    if (!tab5_web_abrir_app(app)) { printf(tr("%s nao abriu (nao abre no PC, ou o servidor nao subiu)\n"), app); return; }
  } else if (argc >= 2) {
    mostrar_uso("web");
    return;
  }
  char url[64], senha[8];
  if (!tab5_web_ligado() || !tab5_web_endereco(nullptr, url, sizeof(url))) {
    printf("%s", tr("acesso pelo PC desligado - 'web ligar' liga (precisa do Wi-Fi)\n"));
  } else {
    tab5_web_senha(senha, sizeof(senha));
    printf(tr("no navegador do PC (mesma rede):  %s\nsenha (qualquer usuario):         %s\n"), url, senha);
  }
  printf("%s", tr("apps no PC:"));
  for (int i = 0; i < nc; ++i) { printf(" %s%s", cat[i], tab5_web_aberto(cat[i]) ? tr(" (aberto)") : ""); }
  printf("%s\n", nc ? "" : tr(" nenhum"));
}
