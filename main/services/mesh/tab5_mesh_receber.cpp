// Servico Mesh - a interpretacao das mensagens que chegam do radio (FromRadio,
// MeshPacket, NodeInfo, telemetria, texto). Partido de tab5_mesh.cpp (o
// servico); o que as partes compartilham esta em mesh/tab5_mesh_interno.hpp.

#include "app.h"   // sistema_avisar
#include <string.h>
#include "esp_log.h"
#include <stdio.h>
#include "tab5_power.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "tab5_mesh_admin.hpp"
#include "tab5_mesh_diario.hpp"
#include "mesh_pb.hpp"
#include "tab5_hora.hpp"
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

// ---------------------------------------------------------------------------
// Interpretacao das mensagens que interessam
// ---------------------------------------------------------------------------
// DeviceMetrics: 1=bateria 2=tensao 3=uso_canal 4=airtime 5=uptime
//
// 'nosso' distingue a telemetria do NOSSO no da telemetria que chega da malha.
// Sem essa distincao (foi o bug) qualquer no vizinho sobrescrevia
// a nossa saude: aparecia "uptime 1136229s" - 13 dias - num C6L que tinha
// reiniciado 35 vezes. Bateria e tensao SEMPRE vao para o no dono da metrica;
// so os indicadores de saude e que sao exclusivos do nosso.
static void ler_metrics(pb_leitor_t l, mesh_no_t* no, bool nosso)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v = 0; pb_varint(&l, &v); if (no) no->bateria = (uint8_t)v; }
    else if (n == 2 && t == 5) { float f = 0; pb_float(&l, &f); if (no) no->tensao = f; }
    else if (n == 3 && t == 5) { float f = 0; pb_float(&l, &f); if (nosso) s_saude.uso_canal = f; }
    else if (n == 4 && t == 5) { float f = 0; pb_float(&l, &f); if (nosso) s_saude.airtime_tx = f; }
    else if (n == 5 && t == 0) { uint64_t v = 0; pb_varint(&l, &v); if (nosso) s_saude.uptime_s = (uint32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  if (nosso) { s_saude.tem_metricas = true; }
}

// LocalStats - a saude da malha vista pelo NOSSO no, e so por ele.
//   1=uptime 2=uso_canal 3=airtime 4..6=contadores de pacote
//   7=nos_online 8=nos_total
// Versoes mais novas acrescentam campos (piso de ruido, por exemplo); o pb_pular()
// cuida deles sem a gente precisar saber quais sao.
static void ler_local_stats(pb_leitor_t l)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.uptime_s   = (uint32_t)v; }
    else if (n == 2 && t == 5) { float f = 0; pb_float(&l,&f);     s_saude.uso_canal  = f; }
    else if (n == 3 && t == 5) { float f = 0; pb_float(&l,&f);     s_saude.airtime_tx = f; }
    else if (n == 4 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.pkt_tx      = (uint32_t)v; }
    else if (n == 5 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.pkt_rx      = (uint32_t)v; }
    else if (n == 6 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.pkt_ruins   = (uint32_t)v; }
    else if (n == 7 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.nos_online = (uint16_t)v; }
    else if (n == 8 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.nos_total  = (uint16_t)v; }
    else if (n == 9 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); s_saude.pkt_repetidos = (uint32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  s_saude.tem_local = true;
  // SEM LOG AQUI. Os
  // valores ja ficam em s_saude, consultaveis sob demanda pelo comando "mesh"
  // do console (imprime canal/airtime/uptime/nos) sempre que precisar olhar.
}

// HardwareModel (mesh.proto) - TABELA COMPLETA, gerada, nao mais adivinhada.
//
// A versao anterior desta funcao era um switch com uma duzia de modelos
// escolhidos a mao, com o argumento de que copiar a lista oficial inteira
// "envelheceria e mentiria". O argumento nao se sustentou no primeiro uso de
// verdade: uma malha real trouxe "modelo 110", "modelo 88",
// "modelo 81" - nos genuinos que a tabela adivinhada simplesmente nao previa,
// entre eles o 111 = M5Stack C6L, o proprio radio que este sistema usa.
//
// A correcao nao e adivinhar mais - e parar de adivinhar. TAB5_MESH_HW_NOMES
// (tab5_mesh_hw_dados.cpp) e gerada pelo gerar_hw.py direto do enum oficial
// do Meshtastic; "envelhece e mente" deixa de ser risco porque reexportar e
// um comando, nao uma memoria de quais numeros existem.
const char* mesh_hw_nome(uint8_t hw)
{
  return TAB5_MESH_HW_NOMES[hw];   // hw e uint8_t: sempre dentro da tabela
}

// User: 1=id 2=nome_longo 3=nome_curto 4=macaddr 5=hw_model
//
// O 'id' e o 'macaddr' sao o que faltava: nome longo e
// curto sao ETIQUETAS que quem renomeia no celular troca a vontade, e por elas
// nao da para saber com que APARELHO se esta falando. O id ("!a1b2c3d4") e
// derivado do MAC e acompanha o radio para sempre.
static void ler_user(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 2) { pb_texto(&l, no->id,    sizeof(no->id)); }
    else if (n == 2 && t == 2) { pb_texto(&l, no->longo, sizeof(no->longo)); }
    else if (n == 3 && t == 2) { pb_texto(&l, no->curto, sizeof(no->curto)); }
    else if (n == 4 && t == 2) {
      // macaddr e 'bytes', nao texto - nao da para usar ler_texto (ele
      // terminaria a string no primeiro byte 0x00 do endereco).
      uint64_t k;
      if (!pb_varint(&l, &k)) { break; }
      if (k > pb_restam(&l)) { break; }
      if (k == 6) { memcpy(no->mac, l.p, 6); no->tem_mac = true; }
      l.p += (size_t)k;
    }
    else if (n == 5 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); no->hw = (uint8_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
}

// Position: 1=lat_i 2=lon_i (sfixed32)
static void ler_position(pb_leitor_t l, mesh_no_t* no)
{
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 5) { uint32_t v = 0; if (!pb_fixo32(&l, &v)) { break; } no->lat_i = (int32_t)v; no->tem_pos = true; }
    else if (n == 2 && t == 5) { uint32_t v = 0; if (!pb_fixo32(&l, &v)) { break; } no->lon_i = (int32_t)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
}

// NodeInfo: 1=num 2=user 3=position 4=snr 5=last_heard 6=device_metrics 9=hops
static void ler_node_info(pb_leitor_t l)
{
  bool fav_radio = false;   // is_favorite: proto3, ausente = false
  uint32_t num = 0;
  mesh_no_t* no = nullptr;
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if (n == 1 && t == 0) {
      uint64_t v = 0; pb_varint(&l, &v); num = (uint32_t)v;
      no = achar_no(num);
      if (!no) { return; }
    }
    else if (!no) { if (!pb_pular(&l, t)) { break; } }         // num sempre vem antes
    else if (n == 2 && t == 2) {
      pb_leitor_t s;
      if (pb_sub(&l,&s)) {
        // O User do NOSSO no, cru: o set_owner o reenvia inteiro (tab5_mesh_admin).
        if (num == s_meu_num && s_meu_num) { mesh_admin_guardar_user(s.p, (size_t)(s.fim - s.p)); }
        ler_user(s, no);
      }
    }
    else if (n == 3 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) ler_position(s, no); }
    else if (n == 4 && t == 5) { pb_float(&l, &no->snr); }
    else if (n == 5 && t == 5) { pb_fixo32(&l, &no->visto_em); marcar_epoch(no->visto_em); }
    else if (n == 6 && t == 2) { pb_leitor_t s; if (pb_sub(&l,&s)) ler_metrics(s, no, num == s_meu_num); }
    // via_mqtt: o no chegou pela INTERNET, nao pelo radio. Muda o significado
    // de tudo que se ve dele - o SNR nao e dele, e nao da para alcanca-lo por
    // RF. Merece aparecer na lista.
    else if (n == 8 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); no->via_mqtt = (v != 0); }
    else if (n == 9 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); no->saltos = (uint8_t)v; }
    else if (n == 10 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); fav_radio = (v != 0); }
    else if (!pb_pular(&l, t)) { break; }
  }

  // Guarda a etiqueta para quando o radio nao estiver aqui. Renomear no
  // aplicativo do celular chega por este mesmo caminho, com o MESMO num - e o
  // cache se atualiza sozinho, sem virar um interlocutor novo.
  //
  // OS DOIS AGORA, curto E longo - so o curto usavel entrava
  // aqui (era o unico que tab5_mesh_nome_exibicao() promete mostrar). Mas a
  // tabela de nos (montar_linha(), app_mesh.cpp) sempre mostrou o longo
  // tambem, e SO da entrada ao vivo - sem cache, ficava "(sem nome)" ate
  // ESTA SESSAO ver uma NodeInfo daquele no de novo, o que e raro (a maioria
  // dos nos e aprendida por telemetria/posicao/texto, nunca por NodeInfo).
  // Ver o aviso grande junto de tab5_mesh_nome_longo_cache() no .hpp.
  if (no) {
    lembrar_nome(num, curto_utilizavel(no->curto) ? no->curto : nullptr,
                       no->longo[0] ? no->longo : nullptr);
    fixar_se_pelo_ar(no);
    fav_do_radio(num, fav_radio);
    ++s_nos_ver;
  }
  sistema_avisar(AVISO_MESH_NOS);
}

// ANEL DE IDS JA VISTOS - dedup PROPRIO, separado do s_msgs.
//
// Ate aqui, a deteccao de eco olhava dentro de s_msgs[MESH_MAX_MSGS] - e
// MESH_MAX_MSGS e 8. Nasceu para o caso simples (voce manda, o radio devolve
// o mesmo pacote segundos depois) e nesse caso 8 sempre bastou: dificilmente
// chegam 8 mensagens de OUTRAS pessoas no intervalo entre o envio e o eco.
//
// COM MQTT ISSO MUDA. O mesmo pacote pode chegar DUAS vezes por caminhos com
// atraso bem diferente - uma copia pelo LoRA local, outra via broker MQTT
// (uplink de outro no, ou o proprio downlink de volta) - e o atraso entre as
// duas nao e mais "alguns segundos previsiveis", e "o que a internet e a queue
// do broker demorarem". Numa malha com ponte ativa, o trafego tambem fica mais
// denso. As duas coisas juntas empurram a segunda copia para FORA da janela de
// 8 mensagens, e ela passa a entrar como se fosse nova - a mesma mensagem
// duas vezes na conversa.
//
// A correcao e um anel SO PARA ISSO, independente do que aparece na tela: 256
// ids, 1 KB (na PSRAM, como o resto do .bss do servico), nada alem de um
// uint32_t cada. Eram 64; com trafego denso passando pelo mesmo anel, a
// janela de um texto encolhia - a copia atrasada pela ponte entrava de
// novo. Nao guarda conteudo - so "este id ja passou por aqui".
#define MESH_DEDUP_N 256
static uint32_t s_ids_vistos[MESH_DEDUP_N];
static int      s_prox_id_visto = 0;

static bool id_ja_visto(uint32_t id)
{
  if (id == 0) { return false; }   // 0 e "sem id" (ver o id em tab5_mesh_enviar) - nunca dedup
  for (int i = 0; i < MESH_DEDUP_N; ++i) {
    if (s_ids_vistos[i] == id) { return true; }
  }
  return false;
}

static void marcar_id_visto(uint32_t id)
{
  if (id == 0) { return; }
  s_ids_vistos[s_prox_id_visto] = id;
  s_prox_id_visto = (s_prox_id_visto + 1) % MESH_DEDUP_N;
}

void guardar_msg(uint32_t de, uint32_t para, uint8_t canal, const char* txt,
                        uint32_t quando, float snr, int32_t rssi, uint32_t id,
                        bool via_mqtt, uint8_t saltos)
{
  // ECO DA PROPRIA MENSAGEM, E DUPLICATA POR PONTE MQTT: as duas descartam
  // pelo mesmo id, ver o comentario do anel logo acima.
  //
  // Ao enviar, guardamos a mensagem aqui na hora. Segundos depois o radio
  // DEVOLVE o mesmo pacote como recebido (from = nosso numero) - e antes de
  // lermos o campo 6 do MeshPacket ele chegava com id 0, virava uma SEGUNDA
  // copia, e era essa copia que a tela mostrava. Sem id, sem marca de entrega:
  // era por isso que o tique verde e o triangulo vermelho nunca apareciam nas
  // mensagens enviadas.
  //
  // O id do pacote e o mesmo que mandamos, entao ele identifica o eco sem
  // ambiguidade. Vale tambem para mensagem enviada pelo CELULAR atraves do
  // C6L: chega uma vez so, com o id que o radio deu a ela.
  if (id_ja_visto(id)) { return; }
  marcar_id_visto(id);

  // Empurra as antigas para o fim: indice 0 e sempre a mais recente, que e
  // como a tela quer ler.
  if (s_n_msgs < MESH_MAX_MSGS) { s_n_msgs++; }
  for (int i = s_n_msgs - 1; i > 0; --i) { s_msgs[i] = s_msgs[i-1]; }
  memset(&s_msgs[0], 0, sizeof(s_msgs[0]));
  s_msgs[0].de = de;
  s_msgs[0].para = para;
  s_msgs[0].canal = canal;
  // 0xFFFFFFFF e o endereco de difusao do Meshtastic. Qualquer outro valor
  // igual ao nosso numero significa mensagem enderecada A VOCE.
  s_msgs[0].direta = (para != 0xFFFFFFFFu) && (para == s_meu_num);
  snprintf(s_msgs[0].texto, sizeof(s_msgs[0].texto), "%s", txt);
  s_msgs[0].quando = quando;
  s_msgs[0].snr = snr;
  s_msgs[0].rssi = rssi;
  s_msgs[0].via_mqtt = via_mqtt;
  s_msgs[0].saltos = saltos;
  s_msgs[0].id = id;

  // MENSAGEM NOSSA NAO E "NAO LIDA". Parece obvio e nao era: o contador subia
  // para tudo, e o tab5_mesh_enviar corrigia DEPOIS decrementando s_nao_lidas -
  // mas esquecia o contador DA CONVERSA. Resultado: mandar no LongFast deixava
  // um "1" na conversa que voce estava olhando.
  //
  // A decisao tem que morar aqui, onde se sabe de quem e a mensagem, e nao
  // num remendo do lado de quem envia.
  const bool minha = (de == s_meu_num);

  // ACOMPANHA TAMBEM O QUE SAIU DO CELULAR.
  //
  // Ate agora so entrava no anel o que o Tab5 mandava - entao mensagem enviada
  // pelo app do celular atraves do C6L aparecia na conversa SEM marca nenhuma,
  // e voce ficava sem saber se chegou.
  //
  // Mas o ACK dela CHEGA ATE NOS: o C6L repassa todo o trafego de FromRadio
  // para cada cliente conectado, e nos somos um. A gente so o descartava por
  // nao ter o id na tabela. Registrando aqui, o mesmo Routing que marca as
  // nossas passa a marcar as dela.
  //
  // A condicao e "veio do nosso no e tem id": e a definicao de "mensagem que
  // este radio transmitiu", tenha sido esta tela ou o celular a pedir.
  //
  // SO TRAFEGO AO VIVO. Durante o dump o no pode reentregar uma mensagem nossa
  // antiga: anotada aqui ela virava ENVIANDO e, 60 s depois, "sem resposta" -
  // gravado no envios.txt POR CIMA do desfecho verdadeiro que ela ja tinha.
  if (minha && id != 0 && trafego_ao_vivo()) { anotar_envio(id, para); }

  // A CONVERSA E SEMPRE "O OUTRO LADO".
  //
  // Difusao vai para o canal. Direta vai para o no do OUTRO - que e o
  // remetente quando recebemos, e o DESTINATARIO quando somos nos que
  // enviamos. Sem essa distincao, mensagem direta enviada por nos caia na
  // conversa do canal, porque o teste de "direta" olhava so o destinatario.
  const bool difusao = (para == 0xFFFFFFFFu);
  const uint32_t outro = (de == s_meu_num) ? para : de;
  const int ci = difusao ? achar_ou_criar_conversa(true, canal, 0)
                         : achar_ou_criar_conversa(false, 0, outro);
  if (ci >= 0) {
    if (!minha) { s_conv[ci].nao_lidas++; }
    s_conv[ci].ultima_em = quando;
    // Sobe para o topo das diretas. Canal tem lugar fixo (pelo indice).
    if (!difusao) { s_conv_seq[ci] = ++s_seq; ++s_conv_ver; }
    gravar_msg(ci, &s_msgs[0]);
  }

  // Mensagem de verdade (enviada ou recebida) e a volta a se falar que desfaz
  // um apagar anterior - ver o comentario grande na flag, acima. VALE PARA OS
  // DOIS (canal tambem se apaga agora): a linha de cima ja
  // recria a conversa do canal incondicionalmente (e deveria - uma mensagem
  // de verdade e o proprio sinal de "voltou"), mas SEM desmarcar aqui a flag
  // continuaria escrita no cartao e o canal sumiria nao no proximo boot.
  if (difusao) { desmarcar_apagada(true, canal); }
  else         { desmarcar_apagada(false, outro); }

  // O CONTADOR SOBE SO DEPOIS DE A LINHA ESTAR NO GRAVADOR (gravar_msg, acima).
  // A tela le este numero sem trava: subindo antes, ela podia pedir a releitura
  // do cartao com a linha ainda nem entregue - e nada mais a fazia reler
  // depois. Este NUNCA satura - ver o aviso grande junto da declaracao.
  ++s_total_msgs;

  publicar_notif();
  // O ID VAI NO LOG: mensagem repetida que passa por aqui ja passou pelo
  // descarte por id (id_ja_visto). Com o id a vista, da para saber de cara se
  // a repeticao e da REDE (id novo - o remetente ou uma ponte reenviou) ou
  // nossa (id igual - o descarte falhou). E a origem: radio ou internet.
  ESP_LOGI(TAG, "mensagem %s de %08lX (id %08lX, %s): %s",
           s_msgs[0].direta ? "DIRETA" : "de canal", (unsigned long)de, (unsigned long)id,
           via_mqtt ? "internet" : "radio", txt);

  sistema_avisar(AVISO_MESH_MSG);
}

// "OS NOS MUDARAM" PARA A TELA, no maximo 4 por segundo: a ponte MQTT chega
// em rajada, e cada aviso fazia a pagina de nos e a coluna se conferirem de
// novo. A tabela muda na hora; so o aviso espera.
static void avisar_nos(void)
{
  static uint32_t ultimo_ms = 0;
  const uint32_t t = tab5_agora_ms();
  if (t - ultimo_ms < 250u) { return; }
  ultimo_ms = t;
  sistema_avisar(AVISO_MESH_NOS);
}

static void ler_data(pb_leitor_t l, uint32_t de, uint32_t para, uint8_t canal,
                     uint32_t quando, float snr, int32_t rssi, uint32_t id,
                     bool via_mqtt, uint8_t saltos, bool transp_mqtt)
{
  int porta = 0;
  uint32_t pedido = 0;                 // request_id: de qual pacote NOSSO isto fala
  int bitfield = -1;                   // -1 = nao veio (firmware anterior a 2.5)
  const uint8_t* pay = nullptr; size_t pay_n = 0;
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); porta = (int)v; }
    else if (n == 2 && t == 2) {
      uint64_t k; if (!pb_varint(&l,&k)) break;
      if (k > pb_restam(&l)) break;              // ver pb_restam(): somar no ponteiro estoura
      pay = l.p; pay_n = (size_t)k; l.p += (size_t)k;
    }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &pedido); }   // request_id
    else if (n == 9 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); bitfield = (int)v; }
    else if (!pb_pular(&l, t)) { break; }
  }
  // O CARIMBO DO REMETENTE (bit 0 = OK_TO_MQTT, ver MESH_CFG_OK_MQTT): sem ele,
  // gateway de servidor publico nao leva a mensagem ao MQTT. No log, para
  // responder "por que a mensagem de fulano nao chegou ao servidor".
  if (porta == PORT_TEXTO && !via_mqtt && de != s_meu_num) {
    ESP_LOGI(TAG, "texto de %08lX: OK para MQTT %s", (unsigned long)de,
             bitfield < 0 ? "ausente (firmware antigo - gateway publico descarta)"
                          : (bitfield & 1) ? "sim" : "NAO (gateway publico descarta)");
  }

  // A CONFIRMACAO NAO TEM CORPO OBRIGATORIO.
  //
  // Um ACK e um Routing com error_reason AUSENTE - e campo ausente em protobuf
  // nao ocupa byte nenhum, entao o payload vem VAZIO. Testar "if (!pay) return"
  // antes daqui, como o codigo fazia, descartaria justamente o caso de sucesso
  // e so o erro apareceria na tela.
  if (porta == PORT_ROTEAMENTO) {
    if (pedido == 0) { return; }
    int erro = 0;
    if (pay) {
      pb_leitor_t rl = { pay, pay + pay_n };
      int rn, rt;
      while (pb_proximo_campo(&rl, &rn, &rt)) {
        if (rn == 3 && rt == 0) { uint64_t v = 0; pb_varint(&rl,&v); erro = (int)v; }
        else if (!pb_pular(&rl, rt)) { break; }
      }
    }
    marcar_envio(pedido, erro, de, transp_mqtt);
    return;
  }

  if (!pay) { return; }

  // A APRESENTACAO DE UM NO, AO VIVO (NODEINFO_APP, payload = User).
  //
  // O FromRadio.node_info so vem no DUMP de configuracao - o no NAO o reenvia
  // quando um vizinho se apresenta depois; ele repassa o PACOTE, e o cliente
  // e quem le o User de dentro dele. Sem ler esta porta, todo no que
  // aparecesse depois do dump (e TODO no no modo internet, que nao tem dump
  // de verdade) ficava sem nome, sem modelo e sem MAC ate o proximo reinicio
  // do radio - e a conversa com ele aparecia pelo id em hexa.
  if (porta == PORT_NODEINFO) {
    mesh_no_t* no = achar_no(de);
    if (de == s_meu_num && s_meu_num) { mesh_admin_guardar_user(pay, pay_n); }
    if (no) {
      ler_user(pb_leitor_t{ pay, pay + pay_n }, no);
      lembrar_nome(de, curto_utilizavel(no->curto) ? no->curto : nullptr,
                   no->longo[0] ? no->longo : nullptr);
      ++s_nos_ver;
      avisar_nos();
    }
    return;
  }

  // POSICAO AO VIVO (POSITION_APP, payload = Position). Mesmo motivo: sem
  // isto a posicao de um no so era a do dump, e envelhecia para sempre.
  if (porta == PORT_POSICAO) {
    mesh_no_t* no = achar_no(de);
    if (no) { ler_position(pb_leitor_t{ pay, pay + pay_n }, no); }
    return;
  }

  if (porta == PORT_TEXTO) {
    char txt[MESH_TEXTO];
    size_t c = (pay_n < sizeof(txt) - 1) ? pay_n : sizeof(txt) - 1;
    memcpy(txt, pay, c); txt[c] = 0;
    // QUEBRA DE LINHA VIRA ESPACO. O historico no cartao e uma mensagem por
    // linha: um "\n" no meio (comum no que vem da ponte do Telegram) cortava a
    // mensagem ali, e o resto virava uma linha sem "M4," que a leitura pula.
    for (size_t k = 0; k < c; ++k) { if (txt[k] == '\n' || txt[k] == '\r' || txt[k] == '\t') { txt[k] = ' '; } }
    s_c_texto++;
    // SEM LOG AQUI. O
    // via_mqtt/saltos DESTE pacote (antes de qualquer fallback da tela) fica
    // gravado em s_msgs[0] normalmente - de "mesh" no console consegue ler
    // a mensagem mais recente com tab5_mesh_msg(0, ...) e ver os dois campos
    // crus, sob demanda, sem precisar de log automatico nenhum.
    guardar_msg(de, para, canal, txt, quando, snr, rssi, id, via_mqtt, saltos);
  }
  else if (porta == PORT_TELEMETRIA) {
    // Telemetry: 2=device_metrics, 6=local_stats. As outras variantes
    // (ambiente, qualidade do ar, energia) ficam para quando houver no com
    // sensor - pular e seguro e nao custa nada.
    s_c_telemetria++;
    pb_leitor_t tl = { pay, pay + pay_n };
    int tn, tt;
    while (pb_proximo_campo(&tl, &tn, &tt)) {
      s_ultima_variante = tn;    // qual variante de Telemetry veio por ultimo
      // Campo 1 = time: a hora em que o NOSSO no gerou a telemetria.
      if      (tn == 1 && tt == 5) { uint32_t v = 0; pb_fixo32(&tl, &v); if (de == s_meu_num) { marcar_hora_viva(v); } }
      else if (tn == 2 && tt == 2) { pb_leitor_t s; if (pb_sub(&tl,&s)) ler_metrics(s, achar_no(de), de == s_meu_num); }
      // LocalStats so faz sentido do proprio no: e a visao DELE da malha.
      else if (tn == 6 && tt == 2 && de == s_meu_num) { s_c_localstats++; pb_leitor_t s; if (pb_sub(&tl,&s)) ler_local_stats(s); }
      else if (!pb_pular(&tl, tt)) { break; }
    }
  }
}

// MeshPacket: 1=from 2=to 3=channel 4=decoded 6=id 7=rx_time 8=rx_snr
// 9=hop_limit 12=rx_rssi 13=via_mqtt (bool, varint) 14=hop_start - os dois
// ultimos lidos para a tela distinguir mensagem ouvida no ar
// (com o numero de saltos) de mensagem que entrou pela ponte MQTT.
static void ler_packet(pb_leitor_t l)
{
  uint32_t de = 0, quando = 0, para = 0xFFFFFFFFu, id = 0;
  uint8_t canal = 0;
  float snr = 0; int32_t rssi = 0;
  bool via_mqtt = false;
  // hop_limit: quanto SOBROU do orcamento de saltos quando o pacote chegou
  // aqui. hop_start: o orcamento ORIGINAL que o remetente armou - viaja
  // junto, sem mudar, pacote a pacote. A diferenca e quantos repetidores ele
  // passou. Zero em hop_start e o valor de fabrica do protobuf (campo
  // ausente) - firmware antigo nao manda, entao a conta fica indefinida.
  uint32_t hop_limit = 0, hop_start = 0;
  // transport_mechanism (21): por onde o pacote entrou no radio. So o MQTT (5)
  // interessa aqui - ver "O IMPLICITO DO MQTT" em marcar_envio().
  bool transp_mqtt = false;
  pb_leitor_t dados = { nullptr, nullptr };
  int n, t;
  while (pb_proximo_campo(&l, &n, &t)) {
    if      (n == 1 && t == 5) { pb_fixo32(&l, &de); }
    else if (n == 2 && t == 5) { pb_fixo32(&l, &para); }
    else if (n == 3 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); canal = (uint8_t)v; }
    else if (n == 6 && t == 5) { pb_fixo32(&l, &id); }       // id do pacote
    else if (n == 7 && t == 5) { pb_fixo32(&l, &quando); }
    else if (n == 8 && t == 5) { pb_float(&l, &snr); }
    else if (n == 9 && t == 0) { uint64_t v = 0; pb_varint(&l,&v); hop_limit = (uint32_t)v; }
    else if (n == 12 && t == 0){ uint64_t v = 0; pb_varint(&l,&v); rssi = (int32_t)v; }
    // CAMPOS 14 E 15, conferidos contra o mesh.pb.h do firmware do C6L
    // via_mqtt = 14, hop_start = 15. Ate aqui eram lidos 13 e 14
    // - o 13 e o 'delayed' (obsoleto, sempre 0) e o 14 era o via_mqtt tomado
    // por hop_start. Resultado: MQTT nunca era reconhecido pelo pacote e o
    // numero de saltos saia errado ou "--" (dai o fallback pelo NodeInfo).
    else if (n == 14 && t == 0){ uint64_t v = 0; pb_varint(&l,&v); via_mqtt = (v != 0); }
    else if (n == 15 && t == 0){ uint64_t v = 0; pb_varint(&l,&v); hop_start = (uint32_t)v; }
    else if (n == 21 && t == 0){ uint64_t v = 0; pb_varint(&l,&v); transp_mqtt = (v == 5); }
    else if (n == 4 && t == 2) { if (!pb_sub(&l, &dados)) break; }
    else if (!pb_pular(&l, t)) { break; }
  }
  // ZERO SIGNIFICA "EU". O Meshtastic usa from=0 em pacote gerado localmente -
  // aparece como "fr=0x00000000" no log do proprio radio. Sem traduzir isso:
  //   - o LocalStats era descartado (o filtro exigia de == meu numero), e os
  //     contadores de nos ficavam em 0/0;
  //   - e um no fantasma de numero 0 entrava na tabela.
  if (de == 0) { de = s_meu_num; }

  // SALTOS = hop_start - hop_limit, so quando hop_start veio preenchido (>0).
  // hop_start == 0 e o padrao de fabrica do protobuf (campo ausente,
  // firmware antigo) - nesse caso a conta fica indefinida, e o sentinela diz
  // isso em vez de inventar um "0 saltos" que seria mentira.
  uint8_t saltos = MESH_SALTOS_DESCONHECIDO;
  if (hop_start > 0 && hop_start >= hop_limit) {
    saltos = (uint8_t)(hop_start - hop_limit);
  }

  // Interpretado no fim porque os metadados (quem, quando, com que sinal) podem
  // vir DEPOIS do bloco de dados na serializacao.
  if (dados.p) { ler_data(dados, de, para, canal, quando, snr, rssi, id, via_mqtt, saltos, transp_mqtt); }

  marcar_epoch(quando);
  marcar_hora_viva(quando);

  if (de != 0) {
    mesh_no_t* no = achar_no(de);
    if (no) {
      if (snr != 0) { no->snr = snr; }
      // SALTOS/VIA_MQTT TAMBEM AQUI - a tela mostrava pouco mais que o ID,
      // comparada com o cliente web. Os dois numeros ja eram calculados ALI EM CIMA para TODO
      // pacote (ver 'saltos'/'via_mqtt' no comeco desta funcao) e ja iam
      // para a MENSAGEM (ler_data) - so nunca tinham sido aplicados ao NO.
      // A tabela de nos so aprendia essa coluna quando um NodeInfo chegava
      // (campo 9/8, ler_node_info() mais acima) - um pacote raro (rajada no
      // dump inicial, depois so de vez em quando), enquanto QUALQUER
      // pacote - mensagem, telemetria, posicao - ja carrega hop_start/
      // hop_limit/via_mqtt na propria borda (MeshPacket). O "Desconhecido"
      // ficava sozinho ate o proximo NodeInfo, mesmo com dezenas de outros
      // pacotes do mesmo no passando por aqui antes disso.
      //
      // So sobrescreve saltos quando ESTE pacote realmente trouxe hop_start
      // (saltos != DESCONHECIDO) - um pacote sem essa info nao deve apagar
      // um valor bom que outro pacote ja tinha ensinado. via_mqtt e sempre o
      // caminho MAIS RECENTE (um no pode ser ouvido direto numa hora e via
      // ponte na outra), entao sobrescreve sem guarda.
      if (saltos != MESH_SALTOS_DESCONHECIDO) { no->saltos = saltos; }
      no->via_mqtt = via_mqtt;
      // AVISTAMENTO AO VIVO. Marcado aqui e nao no ler_node_info de proposito:
      // o node_info chega em rajada no dump de configuracao, para TODOS os nos
      // de uma vez, e carimbar todos ali faria a malha inteira parecer ter sido
      // ouvida agora. Pacote e diferente - ele so chega quando o no falou.
      no->visto_local_ms = tab5_agora_ms();
      if (quando >= EPOCH_MINIMO) { no->visto_em = quando; }
      fixar_se_pelo_ar(no);   // ouvido pelo ar agora: ganha o lugar no topo e a protecao do despejo
      ++s_nos_ver;
    }
  }
  avisar_nos();
}

// LocalStats vem como telemetria do proprio no: 1=uptime 2=uso 3=airtime
// 4..=contagens, 8=nos_online, 9=nos_total, 10=piso_ruido (varia com a versao;
// pulamos o que nao reconhecemos).
void ler_from_radio(const uint8_t* buf, size_t n)
{
  pb_leitor_t l = { buf, buf + n };
  int f, t;

  // LOOP, e nao um unico campo. O FromRadio nao e so a variante: ele tem
  // "uint32 id = 1" FORA do oneof, e protobuf serializa em ordem de numero de
  // campo. Quando o radio preenche esse id - e ele preenche em alguns caminhos
  // do firmware - o primeiro campo do frame e o 1, nenhum ramo abaixo casa, e
  // a MENSAGEM INTEIRA ERA DESCARTADA em silencio.
  //
  // Ler so o primeiro campo era apostar que o id viria sempre zerado (e campo
  // zerado o nanopb omite). A aposta funciona quase sempre, que e a pior
  // especie de erro: some do teste e aparece no uso.
  while (pb_proximo_campo(&l, &f, &t))
  {
  // CAMPO 2, nao 1. O campo 1 do FromRadio e "id"; o pacote e o 2.
  // Conferido no protobuf oficial (meshtastic.protobuf.mesh_pb2).
  // Com o numero errado, TODO pacote chegava e era descartado como campo
  // desconhecido - o contador dizia "0 pacotes" com o enlace perfeito.
  if (f == 2 && t == 2) { s_c_pacotes++; pb_leitor_t s; if (pb_sub(&l,&s)) ler_packet(s); }
  else if (f == 3 && t == 2) {           // my_info
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_my_info(s.p, (size_t)(s.fim - s.p));   // reboot_count, nodedb_count
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 0) {
          uint64_t v = 0; pb_varint(&s,&v);
          const uint32_t novo = (uint32_t)v;
          // So escreve quando MUDA: a NVS tem ciclos de escrita finitos e o
          // my_info chega a cada apresentacao.
          if (novo != 0 && novo != s_meu_num) { lembrar_meu_numero(novo); }
          s_meu_num = novo;
        }
        else if (n2 == 8 && t2 == 0) {   // reboot_count: o diario de reinicios compara com o anterior
          uint64_t v = 0; pb_varint(&s, &v);
          // s_ultimo_quadro_ms ainda e o do quadro ANTERIOR (so atualiza depois
          // de ler_from_radio): zero = primeiro contato desta sessao.
          mesh_diario_contador((uint32_t)v, s_saude.uptime_s, s_ultimo_quadro_ms != 0, mesh_admin_gravando());
        }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  else if (f == 4 && t == 2) { s_c_nodeinfo++; pb_leitor_t s; if (pb_sub(&l,&s)) ler_node_info(s); }
  else if (f == 11 && t == 2) {          // queueStatus: 1=res 4=mesh_packet_id
    // Falha IMEDIATA, antes de o pacote sair pelo radio: a queue de transmissao
    // nao aceitou. Sem isto o envio ficaria "enviando" ate o prazo estourar,
    // quando o radio ja tinha dito que nem tentou.
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      int32_t res = 0; uint32_t pid = 0;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v = 0; pb_varint(&s,&v); res = (int32_t)v; }
        // mesh_packet_id e UINT32 (varint), nao fixed32 - conferido no
        // mesh.pb.h. Lido como fixed32 ele nunca casava, e a
        // recusa da queue do radio nunca marcava o envio como falho.
        else if (n2 == 4 && t2 == 0) { uint64_t v = 0; pb_varint(&s, &v); pid = (uint32_t)v; }
        else if (!pb_pular(&s, t2)) { break; }
      }
      if (pid != 0 && res != 0) { marcar_envio(pid, MESH_ERRO_FILA, s_meu_num); }
    }
  }
  else if (f == 7 && t == 0) {           // configCompleteId
    // FIM DO DOWNLOAD DE CONFIGURACAO. E daqui em diante que o radio comeca a
    // entregar o trafego AO VIVO - antes disso ele esta drenando a queue de
    // configuracao. Nao ha nada a fazer alem de registrar, mas registrar
    // importa: sem esta linha no log nao da para distinguir "o dump nunca
    // terminou" de "terminou e nao chega mensagem", e o conserto e outro.
    uint64_t v = 0;
    if (pb_varint(&l, &v)) {
      s_config_ok_ms = tab5_agora_ms();   // a hora ao vivo conta daqui (ver marcar_hora_viva)
      // A configuracao dos canais chegou inteira: s_canais_sujos ja esta de
      // pe, e a carona da tarefa "cartao" grava em ate 1 s (fora do radio).
      mesh_admin_fim_dump();         // a configuracao do no tambem
      enviar_favs_pendentes();       // estrelas mudadas sem o radio
      ESP_LOGI(TAG, "configuracao completa (id %lu) - trafego ao vivo daqui",
               (unsigned long)v);
    }
  }
  else if (f == 8 && t == 0) {           // rebooted
    uint64_t v = 0;
    if (pb_varint(&l, &v) && v != 0) {
      // TENSAO DO BARRAMENTO NO INSTANTE DO REINICIO
      // ("tem como saber se os 5V do Grove estao chegando?"). Nao medimos o
      // Grove especificamente (so existe o INA226 do barramento da BATERIA,
      // 2S Li-Po ~7.4V - ver tab5_power.hpp), e nao dá pra medir agora
      // mesmo (tab5_power_ultima() so, nunca _medir() daqui: esta e a
      // tarefa do Mesh, nao a de interface, e o barramento I2C interno nao
      // tem mutex - ver o aviso grande em tab5_power.hpp). O que "ultima"
      // devolve e uma leitura em cache de ATE 1s atras, atualizada pela
      // interface - resolucao grosseira para um afundamento rapido de
      // tensao, mas se o barramento estiver visivelmente baixo bem na hora
      // do reinicio, e um indicio real de energia, nao so uma suspeita.
      mesh_diario_foto(s_saude.uptime_s);   // contexto do Tab5 neste instante
      const tab5_medida_t m = tab5_power_ultima();
      ESP_LOGW(TAG, "o radio reiniciou - vou pedir a configuracao de novo "
                    "(barramento no instante: %.2fV %.0fmA, ate 1s atras)",
               (double)m.volts, (double)m.mA);
      s_radio_reiniciou = true;
    }
  }
  // CONFIG E MODULECONFIG - so o que o modo internet precisa: a regiao
  // (Config.lora.region, que vai na raiz do topico) e o servidor MQTT do no
  // (ModuleConfig.mqtt: 2=endereco 3=usuario 4=senha 8=raiz). Endereco vazio
  // = o servidor publico, que e o padrao do firmware.
  else if (f == 5 && t == 2) {           // config: oneof 1..10 = secao; 6=lora { 7=region }
    pb_leitor_t s, sec;
    int n2, t2;
    if (pb_sub(&l, &s)) {
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        // A SECAO CRUA vai para a configuracao do no (tab5_mesh_admin): e ela
        // que volta, inteira, quando um campo e alterado.
        mesh_admin_guardar_secao(false, n2, sec.p, (size_t)(sec.fim - sec.p));
        if (n2 != 6) { continue; }
        int n3, t3;
        while (pb_proximo_campo(&sec, &n3, &t3)) {
          if (n3 == 7 && t3 == 0) { uint64_t v = 0; pb_varint(&sec, &v); mesh_mqtt_aprender_regiao((int)v); }
          else if (!pb_pular(&sec, t3)) { break; }
        }
      }
    }
  }
  else if (f == 9 && t == 2) {           // moduleConfig: oneof 1..17 = modulo; 1=mqtt
    pb_leitor_t s, sec;
    int n2, t2;
    if (pb_sub(&l, &s)) {
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (t2 != 2) { if (!pb_pular(&s, t2)) { break; } continue; }
        if (!pb_sub(&s, &sec)) { break; }
        mesh_admin_guardar_secao(true, n2, sec.p, (size_t)(sec.fim - sec.p));
        if (n2 != 1) { continue; }
        char end[64] = "", usr[32] = "", pwd[32] = "", raiz[48] = "";
        bool mqtt_lig = false, tls = false, proxy = false;   // proto3: ausente = false
        int n3, t3;
        while (pb_proximo_campo(&sec, &n3, &t3)) {
          if      (n3 == 1 && t3 == 0) { uint64_t v = 0; pb_varint(&sec, &v); mqtt_lig = (v != 0); }
          else if (n3 == 7 && t3 == 0) { uint64_t v = 0; pb_varint(&sec, &v); tls = (v != 0); }
          else if (n3 == 9 && t3 == 0) { uint64_t v = 0; pb_varint(&sec, &v); proxy = (v != 0); }
          else if (n3 == 2 && t3 == 2) { pb_texto(&sec, end,  sizeof(end)); }
          else if (n3 == 3 && t3 == 2) { pb_texto(&sec, usr,  sizeof(usr)); }
          else if (n3 == 4 && t3 == 2) { pb_texto(&sec, pwd,  sizeof(pwd)); }
          else if (n3 == 8 && t3 == 2) { pb_texto(&sec, raiz, sizeof(raiz)); }
          else if (!pb_pular(&sec, t3)) { break; }
        }
        mesh_mqtt_aprender_servidor(end, usr, pwd, raiz);
        mesh_mqtt_aprender_no_mqtt(mqtt_lig, proxy, tls);
      }
    }
  }
  else if (f == 10 && t == 2) {          // channel: 1=index 2=settings 3=role
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      const uint8_t* ch_ini = s.p;
      const size_t   ch_n = (size_t)(s.fim - s.p);
      // OS DOIS COMECAM EM ZERO, e isso e o conserto de dois bugs.
      //
      // 'index' (int32) e 'role' (enum) sao campos proto3 SINGULAR: o nanopb
      // NAO os escreve quando valem zero. Comecando em -1 e 1, como era:
      //   - o canal 0 (o primario, LongFast) chegava SEM o campo 1, ficava com
      //     idx -1 e a configuracao inteira dele era ignorada - nunca entrava
      //     como ativo, e por isso nao aparecia para reabrir depois de apagado;
      //   - um canal DESABILITADO (role 0) chegava SEM o campo 3 e era lido como
      //     PRIMARY - continuava "ativo", com o nome antigo, na coluna.
      // O valor ausente e o zero do protobuf: e isso que o firmware quis dizer.
      int idx = 0, papel = 0;
      bool downlink = false, uplink = false;
      char nome[MESH_NOME_CONVERSA] = { 0 };
      uint8_t psk[32];
      size_t  n_psk = 0;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if      (n2 == 1 && t2 == 0) { uint64_t v = 0; pb_varint(&s,&v); idx = (int)v; }
        else if (n2 == 3 && t2 == 0) { uint64_t v = 0; pb_varint(&s,&v); papel = (int)v; }
        else if (n2 == 2 && t2 == 2) {           // ChannelSettings: 2=psk 3=name
          pb_leitor_t cs;
          if (pb_sub(&s,&cs)) {
            int n3, t3;
            while (pb_proximo_campo(&cs, &n3, &t3)) {
              if (n3 == 3 && t3 == 2) { pb_texto(&cs, nome, sizeof(nome)); }
              else if (n3 == 6 && t3 == 0) { uint64_t v = 0; pb_varint(&cs, &v); downlink = (v != 0); }   // o proxy assina
              else if (n3 == 5 && t3 == 0) { uint64_t v = 0; pb_varint(&cs, &v); uplink = (v != 0); }     // so para o diagnostico
              else if (n3 == 2 && t3 == 2) {   // psk: a chave, para o modo internet
                uint64_t k;
                if (!pb_varint(&cs, &k) || k > pb_restam(&cs)) { break; }
                n_psk = (k <= sizeof(psk)) ? (size_t)k : 0;
                if (n_psk) { memcpy(psk, cs.p, n_psk); }
                cs.p += (size_t)k;
              }
              else if (!pb_pular(&cs, t3)) { break; }
            }
          }
        }
        else if (!pb_pular(&s, t2)) { break; }
      }
      // O Channel CRU vai para a configuracao do no (a precisao da posicao do
      // primario e editada la, e volta com o resto dele intacto).
      mesh_admin_guardar_canal(idx, ch_ini, ch_n);
      if (idx >= 0 && idx < MESH_MAX_CANAIS) {
        mesh_mqtt_aprender_downlink(idx, papel != 0 && downlink);   // antes: aplicar_canal ensina o nome
        mesh_mqtt_aprender_uplink(idx, papel != 0 && uplink);
        aplicar_canal(idx, papel, nome, psk, n_psk);
      }
    }
  }
  else if (f == 14 && t == 2) {          // mqttClientProxyMessage: 1=topic 2=data 3=text 4=retained
    // O NO PEDINDO PARA PUBLICAR (proxy ligado nele). Sem o proxy do lado do
    // Tab5 a mensagem e so descartada - o no nao espera resposta.
    pb_leitor_t s;
    if (pb_sub(&l, &s)) {
      static char topico[128];   // so a tarefa do servico le FromRadio
      topico[0] = 0;
      const uint8_t* dado = nullptr; size_t n_dado = 0;
      bool retido = false;
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_texto(&s, topico, sizeof(topico)); }
        else if ((n2 == 2 || n2 == 3) && t2 == 2) {   // data (bytes) ou text: os dois vao crus
          uint64_t k;
          if (!pb_varint(&s, &k) || k > pb_restam(&s)) { break; }
          dado = s.p; n_dado = (size_t)k; s.p += (size_t)k;
        }
        else if (n2 == 4 && t2 == 0) { uint64_t v = 0; pb_varint(&s, &v); retido = (v != 0); }
        else if (!pb_pular(&s, t2)) { break; }
      }
      if (mesh_mqtt_em_proxy() && topico[0] && dado) { mesh_mqtt_proxy_publicar(topico, dado, n_dado, retido); }
    }
  }
  else if (f == 16 && t == 2) {          // clientNotification: aviso do firmware para quem usa
    pb_leitor_t s;
    if (pb_sub(&l, &s)) { mesh_admin_guardar_aviso(s.p, (size_t)(s.fim - s.p)); }
  }
  else if (f == 13 && t == 2) {          // metadata: 1=firmware_version
    pb_leitor_t s;
    if (pb_sub(&l,&s)) {
      mesh_admin_guardar_metadata(s.p, (size_t)(s.fim - s.p));
      int n2, t2;
      while (pb_proximo_campo(&s, &n2, &t2)) {
        if (n2 == 1 && t2 == 2) { pb_texto(&s, s_firmware, sizeof(s_firmware)); }
        else if (!pb_pular(&s, t2)) { break; }
      }
    }
  }
  // config e o resto: pulados, para o loop chegar ao campo seguinte em vez de
  // parar no primeiro desconhecido.
  else if (!pb_pular(&l, t)) { break; }
  }
}
