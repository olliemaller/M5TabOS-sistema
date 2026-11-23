// Servico Mesh (Meshtastic) - o motor: o enlace com o radio (C6L pelo Grove, pelo
// Bluetooth ou so pela internet), a tarefa, o envio e a fila de envio, os envios
// em voo e o desfecho gravado no cartao, e a API publica (tab5_mesh.hpp).
//
// O servico foi partido por assunto; o que as partes compartilham esta em
// mesh/tab5_mesh_interno.hpp:
//   mesh/tab5_mesh_hora.cpp       carimbo corrigido, que horas sao, hora ao vivo
//   mesh/tab5_mesh_nos.cpp        tabela de nos, favoritos, busca, cache de nomes, NodeDB no cartao
//   mesh/tab5_mesh_conversas.cpp  conversas e historico no cartao, apagadas, gravacao, ordem da coluna
//   mesh/tab5_mesh_canais.cpp     canais do no
//   mesh/tab5_mesh_receber.cpp    interpretacao das mensagens do radio
//   mesh/tab5_mesh_gps.cpp        posicao do GPS para a malha

#include "app.h"   // sistema_avisar
#include "idioma.h"
#include "esp_mac.h"   // numero_do_tab5
#include "tab5_donos.hpp"
#include "pool_tarefas.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR - buffers grandes na PSRAM
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "parada.hpp"      // parada cooperativa da tarefa (kernel)
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "tab5_grove.hpp"
#include "notificacao.h"
#include "ajustes.h"
#include "tab5_mesh_ble.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_diario.hpp"
#include "tab5_mesh_wifi_no.hpp"
#include "mesh_pb.hpp"
#include "tab5_wifi.hpp"
#include "tab5_hora.hpp"
#include "nvs.h"
#include "tab5_monitor.hpp"   // tab5_mem_marco
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

#define MESH_UART   UART_NUM_1
#define MESH_RX     54
#define MESH_TX     53
#define MESH_BAUD   115200

// Sem frame por este tempo, consideramos o enlace caido.
// QUANTO SILENCIO ANTES DE DAR O ENLACE POR MUDO.
//
// Era 30s, e 30s e MENOS do que uma malha parada leva para falar. O radio nao
// diz nada por conta propria: telemetria dele sai a cada meia hora, e se
// ninguem transmite nada nao chega frame nenhum. Ou seja, numa malha quieta o
// enlace passava a maior parte do tempo marcado como "mudo" - com o C6L
// perfeitamente conectado. E "mudo" tinha consequencia: disparava um pedido de
// configuracao a cada 10s (ver a tarefa), e cada pedido REINICIA o download
// inteiro no radio. Ficavamos num ciclo de pedir configuracao para sempre, e
// enquanto o dump roda o trafego ao vivo espera na queue do radio - que e
// pequena e descarta o excedente. Mensagem que chega nessa janela some.
//
// 90s: ainda percebe um cabo arrancado em pouco mais de um minuto, e ja nao
// confunde "a malha esta quieta" com "o radio sumiu".
#define SILENCIO_MS 90000

// ---------------------------------------------------------------------------
// Protobuf: services/mesh_pb.hpp (o mesmo codec da ponte MQTT e do admin)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
static Trava s_trava;

// VIDA DO SERVICO. Ate aqui ele so sabia nascer: uma vez de pe, segurava a UART
// do Grove para sempre. Trocar o C6L pelo GPS no cabo nao adiantava nada - o
// modulo novo estava la, mudo, com a porta na mao de quem ja tinha ido embora.
//
// Parada COOPERATIVA (kernel/parada.hpp), e nao vTaskDelete de fora: a
// tarefa pega mutex e escreve no cartao, e mata-la no meio disso deixaria o
// mutex preso e o arquivo aberto. Quem pede marca o pedido; a tarefa termina
// o que estava fazendo, confirma, e so entao a limpeza acontece.
static parada_t      s_parada = { };
bool          s_servico_ativo = false;  // ha tarefa e enlace de pe
// POR ONDE: o cabo Grove ou o Bluetooth. Fixado no iniciar, pela
// preferencia (ajustes_mesh_via) - so muda parando e subindo de novo.
uint8_t       s_via = AJUSTES_MESH_SERIAL;
EXT_RAM_BSS_ATTR mesh_no_t   s_nos[MESH_MAX_NOS];
uint32_t    s_despejos = 0;   // quantas vezes achar_no() despejou o mais antigo desde o boot
EXT_RAM_BSS_ATTR mesh_msg_t  s_msgs[MESH_MAX_MSGS];
int         s_n_msgs = 0;
// TOTAL DE VERDADE, NUNCA SATURA. Sintoma: mensagem nova so aparecia depois
// de trocar de tela e voltar. app_mesh.cpp detectava
// "chegou mensagem nova" comparando tab5_mesh_num_msgs() (que devolve
// s_n_msgs) contra o valor da ultima passada - mas s_n_msgs e o tamanho do
// ANEL de 8 posicoes (MESH_MAX_MSGS), que para de crescer assim que 8
// mensagens (de QUALQUER conversa) ja passaram por ele. Depois disso o
// numero fica parado para sempre, e o comparador nunca mais via diferenca -
// so trocar de tela forcava outros caminhos (s_hist_pronto=false direto) a
// recarregar. Este contador so cresce, nunca reseta a MESH_MAX_MSGS.
uint32_t     s_total_msgs = 0;
int         s_nao_lidas = 0;
int         s_nao_lidas_dir = 0;
mesh_saude_t s_saude = { };
uint32_t    s_meu_num = 0;
char        s_firmware[24] = { 0 };
uint32_t    s_ultimo_quadro_ms = 0;
static uint32_t    s_iniciado_em_ms   = 0;   // ver o aviso em tab5_mesh_silencio_ms()
int         s_notif = -1;   // fonte no servico de notificacao

// NOMES REAIS DOS CANAIS, ditos pelo proprio radio.
//
// Antes o canal 0 era "LongFast" cravado no codigo e o resto virava "Canal 1",
// "Canal 2". Mas o Meshtastic MANDA a configuracao dos canais no dump inicial
// (FromRadio campo 10) - com o nome que voce escolheu no celular. Ignorar isso
// era inventar um rotulo quando o certo estava chegando pela serial.
char s_canal_nome[MESH_MAX_CANAIS][MESH_NOME_CONVERSA];

// SE O CANAL ESTA ATIVO AGORA, pela ULTIMA config que o radio mandou - nasceu
// junto de "canais disponiveis" (ver mais abaixo). Antes disto
// nada guardava esse estado: s_canal_nome[] so era ESCRITO quando o papel
// vinha != 0 (ver o parser do campo 10 abaixo), mas nunca ficava sabendo que
// um canal tinha sido DESLIGADO - o nome antigo so ficava parado la,
// indistinguivel de "ainda ativo". Comeca tudo false: so vira true quando a
// PRIMEIRA config chegar, e nenhum canal deveria ser oferecido como
// "disponivel" antes disso (nao sabemos nada sobre ele ainda).
bool s_canal_ativo[MESH_MAX_CANAIS];
bool s_canais_conhecidos = false;   // ha configuracao (do radio ou da NVS)
bool s_canais_sujos      = false;   // mudou e ainda nao foi para a NVS

// O radio avisou que reiniciou (FromRadio campo 8). Sem isto, a gente so
// percebia pelo silencio - e esperava o prazo inteiro de 10s para so entao
// pedir a configuracao de novo.
volatile bool s_radio_reiniciou = false;
mesh_conversa_t s_conv[MESH_MAX_CONVERSAS];

// ORDEM DA COLUNA (ver tab5_mesh_conversas_versao no .hpp). 's_conv_seq' e a
// ultima atividade da conversa NESTA sessao, num contador que so cresce -
// nao o rx_time, que vale 0 quando o relogio ainda nao foi acertado e
// empataria tudo. Conversa so lida do cartao fica com 0: vai para o fim, na
// ordem da pasta.
uint32_t s_conv_seq[MESH_MAX_CONVERSAS];
uint32_t s_seq = 0;
uint32_t s_conv_ver = 0;

// Contadores de diagnostico. Existem porque "nos 0/0" nao diz se o LocalStats
// nao CHEGA ou se chega e o filtro descarta - e sao perguntas com conserto oposto.
uint32_t s_c_quadros = 0, s_c_pacotes = 0, s_c_telemetria = 0;

// O GROVE POR DENTRO (console 'mesh grove'): o que passou pela UART nos dois
// sentidos e o que o hardware dela reclamou. Bytes fora de quadro sao lixo de
// linha (ou texto de log do no, se ele estiver mandando); estouro de FIFO ou
// de buffer e byte perdido de verdade. Escritos so pela tarefa do servico
// (os de envio, sob s_tx).
static struct {
  uint32_t rx_bytes, tx_bytes, tx_quadros, fora_de_quadro, grandes;
  uint32_t ev_fifo, ev_cheio, ev_quadro, ev_paridade, ev_break;
} s_g;
static volatile bool s_rastro = false;   // 'mesh rastro': cada quadro no log
static QueueHandle_t s_uart_ev = nullptr;
uint32_t s_c_localstats = 0, s_c_texto = 0, s_c_nodeinfo = 0;
int      s_ultima_variante = -1;   // ultimo campo visto dentro de Telemetry

// Quantas vezes um Routing chegou DEPOIS do pacote ja estar ENTREGUE (ver
// marcar_envio). Nao muda o estado mostrado - "confirmado" continua valendo -
// mas e um numero com finalidade propria: se ele cresce rapido, quer dizer
// que a confirmacao "alguem ouviu" esta vindo facil e a confirmacao forte do
// radio (fim a fim) esta vindo dificil - ou seja, a malha ESTA funcionando,
// so que no limite do alcance. E um termometro de qualidade de rota que nao
// existiria se o segundo Routing fosse so descartado sem contar.
static uint32_t s_c_routing_extra = 0;

// ---------------------------------------------------------------------------
// OS ENVIOS EM VOO
// ---------------------------------------------------------------------------
// Anel dos ultimos pacotes que NOS mandamos, com o estado de cada um. Fica so
// em RAM de proposito: o cartao guarda o id junto da mensagem, mas nao o
// estado - reabrir o app amanha e ver "enviando" num pacote de ontem seria
// mentira. Id que nao esta mais aqui devolve MESH_ENVIO_NA, e a tela nao
// desenha marca nenhuma.
#define MESH_MAX_ENVIOS 16

// Quanto esperar pela confirmacao antes de desistir. O radio retransmite
// sozinho algumas vezes e, quando desiste, ELE avisa com Routing.MAX_RETRANSMIT
// - este prazo e so a rede de seguranca para quando o enlace morre no meio.
#define MESH_ACK_MS 60000

typedef struct {
  uint32_t id;
  uint32_t para;     // destino: decide o que um ACK quer dizer (ver marcar_envio)
  uint32_t em_ms;
  uint8_t  estado;
  uint8_t  erro;
} envio_t;

static envio_t  s_envios[MESH_MAX_ENVIOS];
static int      s_prox_envio = 0;
static uint32_t s_envios_ver = 0;

void anotar_envio(uint32_t id, uint32_t para)
{
  if (id == 0) { return; }

  // Ja acompanhado? Nao duplica. Importa porque agora ha DOIS caminhos que
  // chamam isto - o nosso envio e o eco de uma mensagem que saiu do celular -,
  // e a mesma mensagem pode passar pelos dois.
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id == id) { return; }
  }

  envio_t* e = &s_envios[s_prox_envio];
  s_prox_envio = (s_prox_envio + 1) % MESH_MAX_ENVIOS;
  e->id     = id;
  e->para   = para;
  e->em_ms  = tab5_agora_ms();
  e->estado = MESH_ENVIANDO;
  e->erro   = 0;
  s_envios_ver++;
  sistema_avisar(AVISO_MESH_ENVIO);
}

// ---------------------------------------------------------------------------
// O DESFECHO VAI PARA O CARTAO. O caminho, nao.
// ---------------------------------------------------------------------------
// A primeira versao guardava o estado so em RAM, com o argumento de que
// "reabrir amanha e ver ENVIANDO num pacote de ontem seria mentira". O
// argumento vale - para ENVIANDO. Nao vale para o resto.
//
// "Falhou por falta de rota" e um FATO FINAL: ele nao fica mais nem menos
// verdadeiro com o tempo, e some da tela so porque o anel de 16 envios girou ou
// o aparelho reiniciou. Quem mandou a mensagem quer saber que ela nao chegou,
// hoje e semana que vem.
//
// Entao a divisao e por NATUREZA do estado, nao por comodidade:
//   ENVIANDO      - passageiro, so RAM. Nunca gravado.
//   ENTREGUE      - final, gravado.
//   FALHOU        - final, gravado, com o motivo.
//   SEM_RESPOSTA  - final ("desisti de esperar"), gravado.
//
// Arquivo PROPRIO e nao a linha da mensagem: a mensagem e gravada no instante
// do envio, quando o desfecho ainda nao existe. Reescrever aquela linha depois
// significaria reescrever o arquivo inteiro a cada ACK. Aqui e so acrescentar.
static void gravar_desfecho(uint32_t id, uint8_t estado, uint8_t erro)
{
  if (id == 0) { return; }
  char linha[40];
  snprintf(linha, sizeof(linha), "E,%08lX,%u,%u\n", (unsigned long)id, (unsigned)estado, (unsigned)erro);
  tab5_sd_anexar(MESH_PASTA, MESH_ARQ_ENVIOS, linha);   // gravador do sistema (hal/tab5_sd.hpp)
}

// O QUE UM ROUTING QUER DIZER depende de QUEM o mandou ('de') e para onde ia
// o nosso pacote. Silencioso quando o id nao e nosso: a malha esta cheia de
// roteamento que nao nos diz respeito.
//
// O no gera um ACK IMPLICITO quando ouve um vizinho repetir o nosso pacote:
// um Routing com erro 0 vindo DELE MESMO (de == nosso numero). Ele prova que
// a malha ouviu - e, por economia de ar, o no PARA de retransmitir ali.
//   - DIFUSAO: nao ha destinatario para confirmar; o implicito e o melhor que
//     existe, e vale ENTREGUE (o app oficial faz o mesmo).
//   - DIRETA: o implicito e meio caminho - vale OUVIDO. ENTREGUE so com o ACK
//     do PROPRIO destinatario (de == para).
//
// Antes os dois eram iguais: a direta virava "confirmada" no primeiro eco de
// um vizinho, e quando o no depois relatava que o destino nunca respondeu
// (erro 5, MAX_RETRANSMIT), a trava de "ENTREGUE e terminal" engolia o
// relato. A tela dizia entregue para mensagem que nao chegou.
//
// O IMPLICITO DO MQTT NAO E "OUVIDO". Com o C6L ligado na internet, o
// firmware (src/mqtt/MQTT.cpp) gera um ACK implicito para si mesmo quando o
// SERVIDOR devolve o nosso proprio pacote - igual ao de um vizinho, so que
// marcado transport_mechanism = MQTT. Ele prova que o pacote chegou ao
// servidor, nao que algum radio o ouviu. Contado como OUVIDO, uma direta para
// um no DESLIGADO ganhava o check cinza na hora e, como OUVIDO nao desce, o
// tr("tentou demais") que o radio manda depois era engolido: o X so aparecia com
// a internet desligada. O proprio firmware ja ignora esse ACK para parar as
// retransmissoes (ReliableRouter::sniffReceived); aqui ele tambem nao muda a
// marca de uma direta. Na difusao continua valendo ENTREGUE - quem esta no
// servidor recebe, e e a mesma regra de antes.
void marcar_envio(uint32_t id, int erro, uint32_t de, bool so_mqtt)
{
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    envio_t* e = &s_envios[i];
    if (e->id != id || id == 0) { continue; }

    const bool difusao = (e->para == 0xFFFFFFFFu);
    if (so_mqtt && erro == 0 && !difusao && de != e->para) {
      ESP_LOGI(TAG, "mensagem %08lX: chegou ao servidor MQTT - esperando o destino", (unsigned long)id);
      return;
    }
    uint8_t novo;
    if (erro == 0) {
      novo = (difusao || de == e->para) ? MESH_ENTREGUE : MESH_OUVIDO;
    } else {
      novo = MESH_FALHOU;
    }

    // ENTREGUE e terminal: nada posterior o desfaz. OUVIDO so sobe (para
    // ENTREGUE); um erro depois dele nao vira "falhou" - a malha OUVIU, o que
    // faltou foi a confirmacao do destino, e a marca de OUVIDO ja diz isso.
    if (e->estado == MESH_ENTREGUE || (e->estado == MESH_OUVIDO && novo != MESH_ENTREGUE)) {
      if (erro != 0) {
        ++s_c_routing_extra;   // ver o comentario da declaracao - nao e so ruido
        ESP_LOGI(TAG, "mensagem %08lX: aviso '%s' chegou depois de %s - continua assim", (unsigned long)id,
                 mesh_erro_nome((uint8_t)erro), e->estado == MESH_ENTREGUE ? "entregue" : "ouvido");
      }
      return;
    }
    if (e->estado == novo && e->erro == (uint8_t)erro) { return; }
    e->estado = novo;
    e->erro   = (uint8_t)erro;
    s_envios_ver++;
    sistema_avisar(AVISO_MESH_ENVIO);
    gravar_desfecho(id, novo, (uint8_t)erro);
    ESP_LOGI(TAG, "mensagem %08lX: %s%s", (unsigned long)id,
             novo == MESH_ENTREGUE ? "entregue ao destino" : novo == MESH_OUVIDO ? "ouvida por outro no (sem confirmacao do destino)" : "FALHOU - ",
             novo == MESH_FALHOU ? mesh_erro_nome((uint8_t)erro) : "");
    return;
  }
}

uint8_t tab5_mesh_estado_envio(uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
    if (s_envios[i].id != id) { continue; }
    if (erro) { *erro = s_envios[i].erro; }
    return s_envios[i].estado;
  }
  return MESH_ENVIO_NA;
}

uint32_t tab5_mesh_envios_versao(void) { return s_envios_ver; }

const char* mesh_erro_nome(uint8_t erro)
{
  // Routing.Error do meshtastic. So os que da para acontecer aqui; a lista
  // cresce a cada versao do firmware e o resto cai no "?".
  switch (erro) {
    case 0:   return "ok";
    case 1:   return tr("sem rota");
    case 2:   return tr("recusado");
    case 3:   return tr("sem resposta");
    case 4:   return tr("sem interface");
    case 5:   return tr("tentou demais");
    case 6:   return tr("canal errado");
    case 7:   return tr("grande demais");
    case 8:   return tr("ninguém respondeu");
    case 9:   return tr("limite de transmissão");
    case 32:  return tr("pedido inválido");
    case 33:  return tr("sem autorização");
    case 34:  return tr("falha de chave");
    case 35:  return tr("chave desconhecida");
    case 38:  return tr("rápido demais");
    case MESH_ERRO_FILA: return tr("fila do rádio cheia");
    default:  return "?";
  }
}

void travar(void)   { s_trava.travar(); }
void destravar(void){ s_trava.destravar(); }

bool     tab5_mesh_no_ar(void)       { return (tab5_agora_ms() - s_ultimo_quadro_ms) < SILENCIO_MS
                                           && s_ultimo_quadro_ms != 0; }

// HA QUANTO TEMPO O RADIO NAO FALA, em ms. UINT32_MAX = nunca falou.
//
// Existe porque "o enlace esta vivo?" e "alguem arrancou o modulo do cabo?" sao
// perguntas diferentes e querem prazos diferentes. O no_ar() e generoso de
// proposito (90s) para nao chamar de morta uma malha que so esta quieta. A
// vigia do Grove precisa do numero CRU para escolher o proprio prazo - e, com
// ela apoiada no no_ar(), subir o SILENCIO_MS de 30s para 90s empurrou a
// deteccao de troca de modulo de 90s para 150s sem ninguem pedir.
uint32_t tab5_mesh_silencio_ms(void)
{
  if (s_ultimo_quadro_ms == 0) {
    // NENHUM frame AINDA - mas pode ser so porque o servico acabou de
    // subir, nao porque o radio sumiu. MESMO BUG do GPS (ver o aviso grande
    // em tab5_gps_silencio_ms(), achado em hardware):
    // UINT32_MAX incondicional faz a vigia do Grove (kernel/core.cpp) enxergar
    // "mudo ha 60s" no PRIMEIRO tique (ate 2s) depois de QUALQUER reinicio,
    // sem dar ao radio nem os 60s de MUDO_MS que o codigo pretendia dar. Na
    // pratica o C6L costuma responder rapido o bastante para nunca bater
    // nessa janela - mas a malha jamais teve garantia disso, e um radio
    // lento para reconectar (boot frio, ruido) cairia direto no mesmo loop
    // de reinicio a cada 2s que o GPS mostrou. Contar a partir de QUANDO O
    // SERVICO SUBIU, e nao um sentinela fixo, fecha o buraco.
    return s_servico_ativo ? (tab5_agora_ms() - s_iniciado_em_ms) : UINT32_MAX;
  }
  return tab5_agora_ms() - s_ultimo_quadro_ms;
}
uint32_t tab5_mesh_meu_numero(void)  { return s_meu_num; }
const char* tab5_mesh_firmware(void) { return s_firmware; }

void tab5_mesh_definir_beep(bool tocar)
{
  ajustes_definir_beep_mesh(tocar);
  // Se a fonte ainda nao existe (servico nunca subiu nesta sessao), nao ha
  // bandeira para atualizar agora - tab5_mesh_iniciar() aplica a preferencia
  // gravada no instante em que a fonte nascer, ver acima.
  if (s_notif >= 0) { notif_silenciar(s_notif, !tocar); }
}

bool tab5_mesh_beep_ligado(void) { return ajustes_beep_mesh(); }
int      tab5_mesh_nao_lidas(void)   { return s_nao_lidas; }
int      tab5_mesh_nao_lidas_diretas(void) { return s_nao_lidas_dir; }

void tab5_mesh_contadores(uint32_t* quadros, uint32_t* pacotes, uint32_t* telemetria,
                          uint32_t* localstats, uint32_t* texto, uint32_t* nodeinfo,
                          int* ultima_variante)
{
  if (quadros)    *quadros    = s_c_quadros;
  if (pacotes)    *pacotes    = s_c_pacotes;
  if (telemetria) *telemetria = s_c_telemetria;
  if (localstats) *localstats = s_c_localstats;
  if (texto)      *texto      = s_c_texto;
  if (nodeinfo)   *nodeinfo   = s_c_nodeinfo;
  if (ultima_variante) *ultima_variante = s_ultima_variante;
}

// Ver o comentario da declaracao de s_c_routing_extra: nao e ruido, e um
// termometro de qualidade de rota - cresce quando "alguem ouviu" chega facil
// mas a confirmacao forte do radio nao chega.
uint32_t tab5_mesh_routing_extra(void) { return s_c_routing_extra; }

void tab5_mesh_saude(mesh_saude_t* s) { if (s) { travar(); *s = s_saude; destravar(); } }

int tab5_mesh_num_nos(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_NOS; ++i) { if (s_nos[i].usado) { ++n; } }
  destravar();
  return n;
}

uint32_t tab5_mesh_despejos(void) { return s_despejos; }

// O NOSSO NUMERO SOBREVIVE AO DESLIGAMENTO.
//
// Ele so chegava pelo my_info do radio. Sem o C6L plugado valia 0 - e como a
// tela decide o lado do balao por (m->de == meu_numero), TODA mensagem do
// arquivo aparecia como recebida, inclusive as que voce mandou. O arquivo
// abria contando a conversa errada.
//
// Fica na NVS do proprio mesh, e nao em ajustes: ajustes guarda o que a PESSOA
// escolheu, e isto e coisa que o radio contou. Misturar os dois faria o modulo
// de preferencias saber de coisas que nao sao preferencia.

void lembrar_meu_numero(uint32_t num)
{
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_u32(h, "meu_num", num);
  nvs_commit(h);
  nvs_close(h);
}

uint32_t recordar_meu_numero(void)
{
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return 0; }
  if (nvs_get_u32(h, "meu_num", &v) != ESP_OK) { v = 0; }
  nvs_close(h);
  return v;
}

bool tab5_mesh_servico_ativo(void) { return s_servico_ativo; }

// ---------------------------------------------------------------------------
// Tarefa
// ---------------------------------------------------------------------------
// ToRadio{ heartbeat: {} } enquadrado.
//   campo 7, tipo 2 -> tag (7<<3)|2 = 0x3A, comprimento 0
//
// O radio espera que o cliente se manifeste de tempos em tempos; o log do C6L
// mostra "Got client heartbeat" vindo do celular. Sem isto ele pode considerar
// o cliente ausente e parar de entregar trafego - e a ponte fica viva mas muda.
// O ENLACE: entrega o corpo de um ToRadio pela via do servico. Na serial ele
// vai num frame 0x94 0xC3 <tamanho>; no BLE e o proprio valor da
// caracteristica toRadio (ver tab5_mesh_ble.hpp). E o UNICO lugar que sabe
// qual e a via - o resto do servico so monta protobuf.
// Com o no fora e o Wi-Fi de pe, a internet assume (ver usar_internet no
// laco do servico): o que sairia pelo radio vai pelo MQTT.
// Criada uma vez e nunca apagada: o enlace pode ser chamado da interface
// enquanto o servico para e sobe de novo.
static Trava s_tx;

// Um ToRadio do proxy (a descida): o teto do firmware, pelo cabo e pelo BLE.
#define QUADRO_PROXY_MAX 512

bool enlace_enviar(const uint8_t* corpo, size_t n)
{
  // Para o diario de reinicios: o que o Tab5 mandou por ultimo. ToRadio:
  // 1 = pacote, 3 = want_config_id, 7 = batimento.
  if (n > 0) {
    const int campo = corpo[0] >> 3;
    mesh_diario_nota_envio(campo == 1 ? MESH_ENV_PACOTE : campo == 3 ? MESH_ENV_CONFIG
                         : campo == 7 ? MESH_ENV_HEARTBEAT : MESH_ENV_NENHUM);
  }
  if (s_via == AJUSTES_MESH_INTERNET) { return mesh_mqtt_enviar(corpo, n); }
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    if (!mesh_ble_conectado() && mesh_mqtt_conectado()) { return mesh_mqtt_enviar(corpo, n); }
    return mesh_ble_enviar(corpo, n);
  }
  // UM QUADRO POR VEZ NA UART. Cabecalho e corpo sao duas escritas, e
  // escrevem aqui a tarefa do Mesh (batimento, configuracao) e a de interface
  // (texto, favorito, admin). Sem a trava, um batimento podia cair entre o
  // cabecalho e o corpo de uma mensagem: o no lia um tamanho e recebia outro
  // quadro no lugar - as duas se perdiam sem erro nenhum.
  s_tx.travar();
  uint8_t cab[4] = { 0x94, 0xC3, (uint8_t)(n >> 8), (uint8_t)(n & 0xFF) };
  uart_write_bytes(MESH_UART, cab, sizeof(cab));
  uart_write_bytes(MESH_UART, corpo, n);
  s_g.tx_bytes += (uint32_t)(n + 4);
  ++s_g.tx_quadros;
  s_tx.destravar();
  if (s_rastro) { ESP_LOGW("grove", "TX campo %d, %u bytes", n ? (corpo[0] >> 3) : 0, (unsigned)n); }
  return true;
}

static void mandar_heartbeat(void)
{
  const uint8_t q[] = { 0x3A, 0x00 };
  enlace_enviar(q, sizeof(q));
}

// PEDIR A CONFIGURACAO DE NOVO daqui a pouco - depois de gravar algo no no
// (tab5_mesh_admin): ele reinicia e, pela serial, nao fala sozinho.
static volatile uint32_t s_reapresentar_ms = 0;
static volatile bool     s_acordar_proxima = false;

static volatile bool     s_so_config = false;

void tab5_mesh_interno_reapresentar_em(uint32_t ms, bool reiniciou)
{
  // Reiniciado, ele pode estar no modo de baixo consumo da serial e precisar
  // da rajada de 0x94. Sem reinicio, NAO: a rajada no meio de uma conversa e
  // ruido na maquina de quadros dele (ver pedir_apresentacao).
  // Sem reinicio (posicao fixa posta ou tirada) so a CONFIGURACAO mudou: o
  // pedido e o "so configuracao", sem a NodeDB inteira atras.
  if (reiniciou) { s_acordar_proxima = true; s_so_config = false; }
  else           { s_so_config = true; }
  s_reapresentar_ms = (tab5_agora_ms() + ms) | 1u;
}

static void pedir_apresentacao(void)
{
  // A RAJADA DE 0x94 SO NA PRIMEIRA VEZ.
  //
  // Ela existe para tirar o radio do modo de baixo consumo da serial - e o
  // procedimento documentado de acordar. Mas sao 32 bytes de lixo entrando na
  // maquina de enquadramento DELE, e mandar isso com a conversa ja estabelecida
  // e injetar ruido no meio de um frame que pode estar chegando ao mesmo
  // tempo. Depois do primeiro contato o radio esta acordado por definicao: ele
  // acabou de falar conosco.
  if ((s_meu_num == 0 || s_acordar_proxima) && s_via == AJUSTES_MESH_SERIAL) {   // BLE nao tem modo de baixo consumo da serial
    s_acordar_proxima = false;
    uint8_t acorda[32];
    memset(acorda, 0x94, sizeof(acorda));
    s_tx.travar();
    uart_write_bytes(MESH_UART, acorda, sizeof(acorda));
    s_tx.destravar();
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  // SO A CONFIGURACAO quando so ela mudou: want_config_id 69420 e o
  // SPECIAL_NONCE_ONLY_CONFIG do firmware (PhoneAPI) - canais, config e
  // modulos, sem a NodeDB, que pelo Bluetooth com uma malha cheia sao
  // centenas de quadros so para saber que um campo virou. Firmware antigo nao
  // conhece o numero e manda tudo, como antes. Qualquer outro motivo (primeiro
  // contato, reinicio, silencio) pede tudo.
  const bool so_config = s_so_config && s_meu_num != 0;
  s_so_config = false;
  static const uint8_t TUDO_Q[]   = { 0x18, 0x2A };               // ToRadio{ want_config_id: 42 }
  static const uint8_t CONFIG_Q[] = { 0x18, 0xAC, 0x9E, 0x04 };   // ToRadio{ want_config_id: 69420 }
  s_config_ok_ms = 0;   // dump novo: o que vier ate o fim dele e passado
  mesh_admin_inicio_dump();
  if (so_config) { enlace_enviar(CONFIG_Q, sizeof(CONFIG_Q)); }
  else           { enlace_enviar(TUDO_Q, sizeof(TUDO_Q)); }
}

// ---------------------------------------------------------------------------
// Envio
// ---------------------------------------------------------------------------
// Texto, admin e posicao saem no MESMO envelope ToRadio{ packet: MeshPacket{
// to, channel, decoded: Data{ portnum, payload }, id, hop_limit, want_ack } },
// montado num lugar so: mesh_pb_to_radio() (services/mesh_pb.hpp).

// ---------------------------------------------------------------------------
// FILA DE ENVIO DE TEXTO
// ---------------------------------------------------------------------------
typedef struct { uint16_t n; uint32_t id; uint8_t d[MESH_TEXTO + 96]; } envio_fila_t;
static QueueHandle_t s_fila_envio = nullptr;
#define MESH_FILA_ENVIO 8

// Ja da para dizer que nao sai? (ver tab5_mesh_enviar)
static bool pode_sair(uint32_t para)
{
  const bool difusao = (para == 0xFFFFFFFFu);
  if (s_via == AJUSTES_MESH_INTERNET) { return difusao && mesh_mqtt_conectado(); }
  if (s_via == AJUSTES_MESH_BLUETOOTH && !mesh_ble_conectado()) {
    return difusao && mesh_mqtt_conectado() && !mesh_mqtt_em_proxy();   // a internet cobrindo a falta dele
  }
  return true;
}

// DOIS SEGUNDOS ENTRE TEXTOS. O firmware descarta o texto que chega do
// cliente menos de 2 s depois do anterior (PhoneAPI, rate limit da porta TEXT,
// da 2.5 ate a atual): na 2.7 sem erro nenhum - a mensagem ficava "enviando" e
// virava tr("sem resposta"). A fila espera o que falta. Chamada pela tarefa do
// servico, sem trava.
static void laco_envios(void)
{
  static uint32_t ultimo_ms = 0;
  static envio_fila_t e;
  if (!s_fila_envio) { return; }
  for (int k = 0; k < 4; ++k) {
    if (xQueuePeek(s_fila_envio, &e, 0) != pdTRUE) { return; }
    if (ultimo_ms && tab5_agora_ms() - ultimo_ms < 2100) { return; }
    xQueueReceive(s_fila_envio, &e, 0);
    const bool ok = enlace_enviar(e.d, e.n);
    ultimo_ms = tab5_agora_ms();
    if (!ok) {
      // O enlace caiu entre o toque e a vez dela: falhou, e a tela diz.
      ESP_LOGW(TAG, "pacote %08lX: o enlace recusou", (unsigned long)e.id);
      travar();
      marcar_envio(e.id, MESH_ERRO_FILA, s_meu_num);
      destravar();
    }
  }
}

bool tab5_mesh_enviar(uint32_t para, uint8_t canal, const char* texto)
{
  if (!texto || !texto[0]) { return false; }

  // SEM SERVICO NAO HA PARA ONDE ESCREVER. A tela ja impede chegar aqui, mas a
  // guarda mora TAMBEM aqui de proposito: sem servico nao ha UART instalada, e
  // um uart_write_bytes numa porta que nao existe nao e "nao enviou" - e
  // comportamento indefinido. Quem protege o hardware e a camada do hardware.
  if (!s_servico_ativo) {
    ESP_LOGW(TAG, "envio recusado: servico parado (sem no Meshtastic)");
    return false;
  }
  // ENLACE MUDO NAO E MOTIVO PARA RECUSAR, e a recusa era pior que o problema.
  //
  // "no ar" aqui significa so "chegou algum frame do radio nos ultimos 30s".
  // Numa malha parada isso e o estado NORMAL: o radio nao fala sozinho, e nao
  // ha nada de errado com o cabo. Recusando, o botao ENV nao fazia nada, sem
  // mensagem de erro e sem entrada no historico - do lado de fora e
  // indistinguivel de um sistema quebrado, e foi assim que ficou o tempo todo.
  //
  // Mandar e o certo: se o radio nao estiver mesmo la, o queueStatus nao vem, o
  // ACK nao vem, e o prazo de 60s marca a mensagem como tr("sem resposta") na
  // propria tela. Isso e informacao; o silencio nao era.
  if (!tab5_mesh_no_ar()) {
    ESP_LOGW(TAG, "enlace sem quadro recente - enviando assim mesmo");
  }

  // No maximo MESH_PAYLOAD_MAX (233) bytes - acima disso o no descarta o
  // pacote inteiro em silencio (ver o .hpp). Corta na fronteira de caractere:
  // um acento partido ao meio sai como lixo na tela de quem recebe.
  size_t n_txt = strnlen(texto, MESH_PAYLOAD_MAX + 1);
  if (n_txt > MESH_PAYLOAD_MAX) {
    n_txt = MESH_PAYLOAD_MAX;
    while (n_txt > 0 && ((unsigned char)texto[n_txt] & 0xC0) == 0x80) { --n_txt; }
    ESP_LOGW(TAG, "texto acima de %d bytes - enviado cortado", MESH_PAYLOAD_MAX);
  }
  char enviado[MESH_PAYLOAD_MAX + 1];
  memcpy(enviado, texto, n_txt);
  enviado[n_txt] = 0;

  mesh_pb_envelope_t env = { };
  env.para    = para;
  env.canal   = canal;
  env.porta   = PORT_TEXTO;
  env.carga   = (const uint8_t*)texto;
  env.n_carga = n_txt;
  // ID PROPRIO, e e ele que torna a confirmacao possivel. Sem id o radio
  // inventa um, e nunca ficamos sabendo qual - o ACK chegaria com um
  // request_id que nao bate com nada nosso. Zero e reservado, por isso o |1.
  env.id = esp_random() | 1u;
  // hop_limit 3. Sem ele o pacote sai com ZERO SALTOS - so quem ouve a nossa
  // antena diretamente recebe, e nada e repetido adiante. 3 e o padrao do
  // proprio Meshtastic (nao lemos a Config do radio para herdar o dele).
  env.saltos = 3;
  // want_ack nos DOIS casos - tambem na difusao. La nao ha destinatario para
  // confirmar, mas ha a CONFIRMACAO IMPLICITA: quando um vizinho repete o
  // pacote, o firmware devolve um Routing com o nosso request_id. E a unica
  // prova de que alguem ouviu - o mesmo que o app oficial faz num canal.
  env.quer_ack = true;
  const uint32_t id = env.id;

  // O enquadramento (se houver) e do enlace.
  uint8_t quadro[MESH_TEXTO + 96];
  const size_t n_quadro = mesh_pb_to_radio(&env, quadro, sizeof(quadro));
  if (!n_quadro) { ESP_LOGE(TAG, "texto nao coube no quadro"); return false; }

  // A RECUSA QUE DA PARA SABER AGORA sai agora, com o envio falhando na
  // tela: a internet (so canal - a direta precisa de PKI), o Bluetooth sem
  // conexao. O resto e decidido pelo enlace, na fila.
  if (!pode_sair(para)) {
    ESP_LOGW(TAG, "envio recusado: %s", tab5_mesh_enlace_descricao());
    return false;
  }

  // NA FILA, NAO NA HORA. Quem chama e a tarefa de INTERFACE, e o envio pode
  // ter de esperar o limite de 2 s do no entre textos (ver laco_envios) -
  // esperar aqui congelava a tela. A mensagem aparece na conversa ja como
  // "enviando"; a tarefa do servico a entrega no ritmo que o no aceita.
  if (!s_fila_envio) { return false; }
  static envio_fila_t e;   // so a interface envia texto; 300 bytes fora da pilha dela
  e.n = (uint16_t)n_quadro;
  e.id = id;
  memcpy(e.d, quadro, e.n);
  if (xQueueSend(s_fila_envio, &e, 0) != pdTRUE) {
    ESP_LOGW(TAG, "fila de envio cheia - mensagem nao saiu");
    return false;
  }

  // Guarda a nossa propria mensagem: a tela mostra as suas a direita, e sem
  // isto ela sumiria - o radio nao devolve o que voce mesmo enviou.
  travar();
  anotar_envio(id, para);
  // Sem remendo no contador: o guardar_msg ja sabe que a mensagem e nossa
  // (de == s_meu_num) e nao a conta como nao lida, nem aqui nem na conversa.
  // via_mqtt = false: e a NOSSA transmissao saindo pelo nosso proprio radio,
  // nunca chega pela ponte.
  // saltos = desconhecido: e a NOSSA mensagem, ainda nao viajou nenhum salto
  // do ponto de vista de quem recebe - e o meta-line nem aparece para
  // mensagem MINHA de qualquer forma (ver desenhar_msgs em app_mesh.cpp).
  // A HORA DO ENVIO, em UTC como a rx_time do radio. Era 0 - e
  // 0 no fuso -3 vira 31/12/1969 na tela. Relogio ainda nao acertado (sem GPS
  // nem malha desde o boot) continua 0 = "sem data", que a tela sabe tratar.
  const uint32_t quando = tab5_hora_epoch();
  guardar_msg(s_meu_num, para, canal, enviado, quando, 0, 0, id, false, MESH_SALTOS_DESCONHECIDO);
  lembrar_carimbo(id, quando);   // corrigido se o relogio saltar (ao_saltar_relogio)
  destravar();

  ESP_LOGI(TAG, "enviado para %08lX (canal %u) id=%08lX: %s",
           (unsigned long)para, (unsigned)canal, (unsigned long)id, enviado);
  return true;
}

// Quem somos, para o cliente MQTT: ele publica e responde o want_config em
// nome deste numero e destes nomes. Chamado antes de subir o cliente.
// UM NUMERO SO DESTE TAB5, para quando nunca houve no: os 4 ultimos bytes do
// MAC, a mesma conta que o Meshtastic faz para o numero de um no. Antes ficava
// 0 - todo Tab5 sem no (o firmware do M5Burner, usado so pela internet) entrava
// no servidor como "tab5-00000000" e com o remetente !00000000: os clientes de
// mesmo id se derrubavam uns aos outros, e as mensagens de todos pareciam de
// um so. Nao vai para a NVS: o numero de verdade e o do no, quando ele vier.
static uint32_t numero_do_tab5(void)
{
  uint8_t mac[6] = { 0 };
  if (esp_efuse_mac_get_default(mac) != ESP_OK) { esp_read_mac(mac, ESP_MAC_BASE); }
  const uint32_t n = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  return n ? n : 1u;
}

static void informar_no_ao_mqtt(void)
{
  if (s_meu_num == 0) {
    s_meu_num = numero_do_tab5();
    ESP_LOGI(TAG, "sem no conhecido: o Tab5 fala pela internet como !%08lx", (unsigned long)s_meu_num);
  }
  char curto[MESH_NOME_CURTO] = "", longo[MESH_NOME_LONGO] = "";
  tab5_mesh_nome_cache(s_meu_num, curto, sizeof(curto));
  tab5_mesh_nome_longo_cache(s_meu_num, longo, sizeof(longo));
  mesh_mqtt_definir_no(s_meu_num, curto, longo);
}

// AS GRAVACOES LENTAS DO MESH, NA CARONA DA TAREFA "cartao" (tab5_sd_carona).
// Eram feitas no laco do RADIO: enquanto a NodeDB ia para o cartao (montar,
// escrever dezenas de KB, fsync) ninguem lia a UART, e o buffer de 4 KB (~350
// ms a 115200) transbordava num cartao lento - quadro do no perdido. A regra
// do sistema (hal/tab5_sd.hpp) e esta: quem so esvazia dados no cartao pega
// carona na tarefa dele. Canais: a cada 15 s se mudaram (e logo depois do
// dump, que os marca). NodeDB: a cada 10 min se mudou.
static void mesh_na_carona(void)
{
  if (!s_servico_ativo) { return; }
  const uint32_t t = tab5_agora_ms();
  static uint32_t proximos_canais = 0, proxima_nodedb = 600000;
  if (s_canais_sujos && (int32_t)(t - proximos_canais) >= 0) {
    proximos_canais = t + 15000;
    salvar_canais();
  }
  if ((int32_t)(t - proxima_nodedb) >= 0) {
    proxima_nodedb = t + 600000;
    if (s_nos_ver != s_nodedb_ver_salva) { salvar_nodedb(); }
  }
}

static void vTaskMesh(void*)
{
  static uint8_t quadro[1024];
  size_t usado = 0;
  int    estado = 0;          // 0=procura 0x94  1=viu 0x94  2=tamanho  3=corpo
  size_t falta = 0;
  uint8_t tam_hi = 0;
  uint32_t proxima_apresentacao = 0;
  uint32_t proximo_heartbeat = 15000;   // primeiro so depois do dump inicial

  // WATCHDOG: um deadlock aqui e SILENCIOSO e caro de um jeito especifico -
  // esta tarefa segura o mutex do mesh (travar()/destravar()) em varios
  // trechos, e travada DENTRO de um deles o mutex nunca seria devolvido. A
  // tela nao trava (ela so pediria o mutex e ficaria esperando, mas o loop de
  // interface segue vivo por fora), entao o sintoma seria "mensagens paradas
  // de chegar" sem nenhum log dizendo por que - dificil de distinguir de um
  // radio mudo de verdade. Ver o comentario igual em kernel/core.cpp para o
  // raciocinio completo.
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    ESP_LOGW(TAG, "nao consegui inscrever a tarefa no watchdog");
  }

  for (;;)
  {
    // PONTO DE SAIDA. No topo do loop, fora de qualquer secao critica: aqui a
    // tarefa nao segura mutex nem arquivo, entao sumir daqui nao deixa nada
    // pela metade. Como o uart_read_bytes la embaixo espera no maximo 200ms, a
    // parada custa isso no pior caso.
    if (parada_pedida(&s_parada)) {
      // DESINSCREVE ANTES DE MORRER. O watchdog guarda o HANDLE desta tarefa;
      // deletando sem avisar ele antes, o handle fica pendurado la dentro e o
      // proximo ciclo de verificacao do watchdog encontraria uma tarefa que
      // nao existe mais - o tipo de "fantasma" que este sistema anda cacando
      // em outros lugares (Grove, apresentacao do radio). A ordem importa:
      // desinscrever so funciona enquanto a tarefa ainda existe para fazer a
      // chamada.
      esp_task_wdt_delete(NULL);
      parada_confirmar(&s_parada);
      return;   // a vaga do pool devolve o dono e volta a dormir (kernel/pool_tarefas.h)
    }

    // PROVA DE VIDA. Chegar aqui de novo significa que a volta anterior
    // terminou - nenhum uart_read_bytes, trava de mutex ou I/O de cartao
    // ficou preso. 200ms e o maior bloqueio que este loop faz de proposito
    // (o prazo do uart_read_bytes la embaixo); bem dentro dos 5s do watchdog.
    esp_task_wdt_reset();

    const uint32_t t = tab5_agora_ms();

    // Apresentacao SO quando o enlace esta mudo. Pedir de novo com o enlace
    // vivo faz o radio REINICIAR o download de configuracao inteiro - foi o que
    // produziu 11 node_info para 6 nos: o dump rodou duas vezes.
    // O radio avisou que reiniciou: pede a configuracao NA HORA. Sem isto a
    // gente esperava o enlace ficar mudo e o prazo de 10s vencer - e nesse
    // meio-tempo a lista de nos e os canais ficavam do reinicio anterior.
    mesh_admin_tique(t);
    if (s_reapresentar_ms && (int32_t)(t - s_reapresentar_ms) >= 0) {
      s_reapresentar_ms = 0;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (s_radio_reiniciou) {
      s_radio_reiniciou = false;
      proxima_apresentacao = t + 10000;
      pedir_apresentacao();
    }
    else if (!tab5_mesh_no_ar() && t > proxima_apresentacao) {
      // DOIS RITMOS, e a diferenca entre eles e o que resolve.
      //
      // Sem o my_info nao sabemos nem o nosso proprio numero: nada funciona, e
      // insistir de 10 em 10 segundos e o certo - e o primeiro contato.
      //
      // COM ele, ja conversamos com este radio. Um silencio agora quer dizer
      // "a malha esta quieta", nao "o radio sumiu" - e pedir a configuracao de
      // novo faz o C6L REINICIAR o download inteiro, jogando fora o trafego ao
      // vivo que estiver na queue dele. Era um dump a cada 30s, para sempre, na
      // maior parte do tempo. De dois em dois minutos ainda recupera um radio
      // que travou, sem transformar a conversa num ciclo de reapresentacoes.
      const uint32_t espera = (s_meu_num == 0) ? 10000 : 120000;
      proxima_apresentacao = t + espera;
      ESP_LOGI(TAG, "sem quadro ha %d s - pedindo a configuracao ao radio",
               SILENCIO_MS / 1000);
      pedir_apresentacao();
    }

    // Nomes de no pendentes, de 15 em 15 segundos. Espacado de proposito: e o
    // que transforma a rajada de montagens do boot numa unica escrita.
    static uint32_t proxima_descarga = 15000;
    if (t > proxima_descarga) {
      proxima_descarga = t + 15000;
      descarregar_nomes();   // so entrega ao gravador (tab5_sd_anexar): nao bloqueia
    }
    // Canais (NVS) e NodeDB (cartao) NAO sao gravados aqui: ver mesh_na_carona.

    // Prazo dos envios sem resposta. Rede de seguranca: quando o radio desiste
    // por conta propria ele AVISA (Routing.MAX_RETRANSMIT), entao chegar aqui
    // significa que o enlace morreu no meio - e "nao sei" e o que a tela deve
    // dizer, nao "falhou".
    for (int i = 0; i < MESH_MAX_ENVIOS; ++i) {
      if (s_envios[i].estado == MESH_ENVIANDO
       && (t - s_envios[i].em_ms) > MESH_ACK_MS) {
        s_envios[i].estado = MESH_SEM_RESPOSTA;
        s_envios_ver++;
        sistema_avisar(AVISO_MESH_ENVIO);
        gravar_desfecho(s_envios[i].id, MESH_SEM_RESPOSTA, 0);
        ESP_LOGW(TAG, "pacote %08lX: sem confirmacao em %d s",
                 (unsigned long)s_envios[i].id, MESH_ACK_MS / 1000);
      }
    }

    laco_envios();   // texto na fila (ver tab5_mesh_enviar)

    // Batimento periodico. Mantem o radio convencido de que ha cliente do outro
    // lado; sem ele o trafego ao vivo pode simplesmente parar de vir.
    if (t > proximo_heartbeat) {
      proximo_heartbeat = t + 30000;
      mandar_heartbeat();
    }

    // LEITURA EM BLOCO, e a diferenca e de energia, nao de elegancia.
    //
    // Antes era uma chamada por BYTE, com prazo de 200ms. A 115200 baud isso
    // chega a 11.520 chamadas por segundo, cada uma com travessia de queue e
    // troca de contexto - e roda o tempo todo, porque isto e servico, nao app.
    //
    // Lendo em bloco, um pacote inteiro sai de UMA chamada. A tarefa acorda
    // poucas vezes por segundo em vez de milhares, e o core volta a ficar
    // ocioso entre elas - que e o que permite ao DFS trabalhar.
    // POSICAO DO GPS PARA A MALHA - ver enviar_posicao().
    static uint32_t proxima_posicao = 60000;
    if (ajustes_mesh_posicao_min() && t > proxima_posicao) {
      proxima_posicao = t + (enviar_posicao() ? ajustes_mesh_posicao_min() * 60000u : 30000u);
    }

    // O CLIENTE MQTT, EM UM DE DOIS PAPEIS (ver services/tab5_mesh_mqtt.hpp):
    //   INTERNET - o Tab5 fala com a malha pelo servidor, quando o no nao esta
    //              (ou quando a via escolhida e a Internet);
    //   PROXY    - o no esta conectado e com o proxy ligado: o Tab5 so carrega
    //              o que ELE publica e assina (proxy ligado no no).
    // Os dois sao exclusivos por construcao: o proxy exige o no presente, a
    // internet como tapa-buraco exige a falta dele. Com folga nas duas
    // direcoes: o BLE cai e volta sozinho numa reconexao, e o servidor nao
    // deve ser aberto e fechado a cada piscada dele.
    {
      enum { MQ_NADA = 0, MQ_NET, MQ_PROXY };
      static int      modo_pedido = MQ_NADA;
      static uint32_t modo_desde = 0;
      // DOIS JEITOS DE A INTERNET ESTAR AQUI:
      //   - ela E a via (escolhida em Ajustes, ou o kernel subiu o servico so
      //     por ela): roda com o Wi-Fi, e ponto. O C6L no cabo nao a derruba
      //     daqui - quem decide trocar para o Grove e a sondagem do kernel, ao
      //     ACHAR o no (a prioridade do Grove), nao este laco a cada volta. Com
      //     o teste do Grove aqui, escolher "internet" com o C6L plugado
      //     derrubava o MQTT logo depois de subir.
      //   - a via e o no (cabo ou Bluetooth): a internet so cobre a falta
      //     dele, se o ajuste "internet quando sem o no" estiver ligado.
      const bool wifi = tab5_wifi_estado() == TAB5_WIFI_CONECTADO;
      const bool no_ok = (s_via == AJUSTES_MESH_SERIAL)
                      || (s_via == AJUSTES_MESH_BLUETOOTH && mesh_ble_conectado())
                      || tab5_grove_tipo() == GROVE_MESHTASTIC;
      const bool quer_net = wifi && ((s_via == AJUSTES_MESH_INTERNET)
                                  || (ajustes_mesh_internet() && !no_ok));
      // O proxy pede o no CONVERSANDO (nao so a via escolhida) e com o dump
      // completo: e o dump que diz se o proxy esta ligado nele e quais canais
      // descem.
      const bool no_vivo = (s_via == AJUSTES_MESH_SERIAL && tab5_mesh_no_ar())
                        || (s_via == AJUSTES_MESH_BLUETOOTH && mesh_ble_conectado());
      // A CHAVE E DO NO: liga-se o proxy na pagina "No" do app Mesh, que grava
      // proxy_to_client_enabled nele. Ligado no no, o Tab5 empresta a internet.
      const bool quer_proxy = wifi && no_vivo && s_config_ok_ms != 0 && mesh_mqtt_no_tem_proxy();
      const int modo = quer_proxy ? MQ_PROXY : quer_net ? MQ_NET : MQ_NADA;
      int atual = !mesh_mqtt_ativo() ? MQ_NADA : mesh_mqtt_em_proxy() ? MQ_PROXY : MQ_NET;
      if (modo != modo_pedido) { modo_pedido = modo; modo_desde = t; }
      // Descer: o no que acabou de voltar tira a internet NA HORA (o proxy vai
      // subir); o resto espera 10 s firmes, para nao derrubar numa piscada.
      if (atual != MQ_NADA && atual != modo
       && (modo == MQ_PROXY || t - modo_desde >= 10000)) {
        mesh_mqtt_parar();
        atual = MQ_NADA;
      }
      // Subir: a internet como tapa-buraco espera 20 s (o BLE costuma voltar
      // antes disso); a via Internet e o proxy sobem logo.
      const uint32_t folga = (modo == MQ_NET && s_via != AJUSTES_MESH_INTERNET) ? 20000 : 0;
      if (atual == MQ_NADA && modo != MQ_NADA && t - modo_desde >= folga) {
        informar_no_ao_mqtt();
        if (modo == MQ_PROXY) { mesh_mqtt_iniciar_proxy(); } else { mesh_mqtt_iniciar(); }
        atual = !mesh_mqtt_ativo() ? MQ_NADA : modo;
      }
      if (atual == MQ_NET) {
        if (mesh_mqtt_conectou_agora() && s_via == AJUSTES_MESH_INTERNET) { s_radio_reiniciou = true; }
        // ESVAZIA A FILA, nao um por volta: com o no no cabo a volta espera ate
        // 200 ms na serial, e um pacote por volta (5/s) perdia a rajada do
        // servidor. So o primeiro espera (na via Internet e o ritmo do laco).
        for (int k = 0; k < 16; ++k) {
          const size_t n = mesh_mqtt_receber(quadro, sizeof(quadro),
                                             (k == 0 && s_via == AJUSTES_MESH_INTERNET) ? 200 : 0);
          if (n == 0) { break; }
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, n);
          s_ultimo_quadro_ms = tab5_agora_ms();
          destravar();
        }
      } else if (atual == MQ_PROXY) {
        // DESCIDA DO PROXY: o que veio do servidor vai ao no. Ate 8 por volta -
        // a volta dura no maximo os 200 ms da leitura do enlace, e a fila (32)
        // segura a rajada no meio-tempo. Eram 4 (20/s), e um servidor publico
        // entregando o canal inteiro enchia a fila. Mais que 8 seguraria a
        // volta escrevendo na serial/Bluetooth.
        static uint8_t desce[QUADRO_PROXY_MAX];
        for (int k = 0; k < 8; ++k) {
          const size_t n = mesh_mqtt_proxy_receber(desce, sizeof(desce));
          if (n == 0) { break; }
          enlace_enviar(desce, n);
        }
      }
      if (s_via == AJUSTES_MESH_INTERNET) {
        if (!mesh_mqtt_ativo()) { vTaskDelay(pdMS_TO_TICKS(200)); }
        continue;
      }
    }

    // VIA BLUETOOTH: o enlace ja entrega FromRadio inteiro, sem frame - nao
    // ha maquina de enquadramento. Mesmo prazo de 200 ms da serial.
    if (s_via == AJUSTES_MESH_BLUETOOTH) {
      if (mesh_ble_conectou_agora()) { s_radio_reiniciou = true; }   // pede a configuracao ja
      // O que ja chegou de uma vez (o dump vem em rajada); so o primeiro espera.
      for (int k = 0; k < 16; ++k) {
        const size_t n = mesh_ble_receber(quadro, sizeof(quadro), k == 0 ? 200 : 0);
        if (n == 0) { break; }
        travar();
        s_c_quadros++;
        ler_from_radio(quadro, n);
        s_ultimo_quadro_ms = tab5_agora_ms();
        destravar();
      }
      continue;
    }

    // ACORDA NO PRIMEIRO BYTE, LE O RESTO NA HORA. uart_read_bytes so volta
    // com o tamanho pedido OU no prazo: pedindo 256, um quadro de 60 bytes
    // ficava esperando os 200 ms inteiros - cada mensagem chegava atrasada, e o
    // laco inteiro (fila de envio, internet) andava nesse passo. Esperar
    // UM byte dorme igual quando a serial esta quieta e acorda quando ela fala.
    static uint8_t entrada[512];
    // O QUE O HARDWARE DA UART RECLAMOU desde a ultima volta (ver s_g).
    if (s_uart_ev) {
      uart_event_t ev;
      while (xQueueReceive(s_uart_ev, &ev, 0) == pdTRUE) {
        switch (ev.type) {
          case UART_FIFO_OVF:    ++s_g.ev_fifo; break;
          case UART_BUFFER_FULL: ++s_g.ev_cheio; break;
          case UART_FRAME_ERR:   ++s_g.ev_quadro; break;
          case UART_PARITY_ERR:  ++s_g.ev_paridade; break;
          case UART_BREAK:       ++s_g.ev_break; break;
          default: break;
        }
        if (s_rastro && ev.type != UART_DATA) { ESP_LOGW("grove", "evento da UART %d", (int)ev.type); }
      }
    }
    int lidos = uart_read_bytes(MESH_UART, entrada, 1, pdMS_TO_TICKS(200));
    if (lidos <= 0) { continue; }
    size_t pronto = 0;
    if (uart_get_buffered_data_len(MESH_UART, &pronto) == ESP_OK && pronto > 0) {
      if (pronto > sizeof(entrada) - 1) { pronto = sizeof(entrada) - 1; }
      const int mais = uart_read_bytes(MESH_UART, entrada + 1, pronto, 0);
      if (mais > 0) { lidos += mais; }
    }

    s_g.rx_bytes += (uint32_t)lidos;
    for (int k = 0; k < lidos; ++k) {
    const uint8_t b = entrada[k];

    switch (estado) {
      case 0: if (b == 0x94) { estado = 1; } else { ++s_g.fora_de_quadro; } break;
      case 1: estado = (b == 0xC3) ? 2 : (b == 0x94 ? 1 : 0); break;
      case 2: tam_hi = b; estado = 3; break;
      case 3: {
        falta = ((size_t)tam_hi << 8) | b;
        usado = 0;
        if (falta == 0) { estado = 0; }
        else if (falta > sizeof(quadro)) {
          // PULA O CORPO, nao volta a procurar cabecalho dentro dele.
          //
          // Voltando ao estado 0 a maquina passava a varrer os proprios bytes
          // do frame descartado atras de 0x94 0xC3 - e essa dupla aparece por
          // acaso em dado binario. Encontrando uma, ela lia um "tamanho" que e
          // lixo e engolia o frame BOM que vinha depois. Uma mensagem some, e
          // nada no log diz por que.
          ESP_LOGW(TAG, "quadro de %u bytes nao cabe - pulando", (unsigned)falta);
          ++s_g.grandes;
          estado = 5;
        }
        else { estado = 4; }
        break;
      }
      case 5:                      // engolindo um frame grande demais
        if (--falta == 0) { estado = 0; }
        break;
      case 4:
        quadro[usado++] = b;
        if (usado >= falta) {
          if (s_rastro) {
            int cf = usado ? (quadro[0] >> 3) : 0;
            if (cf == 1 && usado > 5) { cf = quadro[5] >> 3; }   // o id (fixo32) antes da variante
            ESP_LOGW("grove", "RX campo %d, %u bytes", cf, (unsigned)usado);
          }
          travar();
          s_c_quadros++;
          ler_from_radio(quadro, usado);
          s_ultimo_quadro_ms = tab5_agora_ms();
          destravar();
          estado = 0;
        }
        break;
    }
    }   // for k
  }
}

// PARA O SERVICO E DEVOLVE O GROVE.
//
// Existe para o caso que o sistema simplesmente nao cobria: trocar o modulo do
// cabo com o aparelho ligado. Sem isto o servico ficava de pe conversando com
// um C6L que nao estava mais la, e o GPS recem-plugado nunca era procurado
// porque a porta tinha dono.
//
// Devolve false quando NAO conseguiu parar - e nesse caso nao limpa nada. Se a
// tarefa nao confirmou que saiu, ela ainda pode estar viva: apagar a UART e o
// mutex debaixo dela trocaria um servico teimoso por uma pane.
bool tab5_mesh_parar(void)
{
  if (!s_servico_ativo) { return true; }   // ja parado; pedir de novo nao e erro

  ESP_LOGI(TAG, "parando o servico...");
  // A tarefa espera no maximo 200 ms na UART (ou na fila do enlace): a saida
  // custa uma volta. 1 s e folga larga. Sem confirmacao, o pedido FICA DE PE
  // e nada e desmontado - a proxima chamada termina (ver parada_pedir).
  if (!parada_pedir(&s_parada, 1000)) {
    ESP_LOGE(TAG, "a tarefa nao confirmou em 1s - segue parando, nada foi desmontado");
    return false;
  }

  // Daqui para baixo a tarefa NAO EXISTE MAIS. So agora e seguro desmontar o
  // que ela usava.

  // TAREFA DE GRAVACAO PRIMEIRO - independente da UART/mutex acima (so
  // depende da propria queue), mas rodando ate aqui. 300ms de folga: uma
  // escrita costuma levar bem menos, e nao ha nada em andamento na tarefa
  // "mesh" que ainda vá enfileirar mais nada (ja confirmou saida). Qualquer
  // pedido ainda na queue quando ela sair e descartado - mesmo trade-off
  // aceito em toda queue deste sistema que enfileira E/S (ver o aviso grande
  // junto de MESH_FILA_GRAVACAO).

  // O que estava pendente vai agora: a tarefa que faria a descarga acabou de
  // morrer, e sem isto os nomes aprendidos nesta sessao se perderiam.
  descarregar_nomes();

  mesh_mqtt_parar();
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    mesh_ble_parar();
  } else if (s_via == AJUSTES_MESH_SERIAL) {
    uart_driver_delete(MESH_UART); s_uart_ev = nullptr;   // a fila de eventos era do driver
    tab5_grove_soltar(GROVE_DONO_MESH);
    sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", false);
  }

  // O AVISO SO SAI SE NAO HA NADA PARA LER. Antes ele era apagado a cada
  // parada - e o Mesh para sozinho (troca de via, Wi-Fi caindo): o aviso
  // sumia sem o usuario ter lido nada. As nao lidas ficam guardadas e voltam
  // com as conversas; o que esta publicado agora continua valendo.
  guardar_nao_lidas();
  if (s_notif >= 0 && !ha_nao_lidas_guardadas()) { notif_limpar(s_notif); }
  salvar_canais();   // o que a carona ainda nao levou (ela para com o servico)
  salvar_nodedb();   // antes do memset da tabela, logo abaixo
  // O MUTEX FICA. Ele era apagado aqui e recriado no proximo iniciar - mas
  // as consultas publicas (a barra da home, o app Mesh) o tomam de OUTRAS
  // tarefas a qualquer momento: apagar um mutex preso ou com alguem
  // esperando nele e memoria liberada em uso, e entre o apagar e o recriar
  // as leituras corriam sem exclusao nenhuma. Criado uma vez, vale para
  // sempre (o mesmo de tab5_sd).

  // Enlace zerado: sem isto o tab5_mesh_no_ar() continuaria respondendo pelo
  // ultimo frame recebido, e a barra diria "conectado" a um radio ausente.
  s_ultimo_quadro_ms = 0;
  s_config_ok_ms = 0;
  // s_meu_num NAO e zerado: nosso numero nao muda porque o cabo saiu, e o
  // arquivo precisa dele para saber de quem e cada mensagem.
  s_n_msgs = 0;   // a contagem de NOS nao tem variavel: sai do memset abaixo
  s_total_msgs = 0;
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  // Sob a trava: a interface pode estar lendo as tabelas agora mesmo (a
  // barra da home, o app Mesh) - antes isto rodava com o mutex ja apagado.
  travar();
  memset(s_nos,  0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  ++s_conv_ver;
  destravar();

  s_servico_ativo = false;
  sistema_avisar(AVISO_MESH_NOS);   // a barra de escrita passa a "so consulta"
  ESP_LOGI(TAG, "servico parado%s", s_via == AJUSTES_MESH_SERIAL ? ", Grove livre" : "");
  return true;
}

bool tab5_mesh_soltar_grove(void)
{
  // Via Bluetooth o servico nao esta no Grove: trocar o modulo do cabo nao e
  // motivo para derrubar a conversa com o no.
  if (s_servico_ativo && s_via != AJUSTES_MESH_SERIAL) { return true; }
  return tab5_mesh_parar();
}

uint8_t tab5_mesh_via(void) { return s_via; }

const char* tab5_mesh_enlace_descricao(void)
{
  if (!s_servico_ativo) { return N_("parado"); }
  if (s_via == AJUSTES_MESH_INTERNET) { return mesh_mqtt_descricao(); }
  if (s_via == AJUSTES_MESH_BLUETOOTH) {
    return (!mesh_ble_conectado() && mesh_mqtt_ativo()) ? mesh_mqtt_descricao() : mesh_ble_descricao();
  }
  return tab5_mesh_no_ar() ? N_("cabo Grove, conversando") : N_("cabo Grove, esperando o no");
}

static void iniciar_via(uint8_t via);

void tab5_mesh_iniciar(void)
{
  // SOBE NA VIA ESCOLHIDA. Se o servico esta de pe por OUTRA via (internet
  // cobrindo a falta do no, ou o Bluetooth quando o Grove acabou de ganhar a
  // prioridade), para antes: sem isto o iniciar_via via "ja esta de pe" e nao
  // fazia nada, e o Mesh seguia na via velha.
  const uint8_t via = ajustes_mesh_via();
  if (s_servico_ativo && s_via != via && !parada_em_curso(&s_parada)) { tab5_mesh_parar(); }
  iniciar_via(via);
}

void tab5_mesh_rastro(bool ligado) { s_rastro = ligado; }

void tab5_mesh_grove_diag(char* out, size_t tam)
{
  snprintf(out, tam,
           "Grove: received %lu bytes in %lu good frames, %lu bytes outside frames, %lu oversized\n"
           "       sent %lu bytes in %lu frames | last frame from node %lu ms ago\n"
           "UART:  FIFO overflow %lu, buffer full %lu, frame error %lu, parity %lu, break %lu\n",
           (unsigned long)s_g.rx_bytes, (unsigned long)s_c_quadros, (unsigned long)s_g.fora_de_quadro,
           (unsigned long)s_g.grandes, (unsigned long)s_g.tx_bytes, (unsigned long)s_g.tx_quadros,
           (unsigned long)(s_ultimo_quadro_ms ? tab5_agora_ms() - s_ultimo_quadro_ms : 0),
           (unsigned long)s_g.ev_fifo, (unsigned long)s_g.ev_cheio, (unsigned long)s_g.ev_quadro,
           (unsigned long)s_g.ev_paridade, (unsigned long)s_g.ev_break);
}

const char* tab5_mesh_internet_estado(void)
{
  if (ajustes_mesh_via() == AJUSTES_MESH_INTERNET) {
    if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return N_("Internet escolhida: esperando o Wi-Fi conectar"); }
    if (!mesh_mqtt_ativo())                        { return N_("Internet escolhida: vai conectar em instantes"); }
    return mesh_mqtt_descricao();
  }
  if (!ajustes_mesh_internet())                   { return N_("Desligada"); }
  if (tab5_grove_tipo() == GROVE_MESHTASTIC
   || (s_servico_ativo && s_via == AJUSTES_MESH_SERIAL)) { return N_("Em espera: o nó no Grove tem prioridade"); }
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO)  { return N_("Esperando o Wi-Fi conectar"); }
  if (!mesh_mqtt_ativo())                         { return N_("Ligada, vai conectar em instantes"); }
  return mesh_mqtt_descricao();
}

const char* tab5_mesh_proxy_estado(void)
{
  // Curto: vai ao lado de "ligado" numa linha da pagina "No".
  static char d[48];
  if (tab5_wifi_estado() != TAB5_WIFI_CONECTADO) { return N_("sem Wi-Fi no Tab5"); }
  if (!mesh_mqtt_no_tem_proxy())                 { return N_("o nó ainda não confirmou"); }
  if (!mesh_mqtt_em_proxy())                     { return N_("vai conectar"); }
  if (!mesh_mqtt_conectado())                    { return N_("conectando ao servidor"); }
  uint32_t sobe, desce, perde;
  mesh_mqtt_proxy_contadores(&sobe, &desce, &perde);
  snprintf(d, sizeof(d), tr("conectado (%lu/%lu)"), (unsigned long)sobe, (unsigned long)desce);
  return d;
}

void tab5_mesh_iniciar_internet(void)
{
  if (s_servico_ativo) { return; }
  iniciar_via(AJUSTES_MESH_INTERNET);
}

static void iniciar_via(uint8_t via)
{
  // IDEMPOTENTE. Sem esta guarda, chamar duas vezes criaria um segundo mutex
  // (vazando o primeiro, com quem as tarefas ainda estariam sincronizando) e
  // uma segunda tarefa disputando a mesma UART. Nunca acontecia porque o unico
  // chamador era o boot; agora existe o caminho de re-sondagem.
  if (s_servico_ativo) {
    // Uma parada em andamento nao pode ser atropelada por um inicio: a tarefa
    // antiga ainda esta viva e a UART ainda e dela.
    if (parada_em_curso(&s_parada)) {
      ESP_LOGW(TAG, "ainda parando o servico anterior - nao vou subir por cima");
    } else {
      ESP_LOGI(TAG, "servico ja esta de pe - nada a fazer");
    }
    return;
  }

  tab5_sd_carona(mesh_na_carona);   // idempotente: a carona e registrada uma vez
  if (!s_fila_envio) { s_fila_envio = xQueueCreateWithCaps(MESH_FILA_ENVIO, sizeof(envio_fila_t), MALLOC_CAP_SPIRAM); }
  if (s_fila_envio) { xQueueReset(s_fila_envio); }
  tab5_hora_ao_saltar(ao_saltar_relogio);   // idempotente


  s_notif = notif_registrar("Meshtastic");
  notif_associar_app(s_notif, "Mesh");   // tocar o aviso abre o app Mesh
  // A preferencia (ajustes) e quem manda desde o INSTANTE em que a fonte
  // nasce - este registro acontece de novo a cada sondagem que reencontra o
  // modulo (troca de cabo), entao sem isto o silencio "esqueceria" a cada
  // hot-swap.
  if (s_notif >= 0) { notif_silenciar(s_notif, !ajustes_beep_mesh()); }
  memset(s_nos, 0, sizeof(s_nos));
  ++s_nos_ver;
  memset(s_conv, 0, sizeof(s_conv));
  // O ANEL DE IDS VISTOS NAO E ZERADO: trocar de via (Grove -> internet) ou
  // reconectar o cabo nao muda quais mensagens ja estao no cartao. Zerado, a
  // mesma mensagem ouvida pelo radio e depois pela ponte MQTT entrava duas
  // vezes no historico.
  memset(&s_saude, 0, sizeof(s_saude));   // a saude e do no desta via, nao da anterior

  ++s_conv_ver;   // a coluna recomeca

  // OS CANAIS EXISTEM DESDE O INICIO, sem esperar mensagem: os ativos da
  // ultima configuracao conhecida (NVS), ou o primario sem nenhuma. Numa malha
  // quieta, sem isto, nao havia para onde escrever. Os apagados ficam de fora
  // - e eram recriados aqui, o canal 0 sempre, mesmo apagado.
  criar_conversas_dos_canais();

  // Os nos que o Tab5 ja conhecia (alem do teto do radio).
  carregar_nodedb();
  mesh_diario_carregar();   // reinicios do no ja anotados (cartao)
  mesh_wifi_no_carregar();  // redes do Wi-Fi do no ja usadas (cartao)

  // FAVORITOS NA TABELA DESDE O INICIO, mesmo sem ter falado: estao fixos.
  recordar_favs();
  for (int i = 0; i < s_n_favs; ++i) { achar_no(s_favs[i]); }

  // RESTAURA O QUE O memset() ACIMA ACABOU DE APAGAR - defeito observado em
  // abrir o app ANTES de plugar o C6L jah carrega tudo do cartao
  // (ao_abrir() chama tab5_mesh_carregar_do_cartao()), mas a sondagem do Grove
  // que chega depois, achando o modulo, zera s_conv[] alguns milissegundos
  // apos - a lista fica so com o canal 0 (linha acima) ate o usuario sair e
  // voltar ao app, o que forca um novo ao_abrir(). Chamar de novo aqui, logo
  // apos o memset, fecha a mesma lacuna sem depender do usuario reabrir nada.
  // Idempotente (achar_ou_criar_conversa acha o que ja existe em vez de
  // duplicar) e seguro nesta tarefa: tab5_mesh_iniciar() so roda dentro de
  // a sondagem do Grove (kernel/grove.cpp), a tarefa de prioridade mais baixa do
  // sistema, feita para aguentar E/S lenta (aqui, montar o cartao) sem travar
  // o toque.
  tab5_mesh_carregar_do_cartao();

  // As conversas renasceram com as nao lidas guardadas (guardar_nao_lidas):
  // a barra passa a contar a partir delas, e nao do que sobrou publicado.
  travar();
  publicar_notif();
  destravar();

  s_via = via;
  if (s_via == AJUSTES_MESH_INTERNET) {
    // Sem cabo e sem radio: o laco do servico abre o MQTT (usar_internet).
  } else if (s_via == AJUSTES_MESH_BLUETOOTH) {
    // Sem Grove e sem UART: o enlace BLE procura o no, conecta e entrega o
    // que chegar. O Bluetooth ja foi ligado por quem chamou.
    if (!mesh_ble_iniciar()) { ESP_LOGE(TAG, "enlace Bluetooth nao subiu"); return; }
  } else {
  uart_config_t cfg = { };
  cfg.baud_rate = MESH_BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity    = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  if (!tab5_grove_tomar(GROVE_DONO_MESH)) {
    ESP_LOGE(TAG, "Grove ocupado - servico nao vai subir");
    return;
  }

  esp_log_level_set("uart", ESP_LOG_ERROR);   // reserva antiga do Ex_I2C, ver tab5_grove
  memset(&s_g, 0, sizeof(s_g));
  if (uart_driver_install(MESH_UART, 4096, 512, 20, &s_uart_ev, 0) != ESP_OK
   || uart_param_config(MESH_UART, &cfg) != ESP_OK
   || uart_set_pin(MESH_UART, MESH_TX, MESH_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
  {
    esp_log_level_set("uart", ESP_LOG_WARN);
    // DESFAZ O QUE JA FOI TOMADO. O servico ainda nao esta ativo, entao
    // tab5_mesh_parar() nao soltaria nada: sem isto o Grove ficava com o Mesh
    // para sempre e o GPS recebia "Grove ocupado". O driver pode ter sido
    // instalado antes de param_config/set_pin falharem.
    if (uart_is_driver_installed(MESH_UART)) { uart_driver_delete(MESH_UART); }
    s_uart_ev = nullptr;
    tab5_grove_soltar(GROVE_DONO_MESH);
    ESP_LOGE(TAG, "nao consegui abrir a UART do Grove");
    return;
  }
  esp_log_level_set("uart", ESP_LOG_WARN);
  // A UART NAO RECEBE COM O P4 EM SONO LEVE. Com a tela apagada o P4 dorme
  // entre eventos (contrato de economia, kernel/app.h), e o que o no manda
  // nesse intervalo se perde: mensagens que nunca chegavam e quadros cortados
  // virando nos de numero absurdo na NodeDB. Com o no no Grove, o sono leve
  // fica segurado enquanto o servico roda (a frequencia ainda desce).
  sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", true);
  }   // via serial

  // Prioridade 3, igual ao WiFi: abaixo da interface (4), para nunca engasgar o
  // toque; acima do registro (1), porque perder byte da serial e irreversivel.
  // ATIVO ANTES DE A TAREFA EXISTIR: com prioridade 3 ela comeca na hora,
  // antes de quem chamou (a sondagem, prioridade 1) voltar - e ja envia.
  s_iniciado_em_ms = tab5_agora_ms();   // ver o aviso em tab5_mesh_silencio_ms()
  s_servico_ativo  = true;
  // 6 KB: com 4 KB o monitor registrou no minimo 264 bytes livres em uso real.
  if (!parada_preparar(&s_parada)
   || !pool_criar(nullptr, vTaskMesh, nullptr, "mesh", 6144, 3, DONO_SERVICO, "Mesh")) {
    // DEVOLVE O QUE O INICIAR TOMOU. Antes so a bandeira voltava: a UART e o
    // Grove ficavam com o Mesh para sempre, e nenhuma sondagem achava mais
    // nada no cabo ate reiniciar.
    parada_desistir(&s_parada);
    s_servico_ativo = false;
    if (s_via == AJUSTES_MESH_BLUETOOTH) { mesh_ble_parar(); }
    else if (s_via == AJUSTES_MESH_SERIAL) {
      uart_driver_delete(MESH_UART); s_uart_ev = nullptr; tab5_grove_soltar(GROVE_DONO_MESH);
      sistema_recurso(RECURSO_SERIAL_GROVE, "Meshtastic", false);
    }
    ESP_LOGE(TAG, "sem memoria para a tarefa do Mesh");
    return;
  }
  sistema_avisar(AVISO_MESH_NOS);
  tab5_mem_marco("Mesh (servico)");
  if (s_via == AJUSTES_MESH_INTERNET) { ESP_LOGI(TAG, "servico no ar (so internet, sem o no)"); }
  else if (s_via == AJUSTES_MESH_BLUETOOTH) { ESP_LOGI(TAG, "servico no ar (Bluetooth)"); }
  else { ESP_LOGI(TAG, "servico no ar (Grove %d baud, RX=%d TX=%d)", MESH_BAUD, MESH_RX, MESH_TX); }
}
