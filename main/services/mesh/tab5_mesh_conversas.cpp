// Servico Mesh - as CONVERSAS: o historico no cartao, as apagadas, a gravacao das
// mensagens e a ordem da coluna. Partido de tab5_mesh.cpp (o servico); o que as
// partes compartilham esta em mesh/tab5_mesh_interno.hpp.

#include <string.h>
#include "idioma.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "tab5_sd.hpp"
#include "tab5_mutex.hpp"
#include "notificacao.h"
#include <dirent.h>
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

static int      visivel_de(int interno);

// NAO LIDAS QUE SOBREVIVEM AO SERVICO. Parar o servico (troca de via, Wi-Fi
// caindo no modo internet, Bluetooth caindo, C6L achado no Grove) zera
// s_conv[] - e com ele as nao lidas. Visto ao vivo: o aviso da barra SUMIA
// sem ninguem ter aberto o Mesh, so porque o servico subiu de novo por outra
// via. Aqui fica a contagem de cada conversa ate ela ser LIDA de verdade;
// achar_ou_criar_conversa() devolve o numero quando a conversa renasce.
typedef struct { bool usada; bool canal; uint8_t idx; uint32_t no; uint16_t n; } nao_lida_guardada_t;
static nao_lida_guardada_t s_guardadas[MESH_MAX_CONVERSAS];

static nao_lida_guardada_t* guardada_de(bool canal, uint8_t idx, uint32_t no, bool criar)
{
  nao_lida_guardada_t* livre = nullptr;
  for (auto& g : s_guardadas) {
    if (!g.usada) { if (!livre) { livre = &g; } continue; }
    if (canal && g.canal && g.idx == idx) { return &g; }
    if (!canal && !g.canal && g.no == no) { return &g; }
  }
  if (!criar || !livre) { return nullptr; }
  *livre = { true, canal, idx, no, 0 };
  return livre;
}

// Com a trava tomada (ou com o servico parado). Fotografa as conversas vivas.
void guardar_nao_lidas(void)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    const mesh_conversa_t& c = s_conv[i];
    if (!c.usada) { continue; }
    nao_lida_guardada_t* g = guardada_de(c.canal, c.idx, c.no, c.nao_lidas > 0);
    if (!g) { continue; }
    if (c.nao_lidas > 0) { g->n = (uint16_t)(c.nao_lidas > 0xFFFF ? 0xFFFF : c.nao_lidas); }
    else                 { g->usada = false; }
  }
}

bool ha_nao_lidas_guardadas(void)
{
  for (const auto& g : s_guardadas) { if (g.usada && g.n) { return true; } }
  return false;
}

// Desfechos lidos do cartao, UMA VEZ por carga de historico.
//
// A primeira versao chamava uma funcao "procura este id no arquivo" de DENTRO
// do loop que le as mensagens. Com 40 mensagens na conversa isso virava 40
// aberturas e 40 varreduras completas do envios.txt, no cartao, com dois
// arquivos abertos ao mesmo tempo e o usuario esperando a conversa aparecer.
// Custo O(mensagens x desfechos) para responder uma pergunta que cabe em 1KB
// de RAM.
//
// Agora o arquivo e lido uma vez para esta tabela e a consulta e em memoria.
#define MESH_DESFECHOS 64

typedef struct {
  uint32_t id;
  uint8_t  estado;
  uint8_t  erro;
  uint32_t quando;   // carimbo corrigido ("H"); 0 = o da linha da mensagem vale
} desfecho_t;

// ULTIMO VENCE. O arquivo so cresce, entao a linha mais recente de um id e a
// verdade atual - um pacote pode ir de SEM_RESPOSTA para ENTREGUE quando um ACK
// chega atrasado. Por isso sobrescrevemos a entrada em vez de parar na primeira.
//
// Tabela cheia: descarta a mais ANTIGA (anel). O arquivo e cronologico, entao o
// que sobra sao os 64 desfechos mais recentes - e o historico so mostra as
// ultimas dezenas de mensagens de qualquer jeito.
static int carregar_desfechos(desfecho_t* tab, int max)
{
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_ENVIOS, "r");
  if (!f) { return 0; }

  int n = 0, prox = 0;
  char linha[64];
  int voltas = 0;
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao segurar o core
    unsigned long lid = 0, lq = 0; unsigned est = 0, err = 0;
    const bool eh_hora = (sscanf(linha, "H,%8lX,%lu", &lid, &lq) == 2);
    if (!eh_hora && sscanf(linha, "E,%8lX,%u,%u", &lid, &est, &err) != 3) { continue; }

    int alvo = -1;
    for (int i = 0; i < n; ++i) { if (tab[i].id == (uint32_t)lid) { alvo = i; break; } }
    if (alvo < 0) {
      if (n < max) { alvo = n++; }
      else         { alvo = prox; prox = (prox + 1) % max; }
      tab[alvo].estado = MESH_ENVIO_NA;
      tab[alvo].erro   = 0;
      tab[alvo].quando = 0;
    }
    tab[alvo].id = (uint32_t)lid;
    if (eh_hora) { tab[alvo].quando = (uint32_t)lq; continue; }
    tab[alvo].estado = (uint8_t)est;
    tab[alvo].erro   = (uint8_t)err;
  }
  fclose(f);
  return n;
}

static uint32_t buscar_carimbo(const desfecho_t* tab, int n, uint32_t id)
{
  if (id == 0) { return 0; }
  for (int i = 0; i < n; ++i) { if (tab[i].id == id) { return tab[i].quando; } }
  return 0;
}

static uint8_t buscar_desfecho(const desfecho_t* tab, int n, uint32_t id, uint8_t* erro)
{
  if (id == 0) { return MESH_ENVIO_NA; }
  for (int i = 0; i < n; ++i) {
    if (tab[i].id != id) { continue; }
    if (erro) { *erro = tab[i].erro; }
    return tab[i].estado;
  }
  return MESH_ENVIO_NA;
}

int tab5_mesh_num_msgs(void) { return s_n_msgs; }

// NUNCA SATURA - ver o aviso grande junto de s_total_msgs. Existe SO para
// quem precisa saber "chegou (ou saiu) mensagem desde a ultima vez que
// olhei" (comparacao direta, nao pintura_mudou_*): tab5_mesh_num_msgs()
// parece servir para isso mas nao serve, trava em MESH_MAX_MSGS (8) para
// sempre.
uint32_t tab5_mesh_total_msgs(void) { return s_total_msgs; }

bool tab5_mesh_msg(int i, mesh_msg_t* saida)
{
  if (i < 0 || i >= s_n_msgs) { return false; }
  travar();
  *saida = s_msgs[i];
  destravar();
  // Derivado na copia, igual ao idade_s dos nos: o estado muda o tempo todo e
  // guardar na mensagem faria a copia envelhecer entre a leitura e o uso.
  saida->envio = tab5_mesh_estado_envio(saida->id, &saida->erro);
  return true;
}

// ---------------------------------------------------------------------------
// CONVERSAS APAGADAS - A FLAG QUE FALTAVA.
// ---------------------------------------------------------------------------
// O BUG: apagar uma conversa (tab5_mesh_esquecer_conversa) sempre so mexeu na
// RAM, de proposito - "o cartao nao e tocado" era a regra, para o historico
// sobreviver caso voces voltassem a se falar. So que tab5_mesh_carregar_do_
// cartao() RELE A PASTA INTEIRA sempre que roda (abrir o app, sair do
// repouso), e o arquivo no-XXXXXXXX.txt continuava la - entao a conversa
// "apagada" voltava sozinha na proxima varredura. Nao era a mensagem
// reaparecendo, era a PASTA sendo lida nova de novo sem lembrar da decisao.
//
// A CORRECAO E A FLAG QUE FOI PEDIDA: um arquivo pequeno, apagadas.txt, uma
// linha por numero de no escondido. tab5_mesh_carregar_do_cartao() para de
// recriar a conversa de quem esta nesta lista - o historico continua no
// cartao (apagar so o REGISTRO, nao o passado, segue sendo a regra), so que
// agora ele fica invisivel de proposito, nao por acidente.
//
// UM NO SAI DA LISTA SOZINHO quando voces voltam a se falar de verdade - toque
// deliberado em "conversar com este no" (tab5_mesh_conversa_com) ou mensagem
// nova chegando (guardar_msg). Reler o cartao NAO conta como "voltar a se
// falar"; so conteudo novo conta. UM CANAL sai igual - toque em "conversar com
// este canal" (tab5_mesh_conversa_do_canal) ou mensagem nova nele.
//
// CANAL PASSOU A ENTRAR AQUI - ate entao so no direto podia ser
// apagado ("canal nao se apaga, e devolver false e mais honesto que fingir",
// dizia o comentario velho de tab5_mesh_esquecer_conversa). O pedido de
// oferecer "canais disponiveis" para reabrir depois exigia justamente a
// mesma flag que ja existia para no - so precisava aprender a guardar QUAL
// TIPO de chave e essa, ja que indice de canal (0..7) e numero de no (sempre
// um valor grande, derivado de MAC) nao podem se confundir no MESMO campo.
typedef struct { bool canal; uint32_t chave; } apagado_t;
#define MESH_ARQ_APAGADAS "apagadas.txt"

#define MESH_MAX_APAGADOS 64   // conversas escondidas; nada a ver com a tabela de nos
static apagado_t s_apagados[MESH_MAX_APAGADOS];
static int       s_n_apagados = 0;
static bool      s_apagados_carregado = false;

// FORMATO DO ARQUIVO: uma linha por entrada.
//   no direto:  "%08lX"   (como sempre foi - RETROCOMPATIVEL de proposito,
//               nenhum arquivo apagadas.txt ja gravado precisa mudar)
//   canal:      "K%u"     ('K' nunca e digito hexadecimal (0-9A-F), entao
//               nunca se confunde com um numero de no legitimo lido do
//               formato antigo)
static void carregar_apagados(void)
{
  // Sem cartao NAO conta como carregado: a lista da RAM nao pode virar o
  // arquivo (desmarcar_apagada reescreve). Tenta de novo no proximo uso.
  if (!tab5_sd_montar()) { return; }
  s_apagados_carregado = true;   // com o cartao: uma leitura por sessao, mesmo se o arquivo faltar
  // O ARQUIVO E A VERDADE: o que foi marcado antes desta leitura ja esta nele
  // (tab5_sd_abrir aplica o diario e o pendente antes de abrir).
  s_n_apagados = 0;
  FILE* f = tab5_sd_abrir(MESH_PASTA, MESH_ARQ_APAGADAS, "r");
  if (!f) { tab5_sd_desmontar(); return; }

  char linha[16];
  while (fgets(linha, sizeof(linha), f) && s_n_apagados < MESH_MAX_APAGADOS) {
    if (linha[0] == 'K') {
      const uint32_t idx = (uint32_t)strtoul(linha + 1, nullptr, 10);
      if (idx < MESH_MAX_CANAIS) {
        s_apagados[s_n_apagados].canal = true;
        s_apagados[s_n_apagados].chave = idx;
        ++s_n_apagados;
      }
      continue;
    }
    const uint32_t num = (uint32_t)strtoul(linha, nullptr, 16);
    if (num != 0) {
      s_apagados[s_n_apagados].canal = false;
      s_apagados[s_n_apagados].chave = num;
      ++s_n_apagados;
    }
  }
  fclose(f);
  tab5_sd_desmontar();
  ESP_LOGI(TAG, "cartao: %d conversa(s) apagada(s) - ficam escondidas", s_n_apagados);
}

// SEM TRAVA: chamada de dentro dela.
bool esta_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal == canal && s_apagados[i].chave == chave) { return true; }
  }
  return false;
}

// Marca uma conversa como apagada: RAM primeiro (vale na hora, mesmo sem
// cartao), depois grava a linha - a linha extra no arquivo, sem reescrever
// tudo, e o mesmo truque de nos.txt.
static void marcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (esta_apagada(canal, chave)) { return; }
  if (s_n_apagados < MESH_MAX_APAGADOS) {
    s_apagados[s_n_apagados].canal = canal;
    s_apagados[s_n_apagados].chave = chave;
    ++s_n_apagados;
  }

  char linha[16];
  if (canal) { snprintf(linha, sizeof(linha), "K%u\n", (unsigned)chave); }
  else       { snprintf(linha, sizeof(linha), "%08lX\n", (unsigned long)chave); }
  tab5_sd_anexar(MESH_PASTA, MESH_ARQ_APAGADAS, linha);
}

// Tira uma conversa da lista de apagados - chamado quando voces voltam a se
// falar de verdade (ver o comentario grande acima). Reescreve o arquivo
// inteiro: e raro (so acontece ao reatar algo que voce tinha apagado), e a
// lista e pequena.
void desmarcar_apagada(bool canal, uint32_t chave)
{
  if (!s_apagados_carregado) { carregar_apagados(); }
  if (!s_apagados_carregado) { return; }   // sem a lista do cartao nao se reescreve o arquivo
  int achou = -1;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal == canal && s_apagados[i].chave == chave) { achou = i; break; }
  }
  if (achou < 0) { return; }   // nao estava apagado - nada a fazer, nada a reescrever

  for (int i = achou; i < s_n_apagados - 1; ++i) { s_apagados[i] = s_apagados[i+1]; }
  --s_n_apagados;

  // Reescreve do zero pela porta do sistema: o substituir grava antes as
  // linhas de "apagada" que ainda estavam para entrar, e com o cartao
  // emprestado ao computador vai para o diario.
  char txt[MESH_MAX_APAGADOS * 12 + 1];
  size_t k = 0;
  for (int i = 0; i < s_n_apagados; ++i) {
    if (s_apagados[i].canal) { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "K%u\n", (unsigned)s_apagados[i].chave); }
    else                     { k += (size_t)snprintf(txt + k, sizeof(txt) - k, "%08lX\n", (unsigned long)s_apagados[i].chave); }
  }
  tab5_sd_substituir(MESH_PASTA, MESH_ARQ_APAGADAS, txt, k);
}

// ---------------------------------------------------------------------------
// Conversas e historico no cartao
// ---------------------------------------------------------------------------
// Nome do arquivo de uma conversa. Curto e sem acento de proposito: e nome de
// arquivo em FAT, e o cartao ja nos ensinou que 8.3 morde (ver o caso do
// satelites.csv que nao abria por falta de LFN).
static void arquivo_da_conversa(const mesh_conversa_t* c, char* out, size_t tam)
{
  if (c->canal) { snprintf(out, tam, "canal-%u.txt", (unsigned)c->idx); }
  else          { snprintf(out, tam, "no-%08lX.txt", (unsigned long)c->no); }
}

int achar_ou_criar_conversa(bool canal, uint8_t idx, uint32_t no)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    if (canal && s_conv[i].canal && s_conv[i].idx == idx) { return i; }
    if (!canal && !s_conv[i].canal && s_conv[i].no == no) { return i; }
  }
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada) { continue; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));
    s_conv_seq[i] = 0;
    ++s_conv_ver;   // a ordem da coluna mudou
    s_conv[i].usada = true;
    s_conv[i].canal = canal;
    s_conv[i].idx = idx;
    s_conv[i].no  = no;
    if (const nao_lida_guardada_t* g = guardada_de(canal, idx, no, false)) { s_conv[i].nao_lidas = g->n; }
    if (canal) {
      // Nome REAL, se o radio ja nos contou (FromRadio campo 10). O rotulo
      // inventado abaixo e so o que resta enquanto a configuracao nao chegou.
      if (idx < MESH_MAX_CANAIS && s_canal_nome[idx][0]) {
        snprintf(s_conv[i].nome, sizeof(s_conv[i].nome), "%s", s_canal_nome[idx]);
      } else {
        snprintf(s_conv[i].nome, sizeof(s_conv[i].nome),
                 idx == 0 ? "LongFast" : "Canal %u", (unsigned)idx);
      }
    } else {
      // Nome do no - curto, longo, e o id so em ultimo caso (nome_exibicao).
      nome_exibicao(no, s_conv[i].nome, sizeof(s_conv[i].nome));
    }
    return i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// GRAVACAO DAS MENSAGENS
// ---------------------------------------------------------------------------
// O Mesh tinha tarefa e queue PROPRIAS de gravacao ("mesh_grava",
// apertar enviar travava a tela montando o cartao). A ideia era
// certa e virou do SISTEMA: tab5_sd_anexar (hal/tab5_sd.hpp) copia a linha e
// volta na hora, e a tarefa "cartao" grava em lote. A mensagem vai como
// URGENTE: gravada em milissegundos, nao no lote de 5 s.
// Acrescenta ao arquivo da conversa. Formato de UMA linha, com o texto POR
// ULTIMO para que virgula dentro da mensagem nao quebre a leitura:
//   M,<quando>,<de>,<snr>,<rssi>,<texto>
//
// NAO FAZ MAIS E/S AQUI - ver o aviso grande acima, junto de laco_gravar().
// Isto roda com s_trava preso (chamado de dentro de guardar_msg()) e as
// vezes na tarefa de INTERFACE - tem que ser rapido incondicionalmente.
void gravar_msg(int ci, const mesh_msg_t* m)
{
  if (ci < 0) { return; }
  char arquivo[32];
  arquivo_da_conversa(&s_conv[ci], arquivo, sizeof(arquivo));
  // "M4" e nao "M3": o registro ganhou os saltos - etiqueta nova
  // em vez de campo a mais, e o leitor sabe o que esperar antes de ler.
  // QUEBRA DE LINHA NO TEXTO (a conversa tem linhas): o registro e de UMA
  // linha, entao o \n do texto vai como 0x1F (separador de unidade, que
  // ninguem digita) e volta a ser \n na leitura (tab5_mesh_historico_de).
  char txt[sizeof(((mesh_msg_t*)0)->texto)];
  size_t k = 0;
  for (const char* c = m->texto; *c && k + 1 < sizeof(txt); ++c) { txt[k++] = *c == '\n' ? '\x1f' : *c == '\r' ? ' ' : *c; }
  txt[k] = 0;
  char linha[sizeof(((mesh_msg_t*)0)->texto) + 96];
  snprintf(linha, sizeof(linha), "M4,%lu,%08lX,%.1f,%ld,%d,%u,%08lX,%s\n",
           (unsigned long)m->quando, (unsigned long)m->de,
           m->snr, (long)m->rssi, (int)m->via_mqtt, (unsigned)m->saltos,
           (unsigned long)m->id, txt);
  if (!tab5_sd_anexar(MESH_PASTA, arquivo, linha, nullptr, true)) {
    ESP_LOGW(TAG, "gravador cheio - mensagem NAO foi salva no cartao");
  }
}

// AS CONVERSAS QUE ESTAO NO CARTAO, sem precisar do radio.
//
// O historico sempre esteve gravado, mas a LISTA de conversas so existia na
// memoria - e ela so era povoada por mensagem chegando. Sem o C6L plugado, o
// app abria dizendo "sem conversas" com o cartao cheio delas. O arquivo estava
// la; faltava alguem ler o indice.
//
// A pasta e o proprio indice: cada conversa e um arquivo, e o nome dele carrega
// a chave (canal-3.txt, no-A1B2C3D4.txt). Nao precisa de catalogo a parte, que
// seria mais um estado para ficar dessincronizado do que descreve.
//
// Serve TAMBEM com o radio no ar: conversas antigas passam a aparecer na coluna
// desde a abertura, em vez de so quando alguem volta a falar. Renomear e
// seguro - o nome do canal chega depois no dump de configuracao e sobrescreve
// o rotulo inventado, porque o arquivo e nomeado pelo INDICE e nao pelo nome.
int tab5_mesh_carregar_do_cartao(void)
{
  // Sem radio, o numero vem da NVS. E o que permite ao arquivo saber quais
  // mensagens sao suas.
  if (s_meu_num == 0) { s_meu_num = recordar_meu_numero(); }

  // Nomes ANTES das conversas: e o cache que da nome a elas quando o radio nao
  // esta plugado. Na ordem inversa, toda conversa nasceria chamada pelo numero.
  if (!s_nomes_lidos) { carregar_nomes(); }

  if (!tab5_sd_montar()) { return 0; }

  char pasta[128];
  if (!tab5_sd_pasta(MESH_PASTA, pasta, sizeof(pasta))) {
    tab5_sd_desmontar();
    return 0;
  }

  DIR* dir = opendir(pasta);
  if (!dir) { tab5_sd_desmontar(); return 0; }

  int achadas = 0;
  struct dirent* e;
  travar();
  recordar_canais();
  while ((e = readdir(dir)) != NULL) {
    const char* n = e->d_name;
    if (strcmp(n, MESH_ARQ_ENVIOS) == 0) { continue; }   // desfechos, nao conversa

    if (strncmp(n, "canal-", 6) == 0) {
      const unsigned long idx = strtoul(n + 6, nullptr, 10);
      // !esta_apagada(true, idx) ACRESCENTADO - faltava aqui, e
      // era a SEGUNDA causa (independente da config do radio) de um canal
      // apagado voltar sozinho: esta varredura roda ao abrir o app e nao
      // sabia nada sobre "apagada", so olhava se o arquivo existia no
      // cartao. Mesma logica que "no-" ja usava, so nunca tinha sido
      // estendida para canal.
      // Canal DESABILITADO no no (configuracao conhecida, ver
      // recordar_canais): o arquivo fica no cartao, a conversa nao volta.
      const bool desligado = s_canais_conhecidos && idx < MESH_MAX_CANAIS && !s_canal_ativo[idx];
      if (idx < MESH_MAX_CANAIS && !desligado && !esta_apagada(true, (uint8_t)idx)
       && achar_ou_criar_conversa(true, (uint8_t)idx, 0) >= 0) { ++achadas; }
    }
    else if (strncmp(n, "no-", 3) == 0) {
      // Hexadecimal, como foi escrito. Zero nao e no - e o que o strtoul
      // devolve quando o nome nao presta, entao serve de peneira tambem.
      const uint32_t num = (uint32_t)strtoul(n + 3, nullptr, 16);
      // Apagada de proposito: nao recriar so porque o arquivo ainda existe no
      // cartao (ver o comentario grande em carregar_apagados, acima). O
      // historico continua salvo - so nao volta a aparecer sozinho.
      if (num != 0 && !esta_apagada(false, num) && achar_ou_criar_conversa(false, 0, num) >= 0) { ++achadas; }
    }
  }
  destravar();
  closedir(dir);
  tab5_sd_desmontar();

  ESP_LOGI(TAG, "cartao: %d conversa(s) no arquivo", achadas);
  return achadas;
}

// COMECAR A FALAR COM ALGUEM QUE AINDA NAO FALOU CONOSCO.
//
// Ate aqui uma conversa direta so nascia quando a mensagem CHEGAVA
// (achar_ou_criar_conversa no caminho de recepcao). Ou seja: dava para
// responder, nunca para puxar assunto. Esta funcao abre o outro sentido, a
// partir da lista de nos.
//
// Devolve o indice VISIVEL - o mesmo que a coluna de conversas usa - e nao o
// interno, porque quem chama e a tela e e com esse que ela sabe trabalhar. Os
// dois divergem assim que uma conversa e liberada no meio do vetor.
int tab5_mesh_conversa_com(uint32_t no)
{
  // Numero zero nao e no; e o preenchimento de quem nao sabe. E conversa com o proprio no
  // mesmo nao existe no protocolo - o radio nao entrega a si proprio.
  if (no == 0) { return -1; }

  travar();
  if (no == s_meu_num) { destravar(); return -1; }

  const int interno = achar_ou_criar_conversa(false, 0, no);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este no": e a volta a se falar que
  // desfaz um apagar anterior (ver o comentario grande acima da flag).
  if (interno >= 0) { desmarcar_apagada(false, no); }
  return visivel;
}

// Mesmo papel de tab5_mesh_conversa_com(), para CANAL - criado
// junto da opcao de apagar canal, para "canais disponiveis" poder reabrir um
// que foi escondido (ver o aviso grande da flag de apagados, e
// tab5_mesh_num_canais_disponiveis()/tab5_mesh_canal_disponivel() no fim
// deste arquivo).
int tab5_mesh_conversa_do_canal(uint8_t idx)
{
  if (idx >= MESH_MAX_CANAIS) { return -1; }

  travar();
  const int interno = achar_ou_criar_conversa(true, idx, 0);
  const int visivel = (interno >= 0) ? visivel_de(interno) : -1;
  destravar();

  // Toque deliberado em "conversar com este canal": desfaz um apagar
  // anterior, mesmo raciocinio de conversa_com() para no.
  if (interno >= 0) { desmarcar_apagada(true, idx); }
  return visivel;
}

// ---------------------------------------------------------------------------
// A ORDEM DA COLUNA
// ---------------------------------------------------------------------------
// Canais primeiro, pelo indice; depois as diretas, mais recente em cima. O
// vetor s_conv e um conjunto de vagas, nao uma ordem: uma conversa nova
// ocupa a primeira vaga livre, que pode ficar ANTES da conversa aberta. Com o
// indice visivel sendo "a n-esima vaga usada", isso deslocava a conversa
// aberta uma casa - o app passava a desenhar o historico de uma e a enviar
// para outra. Agora a ordem e explicita e a mudanca dela e anunciada
// (s_conv_ver), para o app reencontrar a sua pela chave.
//
// SEM TRAVA: chamadas de dentro dela. Insercao direta: sao no maximo 12.
static bool vem_antes(int a, int b)
{
  const mesh_conversa_t& x = s_conv[a];
  const mesh_conversa_t& y = s_conv[b];
  if (x.canal != y.canal) { return x.canal; }
  if (x.canal)            { return x.idx < y.idx; }
  if (s_conv_seq[a] != s_conv_seq[b]) { return s_conv_seq[a] > s_conv_seq[b]; }
  return a < b;
}

static int ordem_conversas(int out[MESH_MAX_CONVERSAS])
{
  int n = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    int j = n++;
    while (j > 0 && vem_antes(i, out[j - 1])) { out[j] = out[j - 1]; --j; }
    out[j] = i;
  }
  return n;
}

static int interno_de(int visivel)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  return (visivel >= 0 && visivel < n) ? o[visivel] : -1;
}

static int visivel_de(int interno)
{
  int o[MESH_MAX_CONVERSAS];
  const int n = ordem_conversas(o);
  for (int k = 0; k < n; ++k) { if (o[k] == interno) { return k; } }
  return -1;
}

uint32_t tab5_mesh_conversas_versao(void) { return s_conv_ver; }

int tab5_mesh_conversa_indice(bool canal, uint8_t idx, uint32_t no)
{
  int v = -1;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada || s_conv[i].canal != canal) { continue; }
    if (canal ? (s_conv[i].idx == idx) : (s_conv[i].no == no)) { v = visivel_de(i); break; }
  }
  destravar();
  return v;
}

// APAGAR UMA CONVERSA - CANAL TAMBEM.
//
// Nasceu de um efeito colateral do "tocar no no para falar com ele": bastava
// TOCAR para a conversa existir, e ela ficava la vazia para sempre. Criar sem
// oferecer como desfazer e um caminho so de ida.
//
// CANAL NAO SE APAGAVA - "devolver false e mais honesto que
// fingir", dizia o comentario velho aqui, porque nao havia flag nenhuma que
// impedisse o canal de voltar sozinho na proxima mensagem ou na proxima
// config. Agora ha (a mesma "apagados" que ja servia no direto, extendida
// para guardar tambem indice de canal - ver o aviso grande junto dela) e os
// dois pontos que recriavam a conversa sozinhos (a varredura do cartao e o
// parser da config, ver os avisos la) ja checam essa flag. Falta so o "voltar
// a falar de proposito" - tab5_mesh_conversa_do_canal() cuida disso.
//
// O CARTAO NAO E TOCADO, de proposito. O que se apaga e o registro em memoria,
// nao o historico. Se voces voltarem a se falar, a conversa renasce com o que
// ja tinha sido dito - que e o que se espera de uma lista, e nao de um
// expurgo. Uma conversa vazia, que e o caso que motivou isto, nao tem nada no
// cartao mesmo.
bool tab5_mesh_esquecer_conversa(int idx)
{
  bool ok = false, canal_apagado = false;
  uint32_t chave_apagada = 0;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    // As nao lidas dela saem do total sozinhas: publicar_notif() reconta.
    // Guardado antes do memset apagar os proprios campos.
    canal_apagado = s_conv[i].canal;
    chave_apagada = canal_apagado ? s_conv[i].idx : s_conv[i].no;
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].canal, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
    memset(&s_conv[i], 0, sizeof(s_conv[i]));   // usada volta a false
    s_conv_seq[i] = 0;
    ++s_conv_ver;
    ok = true;
  }
  if (ok) { publicar_notif(); }
  destravar();

  // Fora da trava: e I/O de cartao (fopen/fclose), nao deve segurar o mutex
  // que o resto do servico (recepcao, envio) tambem disputa. Canal-0 e chave
  // valida (LongFast e o canal 0), entao so no direto tem "zero = invalido".
  if (ok && (canal_apagado || chave_apagada != 0)) {
    marcar_apagada(canal_apagado, chave_apagada);
  }
  return ok;
}

int tab5_mesh_num_conversas(void)
{
  int n = 0;
  travar();
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) { if (s_conv[i].usada) { ++n; } }
  destravar();
  return n;
}

bool tab5_mesh_conversa(int idx, mesh_conversa_t* saida)
{
  bool ok = false;
  travar();
  const int i = interno_de(idx);
  if (i >= 0) {
    *saida = s_conv[i];
    // A segunda linha, derivada agora (ver mesh_conversa_t.sub): o nome
    // longo de quem e a conversa direta, se ele disser algo que o nome da
    // primeira linha ainda nao disse.
    saida->sub[0] = 0;
    if (saida->canal) {
      snprintf(saida->sub, sizeof(saida->sub), saida->idx == 0 ? tr("canal primário") : tr("canal %u"), (unsigned)saida->idx);
    } else {
      const char* longo = nullptr;
      for (int k = 0; k < MESH_MAX_NOS; ++k) {
        if (s_nos[k].usado && s_nos[k].num == saida->no) { if (s_nos[k].longo[0]) { longo = s_nos[k].longo; } break; }
      }
      nome_legivel(longo ? longo : longo_conhecido(saida->no), saida->sub, sizeof(saida->sub));
      if (!strcmp(saida->sub, saida->nome)) { saida->sub[0] = 0; }
    }
    ok = true;
  }
  destravar();
  return ok;
}

// Um lugar so para publicar, chamado de todos os pontos que mexem nos
// contadores. Espalhar a publicacao pelos pontos de alteracao seria repetir a
// regra de gravidade em cinco lugares - e esquecer num deles.
//
// SEM TRAVA: quem chama ja esta dentro dela. Tomar de novo travaria o mutex
// contra si mesmo (nao e recursivo).
void publicar_notif(void)
{
  // O TOTAL E DERIVADO, NUNCA MANTIDO A PARTE. Antes eram tres
  // contadores mexidos por aritmetica em quatro lugares (+1 ao chegar, -N ao
  // ler/apagar, zerar ao abrir o app) - e zerar o total ao abrir o app SEM
  // zerar as conversas os descolava: dali em diante, ler uma conversa antiga
  // subtraia mais do que o total tinha, o piso em zero engolia a diferenca e
  // uma mensagem NOVA de outra conversa sumia da barra. Somando as conversas
  // aqui, a barra e as bolinhas de cada conversa sao a mesma verdade.
  s_nao_lidas = 0;
  s_nao_lidas_dir = 0;
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (!s_conv[i].usada) { continue; }
    s_nao_lidas += s_conv[i].nao_lidas;
    if (!s_conv[i].canal) { s_nao_lidas_dir += s_conv[i].nao_lidas; }
  }
  if (s_notif < 0) { return; }
  char resumo[NOTIF_RESUMO];
  if (s_nao_lidas_dir > 0) {
    snprintf(resumo, sizeof(resumo), "%d direta%s", s_nao_lidas_dir,
             s_nao_lidas_dir > 1 ? "s" : "");
  } else {
    // QUAL CANAL, e nao so "quantas mensagens" - "1 no canal" nao dizia nada
    // quando ha mais de um canal na lista (LongFast, BR...). Sabendo o NOME
    // do unico canal com novidade, a barra de status ja diz onde olhar sem
    // precisar abrir o app so para descobrir.
    //
    // CHAMADA SEMPRE COM O MUTEX JA TOMADO pelo chamador (ver os quatro
    // pontos que chamam publicar_notif) - por isso le s_conv[] direto, sem
    // travar() de novo (destravaria duas vezes o mesmo lugar - deadlock).
    const char* nome_unico = nullptr;
    int canais_com_novidade = 0;
    for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
      if (s_conv[i].usada && s_conv[i].canal && s_conv[i].nao_lidas > 0) {
        ++canais_com_novidade;
        nome_unico = s_conv[i].nome;
      }
    }
    if (canais_com_novidade == 1 && nome_unico && nome_unico[0]) {
      snprintf(resumo, sizeof(resumo), "%d em %s", s_nao_lidas, nome_unico);
    } else if (canais_com_novidade > 1) {
      snprintf(resumo, sizeof(resumo), "%d em %d canais", s_nao_lidas, canais_com_novidade);
    } else {
      // Rede de seguranca: s_nao_lidas>0 sem nenhum canal marcado nao deveria
      // acontecer, mas sem isto a barra ficaria muda em vez de generica.
      snprintf(resumo, sizeof(resumo), tr("%d no canal"), s_nao_lidas);
    }
  }
  // Direta e ATENCAO: e endereçada a voce. Trafego de canal e INFO - vale
  // saber, nao exige nada.
  notif_publicar(s_notif, s_nao_lidas,
                 s_nao_lidas_dir > 0 ? NOTIF_ATENCAO : NOTIF_INFO, resumo);
}

void tab5_mesh_marcar_conversa_lida(int idx)
{
  travar();
  const int i = interno_de(idx);
  // O total e as diretas saem sozinhos: publicar_notif() reconta.
  if (i >= 0) {
    s_conv[i].nao_lidas = 0;
    // Lida de verdade: a guardada some junto, para nao voltar no proximo reinicio.
    if (nao_lida_guardada_t* g = guardada_de(s_conv[i].canal, s_conv[i].idx, s_conv[i].no, false)) { g->usada = false; }
  }
  publicar_notif();
  destravar();
}

// Le as ULTIMAS 'max' mensagens do arquivo. Le o arquivo inteiro mantendo uma
// janela circular em vez de tentar posicionar no fim: linha tem tamanho
// variavel, entao nao ha como saltar direto - e o arquivo e pequeno.
int tab5_mesh_historico(int conversa, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c;
  if (!tab5_mesh_conversa(conversa, &c)) { return 0; }
  return tab5_mesh_historico_de(c.canal, c.idx, c.no, saida, max);
}

int tab5_mesh_historico_de(bool canal, uint8_t idx, uint32_t no, mesh_msg_t* saida, int max)
{
  mesh_conversa_t c = { };
  c.canal = canal; c.idx = idx; c.no = no;
  if (!tab5_sd_montar()) { return 0; }

  char nome[32];
  arquivo_da_conversa(&c, nome, sizeof(nome));

  // Volta a usar o servico: quem sabe distinguir "ainda nao existe" de "deu
  // errado" e o tab5_sd, nao cada app. Conversa sem historico devolve 0
  // mensagens sem alarme no log.
  FILE* f = tab5_sd_abrir(MESH_PASTA, nome, "r");
  if (!f) { tab5_sd_desmontar(); return 0; }
  // SO O FIM DO ARQUIVO. Daqui so saem as 'max' ultimas mensagens, e uma linha
  // tem no maximo sizeof(linha) bytes: as ultimas max * sizeof(linha) bytes
  // bastam. Antes lia o arquivo INTEIRO a cada abertura - o LongFast, com o
  // trafego do MQTT, juntava megabytes e a conversa demorava segundos para
  // abrir. Buffer de 16 KB na PSRAM: o padrao do stdio le aos pedacinhos.
  char* vbuf = (char*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
  if (vbuf) { setvbuf(f, vbuf, _IOFBF, 16 * 1024); }
  {
    const long cauda = (long)max * (long)(MESH_TEXTO + 80);
    long tam = -1;
    if (fseek(f, 0, SEEK_END) == 0) { tam = ftell(f); }
    if (tam > cauda && fseek(f, tam - cauda, SEEK_SET) == 0) {
      int ch;
      while ((ch = fgetc(f)) != EOF && ch != '\n') { }   // a linha cortada no meio
    } else {
      fseek(f, 0, SEEK_SET);
    }
  }

  // Desfechos carregados UMA VEZ, antes do loop. Ver o comentario da tabela.
  static desfecho_t desf[MESH_DESFECHOS];
  const int n_desf = carregar_desfechos(desf, MESH_DESFECHOS);

  int n = 0, primeiro = 0, voltas = 0;
  // TEM QUE CABER O PREFIXO (~60 bytes: "M4,quando,de,snr,rssi,vmqtt,saltos,
  // id,") MAIS O TEXTO INTEIRO (ate MESH_TEXTO-1=237 bytes, ver o aviso la)
  // MAIS a quebra de linha - senao esta funcao vira o MESMO tipo de
  // truncamento que a leitura ao vivo tinha (MESH_TEXTO pequeno demais),
  // so que ao reler do cartao em vez de ao receber.
  char linha[MESH_TEXTO + 80];
  while (fgets(linha, sizeof(linha), f)) {
    if ((++voltas % 128) == 0) { vTaskDelay(1); }   // nao segurar o core
    const bool fmt_m4 = (strncmp(linha, "M4,", 3) == 0);
    const bool fmt_m3 = !fmt_m4 && (strncmp(linha, "M3,", 3) == 0);
    const bool fmt_m2 = !fmt_m4 && !fmt_m3 && (strncmp(linha, "M2,", 3) == 0);
    if (!fmt_m4 && !fmt_m3 && !fmt_m2 && strncmp(linha, "M,", 2) != 0) { continue; }

    mesh_msg_t m; memset(&m, 0, sizeof(m));
    unsigned long quando = 0, de = 0, id = 0; float snr = 0; long rssi = 0;
    int vmqtt = 0;
    unsigned saltos = MESH_SALTOS_DESCONHECIDO;
    // %n devolve onde o texto comeca: tudo dali ate o fim da linha e a mensagem,
    // inclusive virgulas.
    int pos = 0;
    if (fmt_m4) {
      if (sscanf(linha, "M4,%lu,%8lX,%f,%ld,%d,%u,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &saltos, &id, &pos) < 7) { continue; }
    } else if (fmt_m3) {
      // M3 tem via_mqtt mas nao saltos (campo chegou logo depois, mesma leva
      // ) - fica MESH_SALTOS_DESCONHECIDO, o sentinela de
      // "nao da para saber", nunca um numero inventado.
      if (sscanf(linha, "M3,%lu,%8lX,%f,%ld,%d,%8lX,%n",
                 &quando, &de, &snr, &rssi, &vmqtt, &id, &pos) < 6) { continue; }
    } else if (fmt_m2) {
      // M2 nao tem via_mqtt nem saltos - via_mqtt fica false, que e a
      // suposicao mais segura: mensagem antiga muito provavelmente veio pelo
      // radio mesmo, e um valor errado aqui so troca um icone, nunca perde
      // texto nem confunde entrega.
      if (sscanf(linha, "M2,%lu,%8lX,%f,%ld,%8lX,%n",
                 &quando, &de, &snr, &rssi, &id, &pos) < 5) { continue; }
    } else {
      // Formato antigo, sem id. A mensagem aparece igual; so nao ha marca de
      // entrega - e nao ha mesmo, porque o estado morreu no reinicio.
      if (sscanf(linha, "M,%lu,%8lX,%f,%ld,%n",
                 &quando, &de, &snr, &rssi, &pos) < 4) { continue; }
    }
    m.quando = (uint32_t)quando; m.de = (uint32_t)de; m.snr = snr; m.rssi = (int32_t)rssi;
    m.via_mqtt = (vmqtt != 0);
    m.saltos = (uint8_t)saltos;
    m.id = (uint32_t)id;
    // ANEL PRIMEIRO, CARTAO DEPOIS. O anel tem o estado ao vivo, inclusive o
    // ENVIANDO que nunca vai para o cartao; o cartao tem os desfechos de todos
    // os envios que ja sairam do anel ou de antes do ultimo reinicio.
    m.envio = tab5_mesh_estado_envio(m.id, &m.erro);
    if (m.envio == MESH_ENVIO_NA) { m.envio = buscar_desfecho(desf, n_desf, m.id, &m.erro); }
    const uint32_t corrigido = buscar_carimbo(desf, n_desf, m.id);
    if (corrigido) { m.quando = corrigido; }
    snprintf(m.texto, sizeof(m.texto), "%s", linha + pos);
    m.texto[strcspn(m.texto, "\r\n")] = 0;
    for (char* c = m.texto; *c; ++c) { if (*c == '\x1f') { *c = '\n'; } }   // a quebra de linha (ver gravar_msg)

    saida[(primeiro + n) % max] = m;
    if (n < max) { ++n; } else { primeiro = (primeiro + 1) % max; }
  }
  fclose(f);
  heap_caps_free(vbuf);
  tab5_sd_desmontar();

  // Desenrola a janela para a ordem cronologica.
  if (primeiro != 0 && n == max) {
    mesh_msg_t tmp[1];
    for (int i = 0; i < n; ++i) {
      // rotacao simples: barata para dezenas de itens
      tmp[0] = saida[0];
      for (int k = 0; k < n - 1; ++k) { saida[k] = saida[k+1]; }
      saida[n-1] = tmp[0];
      if (--primeiro == 0) { break; }
    }
  }
  return n;
}
