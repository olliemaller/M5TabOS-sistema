// Comandos do console - SISTEMA: estado, tarefas, memoria, energia, musica, fundo, log, berry, pendrive, usb.
// (Partido de console.cpp; a tabela COMANDOS e a ajuda continuam la.)

#include "tab5_heap_psram.hpp"
#include "tab5_musica.hpp"
#include "tab5_video.hpp"
#include "tab5_h264.hpp"
#include "tab5_arquivos.hpp"   // TAB5_ARQ_CAMINHO_MAX
#include "tab5_bateria.hpp"
#include "tab5_ensaio.hpp"
#include "tab5_hora.hpp"   // tab5_agora_ms
#include "tab5_fuso.hpp"
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "tab5_pendrive.hpp"
#include "tab5_usb.hpp"
#include "tab5_volumes.hpp"
#include "tab5_teclado.hpp"
#include "tab5_wifi.hpp"
#include "tab5_grove.hpp"
#include "tab5_mesh_admin.hpp"  // mesh_cfg_ler (Wi-Fi do no)
#include "lwip/netdb.h"
#include "esp_app_desc.h"   // esp_app_desc_t (versao do programa)
#include "registro.h"
#include "tab5_log.hpp"
#include "ajustes.h"
#include "tab5_ble.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel
#include "tab5_monitor.hpp"   // tab5_mem_extrato
#include "console/console_interno.hpp"

// LE O QUE O MONITOR JA MEDIU (tab5_monitor_foto), em vez de tirar duas fotos
// proprias de uxTaskGetSystemState: cada foto e uma secao critica longa (a que
// ja derrubou frame do painel) e o retrato pedia 2 x 40 TaskStatus_t de
// malloc interno. A foto do monitor e do ultimo segundo, com os donos.
static void cmd_tarefas(void)
{
  mon_foto_t* f = (mon_foto_t*)heap_caps_malloc(sizeof(mon_foto_t), MALLOC_CAP_SPIRAM);
  if (!f) { printf("%s", tr("sem memoria para o retrato\n")); return; }
  if (!tab5_monitor_foto(f)) { heap_caps_free(f); printf("%s", tr("monitor ainda sem a primeira amostra\n")); return; }

  // EM QUE ESTADO ISTO FOI MEDIDO. Sem esta linha a medida e ambigua: o mesmo
  // comando com a tela acesa e com ela apagada da numeros muito diferentes, e
  // nada no retrato dizia qual dos dois voce estava olhando. Digitar no serial
  // NAO acorda a tela (so o toque acorda), entao da para medir o repouso de
  // verdade - mas so se der para conferir que era repouso mesmo.
  printf(tr("--- %d tarefas   [%s]   CPU %.1f%%   foto %lu us (pior %lu) ---\n"), f->n,
         tab5_power_em_repouso() ? tr("EM REPOUSO - tela apagada") : tr("tela acesa"),
         (double)f->cpu, (unsigned long)f->foto_us, (unsigned long)f->foto_pior_us);
  printf("  %-16s %3s %6s %6s %10s %5s  %s\n", tr("nome"), tr("pri"), tr("nucleo"), tr("CPU%"), tr("pilha livre"), tr("onde"), tr("dono"));

  for (int i = 0; i < f->n; ++i) {
    const mon_tarefa_t& t = f->t[i];
    // xCoreID vale tskNO_AFFINITY (0x7FFFFFFF) quando a tarefa roda nos dois -
    // testar por negativo nunca acerta, e imprimir cru mostra um numero
    // gigante. Foi um bug real na coluna CORE do app.
    char nucleo[8];
    if (t.core == tskNO_AFFINITY) { snprintf(nucleo, sizeof(nucleo), "0e1"); }
    else                          { snprintf(nucleo, sizeof(nucleo), "%d", (int)t.core); }
    // "onde": a pilha na SRAM interna (escassa) ou na PSRAM.
    printf("  %-16s %3u %6s %6.1f %8u B %5s  %s\n",
           t.nome, (unsigned)t.prioridade, nucleo, (double)t.pct,
           (unsigned)t.pilha_livre, t.pilha_psram ? "PSRAM" : "SRAM", t.dono);
  }
  // A CONTA E DO APARELHO, NAO POR NUCLEO: as porcentagens somam 100% (os
  // dois nucleos juntos), a mesma conta do app Tarefas. Os dois IDLE juntos
  // sao a ociosidade do aparelho; um nucleo inteiro ocupado aparece como 50%.
  printf("%s", tr("\n  CPU%% do aparelho (os dois nucleos somam 100%%): um nucleo cheio = 50%%.\n"));
  heap_caps_free(f);
}

static void cmd_memoria(void)
{
  // O QUE O idf.py size NAO CONTA.
  //
  // O relatorio da compilacao mostra so o ESTATICO: .text, .data, .bss. Ele nao
  // sabe nada do que e alocado depois - stack de tarefa, buffer de UART, queue
  // do WiFi, os dois framebuffers de 1,8MB do painel. E justamente isso que
  // decide se cabe mais um app.
  printf("%s", tr("--- memoria ---\n"));

  const size_t i_liv = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  const size_t i_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const size_t i_blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  const size_t p_liv = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t p_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  const size_t p_blk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  printf(tr("  SRAM do P4  : %u KB uteis (768 - cache L2); heap %u KB, fixo (codigo+dados) %u KB\n"),
         (unsigned)((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(((768u * 1024u - (unsigned)CONFIG_CACHE_L2_CACHE_SIZE) - i_tot) / 1024));
  printf(tr("  RAM interna : %u de %u KB em uso (%u%%), livre %u KB\n"),
         (unsigned)((i_tot - i_liv) / 1024), (unsigned)(i_tot / 1024),
         (unsigned)(i_tot ? ((i_tot - i_liv) * 100 / i_tot) : 0),
         (unsigned)(i_liv / 1024));
  // O "minimo ja visto" e o numero que importa para saber se falta: o livre de
  // agora nao mostra o pico de uso que ja passou.
  printf(tr("                minimo ja visto %u KB, maior bloco %u KB\n"),
         (unsigned)(i_min / 1024), (unsigned)(i_blk / 1024));

  printf(tr("  PSRAM       : %u de %u KB em uso (%u%%), livre %u KB\n"),
         (unsigned)((p_tot - p_liv) / 1024), (unsigned)(p_tot / 1024),
         (unsigned)(p_tot ? ((p_tot - p_liv) * 100 / p_tot) : 0),
         (unsigned)(p_liv / 1024));
  printf(tr("                minimo ja visto %u KB, maior bloco %u KB\n"),
         (unsigned)(p_min / 1024), (unsigned)(p_blk / 1024));

  uint32_t prog_total;
  if (tab5_monitor_programa(nullptr, &prog_total)) {
    printf(tr("  particao    : %u KB para o programa\n"), (unsigned)(prog_total / 1024));
  }
}

// MAPA DA RAM INTERNA - onde esta cada byte e quao picotado esta o livre.
//
// "livre 80 KB" nao diz se da para alocar 16 KB: se o livre estiver em 40
// pedacos de 2 KB, nao da. O heap do IDF (TLSF) e dividido em REGIOES (cada
// faixa de endereco que sobrou depois do .bss, a TCM, a RAM do LP, a reserva
// DMA); um bloco nunca atravessa regioes. Aqui cada bloco de cada regiao e
// visitado (heap_caps_walk) e contado por faixa de tamanho, usados e livres.
//
// COMO LER:
//   - blocos LIVRES pequenos e muitos = fragmentacao. O que importa e o
//     "maior" de cada regiao, nao a soma;
//   - blocos USADOS entre 4 e 16 KB sao malloc() comuns que o
//     SPIRAM_MALLOC_ALWAYSINTERNAL (4096) obriga a ficar na interna. Se forem
//     muitos, baixar esse limite os manda para a PSRAM.
#define MAPA_FAIXAS 6
static const uint32_t s_faixa_lim[MAPA_FAIXAS] = { 64, 256, 1024, 4096, 16384, UINT32_MAX };
static const char*    s_faixa_nome[MAPA_FAIXAS] = { "<64", "<256", "<1K", "<4K", "<16K", ">=16K" };
#define MAPA_REGIOES 8

typedef struct {
  intptr_t ini, fim;
  uint32_t usado, livre, maior, n_livres, n_usados;
  uint32_t livres_n[MAPA_FAIXAS], usados_n[MAPA_FAIXAS], usados_b[MAPA_FAIXAS];
} mapa_regiao_t;

typedef struct {
  mapa_regiao_t r[MAPA_REGIOES];
  int n;
  bool estourou;
} mapa_t;

// Roda com a trava do heap presa: nada de printf nem malloc aqui dentro.
static bool mapa_visitar(walker_heap_into_t h, walker_block_info_t b, void* u)
{
  mapa_t* m = (mapa_t*)u;
  mapa_regiao_t* r = nullptr;
  for (int i = 0; i < m->n; ++i) { if (m->r[i].ini == h.start) { r = &m->r[i]; break; } }
  if (!r) {
    if (m->n >= MAPA_REGIOES) { m->estourou = true; return false; }
    r = &m->r[m->n++];
    memset(r, 0, sizeof(*r));
    r->ini = h.start; r->fim = h.end;
  }
  int f = 0;
  while (f < MAPA_FAIXAS - 1 && b.size >= s_faixa_lim[f]) { ++f; }
  if (b.used) {
    r->usado += b.size; r->n_usados++;
    r->usados_n[f]++; r->usados_b[f] += b.size;
  } else {
    r->livre += b.size; r->n_livres++;
    r->livres_n[f]++;
    if (b.size > r->maior) { r->maior = b.size; }
  }
  return true;
}

static void cmd_mem_mapa(void)
{
  // static: ~1 KB, fora da pilha do console.
  static mapa_t m;
  memset(&m, 0, sizeof(m));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &m);

  printf("%s", tr("--- mapa da RAM interna, por regiao ---\n"));
  uint32_t u_n[MAPA_FAIXAS] = {0}, u_b[MAPA_FAIXAS] = {0}, l_n[MAPA_FAIXAS] = {0};
  for (int i = 0; i < m.n; ++i) {
    const mapa_regiao_t& r = m.r[i];
    const uint32_t frag = r.livre ? (uint32_t)(100 - (uint64_t)r.maior * 100 / r.livre) : 0;
    printf(tr("  %08x-%08x %4u KB: usado %4u KB em %4u blocos | livre %4u KB em %3u, maior %3u KB (frag %u%%)\n"),
           (unsigned)r.ini, (unsigned)r.fim, (unsigned)((r.fim - r.ini) / 1024),
           (unsigned)(r.usado / 1024), (unsigned)r.n_usados,
           (unsigned)(r.livre / 1024), (unsigned)r.n_livres, (unsigned)(r.maior / 1024),
           (unsigned)frag);
    for (int f = 0; f < MAPA_FAIXAS; ++f) {
      u_n[f] += r.usados_n[f]; u_b[f] += r.usados_b[f]; l_n[f] += r.livres_n[f];
    }
  }
  if (m.estourou) { printf(tr("  (mais regioes que %d - lista cortada)\n"), MAPA_REGIOES); }

  printf("%s", tr("\n  faixa    usados (qtd / KB)   livres (qtd)\n"));
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf("  %-6s   %5u / %5u       %5u\n", s_faixa_nome[f],
           (unsigned)u_n[f], (unsigned)(u_b[f] / 1024), (unsigned)l_n[f]);
  }
}

// CACA AO VAZAMENTO: 'mem marca' tira uma foto do heap; 'mem delta' diz o que
// mudou desde ela. O uso e abrir e fechar o app (ou ligar e desligar o servico)
// algumas vezes entre os dois: o que NAO volta ao valor da marca e o que ficou
// preso. Compara bytes e QUANTIDADE de blocos usados (uma lista que cresce de
// 1 em 1 bloco escapa da conta em bytes), por faixa de tamanho na interna (a
// faixa que cresce diz o tipo de coisa: pilha, buffer, objeto pequeno), e as
// tarefas vivas (uma tarefa que nao morre segura a pilha dela).
typedef struct {
  bool     ok;
  mapa_t   mapa;                 // so a interna, por faixa
  uint32_t i_bytes, i_blocos;    // total usado, internal
  uint32_t p_bytes, p_blocos;    // total usado, PSRAM
  uint32_t tarefas;
} marca_mem_t;
static marca_mem_t s_marca_mem;

static void foto_mem(marca_mem_t* o)
{
  memset(o, 0, sizeof(*o));
  heap_caps_walk(MALLOC_CAP_INTERNAL, mapa_visitar, &o->mapa);
  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
  o->i_bytes = (uint32_t)hi.total_allocated_bytes; o->i_blocos = (uint32_t)hi.allocated_blocks;
  heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
  o->p_bytes = (uint32_t)hi.total_allocated_bytes; o->p_blocos = (uint32_t)hi.allocated_blocks;
  o->tarefas = (uint32_t)uxTaskGetNumberOfTasks();
  o->ok = true;
}

static void cmd_mem_marca(void)
{
  foto_mem(&s_marca_mem);
  printf(tr("marca tirada: interna %u KB em %u blocos, PSRAM %u KB em %u blocos, %u tarefas.\n"
         "Abra e feche o app algumas vezes e rode 'mem delta'.\n"),
         (unsigned)(s_marca_mem.i_bytes / 1024), (unsigned)s_marca_mem.i_blocos,
         (unsigned)(s_marca_mem.p_bytes / 1024), (unsigned)s_marca_mem.p_blocos,
         (unsigned)s_marca_mem.tarefas);
}

static void cmd_mem_delta(void)
{
  if (!s_marca_mem.ok) { printf("%s", tr("sem marca - rode 'mem marca' primeiro\n")); return; }
  static marca_mem_t agora;
  foto_mem(&agora);
  printf("%s", tr("--- o que mudou desde a marca ---\n"));
  printf(tr("  interna : %+ld B em %+ld blocos\n"), (long)agora.i_bytes - (long)s_marca_mem.i_bytes,
         (long)agora.i_blocos - (long)s_marca_mem.i_blocos);
  printf(tr("  PSRAM   : %+ld B em %+ld blocos\n"), (long)agora.p_bytes - (long)s_marca_mem.p_bytes,
         (long)agora.p_blocos - (long)s_marca_mem.p_blocos);
  printf(tr("  tarefas : %+ld\n"), (long)agora.tarefas - (long)s_marca_mem.tarefas);
  uint32_t a_n[MAPA_FAIXAS] = {0}, a_b[MAPA_FAIXAS] = {0}, m_n[MAPA_FAIXAS] = {0}, m_b[MAPA_FAIXAS] = {0};
  for (int i = 0; i < agora.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { a_n[f] += agora.mapa.r[i].usados_n[f]; a_b[f] += agora.mapa.r[i].usados_b[f]; }
  }
  for (int i = 0; i < s_marca_mem.mapa.n; ++i) {
    for (int f = 0; f < MAPA_FAIXAS; ++f) { m_n[f] += s_marca_mem.mapa.r[i].usados_n[f]; m_b[f] += s_marca_mem.mapa.r[i].usados_b[f]; }
  }
  printf("%s", tr("\n  interna, blocos usados por faixa (variacao):\n"));
  for (int f = 0; f < MAPA_FAIXAS; ++f) {
    printf(tr("  %-6s   %+5ld blocos  %+7ld B\n"), s_faixa_nome[f],
           (long)a_n[f] - (long)m_n[f], (long)a_b[f] - (long)m_b[f]);
  }
  printf("%s", tr("\n  Tudo perto de zero depois de abrir/fechar = sem vazamento. Se o mesmo\n"
         "  numero cresce a cada ciclo, e vazamento: a faixa mostra o tipo de bloco.\n"));
}

static void cmd_mem_extrato(void)
{
  static mem_marco_t e[MEM_MARCOS_N];
  const int n = tab5_mem_extrato(e, MEM_MARCOS_N);
  printf("%s", tr("--- extrato da RAM interna (boot e subida dos servicos) ---\n"));
  printf("  %8s  %-26s %8s %8s %8s %8s\n", tr("ms"), tr("etapa"), tr("livre"), tr("delta"), tr("maior"), tr("minimo"));
  for (int i = 0; i < n; ++i) {
    printf("  %8lu  %-26s %8lu %+8ld %8lu %8lu\n", (unsigned long)e[i].ms, e[i].etapa,
           (unsigned long)e[i].livre, (long)e[i].delta, (unsigned long)e[i].maior_bloco,
           (unsigned long)e[i].minimo);
  }
  if (n == MEM_MARCOS_N) { printf("%s", tr("  (cheio: marcos seguintes descartados)\n")); }
}

// QUEM ESTA PROIBINDO O LIGHT SLEEP.
//
// Toda a economia de energia do IDF passa pelas travas de PM. Um driver que
// precisa de clock estavel cria uma trava na inicializacao e a segura; enquanto
// houver UMA trava viva, o sistema nao desce para o modo que ela proibe - e nao
// ha aviso nenhum, o esp_pm simplesmente nao dorme.
//
// Foi assim que se descobriu que o painel DPI segurava duas travas
// desde a criacao e so as soltava no destrutor - o que levou ao
// DESMONTAR_DSI_NO_REPOUSO em tab5_power.cpp. Sem este despejo, aquilo teria
// sido adivinhacao.
//
// COMO USAR: o numero que interessa nao e este, com a tela acesa. E o de DENTRO
// do repouso - e por isso o comando aceita um atraso em segundos:
//
//     energia travas 30      (deixa a tela apagar e so entao despeja)
//
// Tipos que importam: NO_LIGHT_SLEEP proibe dormir; CPU_FREQ_MAX prende o
// clock no teto; APB_FREQ_MAX prende o barramento - e este tambem impede o
// light sleep, porque dormir pararia o APB.
static void cmd_travas(int segundos)
{
#if CONFIG_PM_ENABLE
  // O tempo de tela vem de Ajustes e pode ser qualquer coisa. Dizer qual e
  // evita a medida inutil mais provavel: esperar menos que ele e despejar as
  // travas com a tela ainda acesa, quando o painel ainda segura as duas.
  const int tela = (int)ajustes_tela_s();
  if (segundos > 0 && segundos <= tela) {
    printf(tr("AVISO: a tela apaga em %d s e voce pediu %d. Vai medir acordado.\n"
           "       use 'energia travas %d' ou mais.\n"), tela, segundos, tela + 10);
  }

  if (segundos > 0) {
    printf(tr("esperando %d s - NAO toque na tela (o teclado do serial pode usar)...\n"),
           segundos);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)segundos * 1000));
  }

  const bool dormindo = tab5_power_em_repouso();
  printf(tr("--- travas de PM  [%s] ---\n"),
         dormindo ? tr("EM REPOUSO - e esta a medida que vale")
                  : tr("TELA ACESA - esta medida NAO vale para o light sleep"));
  esp_pm_dump_locks(stdout);

  if (dormindo) {
    printf("%s", tr("\nvazio, ou so travas soltas = caminho livre para o light sleep\n"
           "dsi_dpi / dsi_phy presos    = o desmonte da tela nao aconteceu\n"
           "uart_driver preso           = e o Grove; decisao de projeto antes\n"));
  } else {
    printf(tr("\na tela nao apagou. tempo configurado: %d s.\n"
           "tente 'energia travas %d' e nao toque na tela durante a espera.\n"),
           tela, tela + 10);
  }
#else
  (void)segundos;
  printf("%s", tr("CONFIG_PM_ENABLE desligado: nao ha travas para mostrar\n"));
#endif
}

// FORCA O REPOUSO AGORA, no clock que voce escolher - para descobrir se um
// piso MENOR que os 40MHz de hoje (ver o "int piso = ... : 40" em
// core.cpp) e aceito pelo silicio e pelo painel, sem regravar a cada
// numero novo. Mesma ideia do 'tela relogio <mhz>' - comparar em execucao em
// vez de adivinhar.
//
// SEGURO CHAMAR DO CONSOLE (uma tarefa diferente da interface): reli
// tab5_power_repouso()/retomar() e nenhuma das duas toca I2C - so mexem no
// painel DSI, no PM (esp_pm_configure) e no AXI-ICM. A regra de "so a
// interface fala com o barramento interno" e sobre o INA226/touch/teclado/
// IMU, e nao vale aqui.
//
// UM CUIDADO QUE FICA COM QUEM TESTA: se o repouso AUTOMATICO (o timer de
// ociosidade da tela, em Ajustes) disparar no MESMO instante que este comando,
// as duas chamadas correm sem trava nenhuma por cima do mesmo estado
// (s_em_repouso e companhia sao globais do modulo, sem mutex) - e o resultado
// seria desmontar o painel duas vezes. Na pratica a janela e estreitissima
// (o guard "ja em repouso?" barra o caso comum), mas para testar com sossego
// aumente o tempo de tela em Ajustes antes, ou teste logo depois de tocar a
// tela (o relogio de ociosidade acabou de reiniciar).
//
// Se o MHz pedido nao existir nesta revisao de silicio (validos: 360, 180,
// 90 e XTAL/N -> 40, 20, 10, 8, 5, 4, 2, 1), tab5_power_repouso() ja desfaz
// sozinho e devolve false - a tela volta a acender, sem travar nada.
static void cmd_repouso_forcar(int mhz)
{
  if (mhz <= 0) {
    printf("uso: energia repouso <mhz>   (ex: energia repouso 20)\n"
           "valores validos nesta placa: " TAB5_CPU_DEGRAUS "\n");
    return;
  }
  if (tab5_power_em_repouso()) {
    printf("%s", tr("ja esta em repouso - toque a tela para acordar e tente de novo\n"
           "(chamar de novo agora nao faz nada: tab5_power_repouso() e "
           "idempotente enquanto dormindo)\n"));
    return;
  }

  printf(tr("entrando em repouso a %d MHz...\n"), mhz);
  fflush(stdout);
  const bool ok = tab5_power_repouso(mhz);
  if (ok) {
    printf("%s", tr("repouso confirmado. a tela esta apagada, mas este console continua\n"
           "vivo - use 'energia' agora (e de novo daqui uns segundos, para a\n"
           "corrente assentar) para medir. toque a tela para acordar.\n"));
  } else {
    printf("%s", tr("o repouso RECUSOU esse clock - o log de erro acima (ESP_LOGE de "
           "'power') explica o motivo. a tela deve ter voltado a acender sozinha.\n"));
  }
}

// TETO DO CPU COM A TELA ACORDADA - ver o aviso grande em tab5_power.hpp.
// Diferente de 'energia repouso', a tela NAO apaga: o painel continua ligado,
// desenhando, no relogio de pixel que estiver configurado agora ('tela
// relogio <mhz>'). Existe para testar a combinacao que faltava: painel devagar
// E CPU devagar, os dois ACORDADOS ao mesmo tempo.
static void cmd_teto_cpu(int mhz)
{
  if (mhz <= 0) {
    printf("uso: energia teto <mhz>   (ex: energia teto 180)\n"
           "combine with 'display clock <mhz>' first if testing both together\n"
           "valores validos nesta placa: " TAB5_CPU_DEGRAUS "\n");
    return;
  }
  const bool ok = tab5_power_teto_cpu(mhz);
  if (ok) {
    printf(tr("teto aceito - a tela continua ligada, de olho em underrun/piscada azul\n"
           "agora. Se der problema, 'energia teto %d' desfaz na hora.\n"
           "\n"
           "ATENCAO se for deixar o aparelho parado: o repouso fotografa o clock\n"
           "ATUAL como \"normal\" e acorda de volta nele - se a tela apagar sozinha\n"
           "com o teto ainda em %d, ela acorda PRESA em %d, nao em %d. Volte para\n"
           "%d antes de largar o aparelho, ou abra/feche qualquer app (que\n"
           "reaplica o perfil e resolve sozinho).\n"),
           TAB5_CPU_MAX_MHZ, mhz, mhz, TAB5_CPU_MAX_MHZ, TAB5_CPU_MAX_MHZ);
  } else {
    printf("%s", tr("RECUSADO - o log de erro acima ('power') explica o motivo.\n"));
  }
}

static void cmd_energia(void)
{
  // LEITURA DE CACHE, NAO DO INA226.
  //
  // O I2C_Class do M5Unified nao tem mutex: se esta tarefa for ao INA226
  // enquanto a interface le o touch, as duas transacoes se intercalam e voce le
  // registrador errado - sem erro nenhum, so numero sem sentido. A interface
  // mede uma vez por segundo e deixa o valor aqui; e daqui que se le.
  const tab5_medida_t m = tab5_power_ultima();

  printf("%s", tr("--- energia ---\n"));
  printf(tr("  agora    : %.2f V   %.0f mA   %.0f mW   %d%%   CPU %d MHz\n"),
         m.volts, m.mA, m.mW, m.pct, tab5_power_cpu_mhz());
  printf("             %s\n",
         (m.mA > 20)  ? tr("carregando")
       : (m.mA < -20) ? tr("na bateria") : tr("parado / na tomada sem carregar"));

  // O ESTIMADOR, ao vivo - ver estimar_pct() em tab5_power.cpp. A diferenca
  // entre "estimador" e "curva" e o que a contagem de mAh esta corrigindo.
  {
    float soc, curva, desde; const char* fonte;
    tab5_power_soc_debug(&soc, &curva, &fonte, &desde);
    printf(tr("  bateria  : estimador %.1f%%  | curva de tensao diria %.0f%%  | partiu de: %s\n"),
           soc, curva, fonte);
    if (desde >= 0) {
      printf(tr("             ultima carga ha %.0f min%s\n"), desde,
             desde < 60 ? tr(" (a curva ainda nao corrige - tensao pos-carga mente)") : "");
    }
    char guarda[64];
    tab5_bateria_resumo(guarda, sizeof(guarda));
    printf(tr("  guarda   : %s\n"), guarda);
  }

  tab5_ponto_t h[TAB5_HIST_N];
  const int n = tab5_power_historico(h);
  if (n <= 1) {
    printf(tr("\n  historico ainda vazio (uma amostra a cada %d s)\n"), TAB5_HIST_SEG);
    return;
  }

  // Escala da barra pelo MAIOR consumo da janela, e nao por um teto fixo: com
  // teto fixo, uma janela inteira em repouso viraria uma coluna de nada.
  float pico = 1.0f;
  for (int i = 0; i < n; ++i) { const float v = fabsf(h[i].mA); if (v > pico) pico = v; }

  printf(tr("\n  ultimos %d min (uma linha por minuto, barra = corrente ate %.0f mA)\n"),
         (n * TAB5_HIST_SEG) / 60, pico);
  printf("  %6s %7s %11s\n", tr("quando"), tr("tensao"), tr("corrente"));

  // De duas em duas amostras: com TAB5_HIST_SEG=30 isso da uma linha por
  // minuto, e 30 linhas cabem numa tela de terminal. Todas as 60 obrigariam a
  // rolar para ver o comeco.
  for (int i = 0; i < n; i += 2) {
    const int min_atras = ((n - 1 - i) * TAB5_HIST_SEG) / 60;
    char barra[33];
    int larg = (int)((fabsf(h[i].mA) / pico) * 32.0f);
    if (larg < 0)  { larg = 0; }
    if (larg > 32) { larg = 32; }
    memset(barra, '#', (size_t)larg);
    barra[larg] = 0;
    printf(tr("  -%3d m %6.2fV %7.0f mA  %s\n"), min_atras, h[i].volts, h[i].mA, barra);
  }
}

// ---------------------------------------------------------------------------
// SISTEMA
// ---------------------------------------------------------------------------

// Uma linha por servico do sistema, no mesmo lugar. A primeira pergunta quando
// algo estranho acontece e "o que esta de pe agora?" - e sem isto a resposta
// exigia cacar linha de log de tres tarefas diferentes.
void c_estado(int, char**)
{
  const esp_app_desc_t* app = esp_app_get_description();
  const uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
  const char* aberto = sistema_app_atual();
  printf(tr("--- M5tabOS %s ---\n"), app->version);
  // Bloqueada, a interface mostra so a tela de bloqueio: o app aberto NAO
  // desenha ate alguem destravar (no teste pelo console isso parecia defeito).
  const char* situacao = sistema_bloqueado()     ? tr("   (tela bloqueada: o app nao desenha)")
                       : tab5_power_em_repouso() ? tr("   (em repouso)") : "";
  printf(tr("  ligado ha: %luh%02lum%02lus   app aberto: %s%s\n"),
         (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60),
         aberto ? aberto : tr("nenhum (tela inicial)"), situacao);
  printf(tr("  memoria  : interna %u KB livre (minimo %u KB), PSRAM %u KB livre\n"),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  printf(tr("  cartao   : %s\n"), tab5_sd_descricao());

  const char* est = tab5_wifi_descricao();
  const int r = (int)tab5_wifi_rssi();
  if (tab5_wifi_estado() == TAB5_WIFI_CONECTADO) {
    char sinal[12];
    if (r == TAB5_WIFI_RSSI_DESCONHECIDO) { snprintf(sinal, sizeof(sinal), "sinal --"); }
    else { snprintf(sinal, sizeof(sinal), "%ddBm", r); }
    printf(tr("  wifi     : %s  %s  %s\n"), est, tab5_wifi_ip(), sinal);
  } else {
    printf(tr("  wifi     : %s\n"), est);
  }
  printf(tr("  bluetooth: %s\n"), tab5_ble_descricao());

  printf(tr("  teclado  : %s\n"), tab5_teclado_presente() ? tr("encaixado") : tr("ausente"));

  printf(tr("  grove 5V : %s   detectado: %s\n"),
         ajustes_grove() ? tr("ligado") : tr("desligado"), grove_nome(tab5_grove_tipo()));

  if (tab5_mesh_meu_numero() != 0 || tab5_mesh_no_ar()) {
    uint32_t q, pk, tel, ls, tx, ni; int var;
    tab5_mesh_contadores(&q, &pk, &tel, &ls, &tx, &ni, &var);
    printf(tr("  mesh     : %s, %d nos, %lu quadros, %lu pacotes\n"),
           tab5_mesh_no_ar() ? tr("no ar") : tr("mudo"),
           tab5_mesh_num_nos(), (unsigned long)q, (unsigned long)pk);
  } else {
    printf("%s", tr("  mesh     : nao iniciado\n"));
  }

  int na_ram; uint32_t grav, falhas;
  registro_estado(&na_ram, &grav, &falhas);
  printf(tr("  registro : %d na RAM, %lu gravadas, %lu falhas\n"),
         na_ram, (unsigned long)grav, (unsigned long)falhas);

  {
    size_t pend; uint32_t lgrav, lperd; char arq[24];
    tab5_log_estado(&pend, &lgrav, &lperd, arq, sizeof(arq));
    printf(tr("  log      : %u byte(s) na RAM, %lu linha(s) gravadas, %lu perdidas (%s)\n"),
           (unsigned)pend, (unsigned long)lgrav, (unsigned long)lperd,
           arq[0] ? arq : tr("ainda sem descarga"));
  }

  tab5_medida_t m = tab5_power_ultima();
  printf(tr("  energia  : %dMHz %.2fV %.1fmA %d%%\n"),
         tab5_power_cpu_mhz(), m.volts, m.mA, m.pct);
}

void c_tarefas(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "memoria")) { cmd_memoria(); }   // forma antiga
  else                                      { cmd_tarefas(); }
}

void c_mem(int argc, char** argv)
{
  const char* o = (argc >= 2) ? argv[1] : "";
  if (!*o) {
    cmd_memoria();
    printf("%s", tr("\n  'mem extrato' e 'mem mapa' mostram para onde a RAM interna foi\n"));
  }
  else if (eh(o, "extrato")) { cmd_mem_extrato(); }
  else if (eh(o, "mapa"))    { cmd_mem_mapa(); }
  else if (eh(o, "marca"))   { cmd_mem_marca(); }
  else if (eh(o, "delta"))   { cmd_mem_delta(); }
  else if (eh(o, "integridade")) {
    // O heap inteiro (interna e PSRAM): a estrutura e, com o envenenamento
    // leve ligado (sdkconfig.defaults), as marcas antes e depois de cada
    // bloco. true = imprime o que achou de errado e onde.
    printf("%s", tr("conferindo o heap inteiro...\n"));
    fflush(stdout);
    const bool ok = heap_caps_check_integrity_all(true);
    printf(ok ? tr("integro: nenhuma escrita fora do lugar\n")
              : tr("CORROMPIDO - o endereco esta acima; o log do sistema guarda a linha\n"));
  }
  else if (eh(o, "tudo"))    { cmd_memoria(); printf("\n"); cmd_mem_extrato(); printf("\n"); cmd_mem_mapa(); }
  else if (eh(o, "estresse")) {
    tab5_heap_psram_estresse(argc >= 3 ? atoi(argv[2]) : 500, !(argc >= 4 && eh(argv[3], "linha")));
  }
  else                       { mostrar_uso("mem"); }
}

void c_energia(int argc, char** argv)
{
  if (argc < 2)                   { cmd_energia(); }
  else if (eh(argv[1], "travas"))  { cmd_travas(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (eh(argv[1], "quem")) {
    // QUEM SEGURA O QUE (kernel/recursos.h): o que cada servico declarou e o
    // que a suspensao preserva por causa disso.
    recurso_uso_t u[RECURSOS_MAX];
    const int n = sistema_recursos(u, RECURSOS_MAX);
    const uint32_t agora = tab5_agora_ms();
    printf(tr("%d recurso(s) em uso (com a tela apagada o clock vai de 40 a 180 MHz)\n"), n);
    for (int i = 0; i < n; ++i) {
      printf(tr("  %-16s %-20s ha %lu s\n"), sistema_recurso_nome(u[i].r), u[i].quem, (unsigned long)((agora - u[i].desde_ms) / 1000));
    }
  }
  else if (eh(argv[1], "repouso")) { cmd_repouso_forcar(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (eh(argv[1], "teto"))    { cmd_teto_cpu(argc >= 3 ? atoi(argv[2]) : 0); }
  else if (eh(argv[1], "sono")) {
    // O sono leve do P4 em repouso (contrato de economia, kernel/app.h).
    if (argc >= 3 && (eh(argv[2], "on") || eh(argv[2], "off"))) {
      ajustes_definir_sono_leve(eh(argv[2], "on"));
      printf(tr("sono leve em repouso: %s (vale no proximo repouso)\n"), ajustes_sono_leve() ? tr("ligado") : tr("desligado"));
    } else if (argc >= 3 && eh(argv[2], "segurar")) {
      const bool sim = !(argc >= 4 && eh(argv[3], "nao"));
      sistema_economia_segurar(ECONOMIA_SEGURA_TESTE, sim);
      printf(tr("teste %s o sono\n"), sim ? tr("segurando") : tr("soltou"));
    } else {
      static char r[256];
      recursos_resumo(r, sizeof(r));
      printf(tr("sono leve: ajuste %s, agora %s, seguram: 0x%02lx%s%s%s\nrecursos em uso: %s\n"),
             ajustes_sono_leve() ? tr("ligado") : tr("desligado"),
             tab5_power_sono_leve_ligado() ? tr("DORMINDO entre eventos") : tr("acordado"),
             (unsigned long)sistema_economia_seguros(),
             (sistema_economia_seguros() & ECONOMIA_SEGURA_WIFI) ? tr(" (Wi-Fi com receptor pedido)") : "",
             (sistema_economia_seguros() & ECONOMIA_SEGURA_RECURSOS) ? tr(" (recursos)") : "",
             (sistema_economia_seguros() & ECONOMIA_SEGURA_TESTE) ? tr(" (teste)") : "", r);
      printf("%s", tr("com o cabo USB ligado o console impede o sono - meca na bateria ('energia ensaio')\n"));
    }
  }
  else if (eh(argv[1], "ensaio")) {
    // O consumo de cada servico com a tela apagada - ver services/tab5_ensaio.hpp.
    if (argc >= 3 && eh(argv[2], "parar")) { tab5_ensaio_parar(); printf("%s", tr("parando - os ajustes voltam em 1 s\n")); }
    else if (argc >= 3 && eh(argv[2], "comecar")) {
      const int min = argc >= 4 ? atoi(argv[3]) : 6;
      int fases = argc >= 5 ? atoi(argv[4]) : 5;
      if (fases < 1 || fases > 5) { fases = 5; }
      if (tab5_ensaio_comecar(min, fases)) {
        printf(tr("ensaio pedido (%d min medindo por fase, %d fase(s)). Tire o cabo, deixe a tela\n"
               "apagar e nao mexa por ~%d min. Depois: 'energia ensaio'.\n"), min, fases, fases * (min + 2));
      } else { printf("%s", tr("ja ha um ensaio rodando ('energia ensaio parar')\n")); }
    }
    else { tab5_ensaio_imprimir(); }
  }
  else                             { mostrar_uso("energia"); }
}

// As partes de fundo dos apps (kernel/fundo.h). A parada pedida daqui
// acontece na interface (fundo_manter), como toda parada.
// MUSICA - o tocador do sistema (services/tab5_musica), pela serial.
void c_musica(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "lista")) {
    const char* rel = argc >= 3 ? argv[2] : "";
    tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * TAB5_MUSICA_LISTA_MAX, MALLOC_CAP_SPIRAM);
    if (!it) { printf("%s", tr("sem memoria\n")); return; }
    char pasta[160] = "";
    tab5_musica_pasta(pasta, sizeof(pasta));
    bool cheia = false;
    const int n = tab5_musica_listar(rel, it, TAB5_MUSICA_LISTA_MAX, &cheia);
    if (n < 0) { printf(tr("sem cartao, ou %s/%s nao existe\n"), pasta, rel); }
    for (int i = 0; i < n; ++i) {
      char u[TAB5_MUSICA_NOME_MAX * 2];
      tab5_musica_nome_utf8(it[i].nome, u, sizeof(u));
      printf("  %3d  %s%s\n", i, it[i].pasta ? tr("[pasta] ") : "", u);
    }
    if (n >= 0) { printf(tr("--- %s/%s: %d item(ns)%s\n"), pasta, rel, n, cheia ? tr(" (havia mais)") : ""); }
    heap_caps_free(it);
    return;
  }
  if (argc >= 3 && eh(argv[1], "tocar")) {
    // musica tocar <n> [subpasta] - o n e o da 'musica lista'.
    const char* rel = argc >= 4 ? argv[3] : "";
    tab5_musica_zerar_diag();
    printf(tab5_musica_tocar(rel, atoi(argv[2]), nullptr) ? tr("tocando\n") : tr("nao tocou (o item e musica?)\n"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "aleatorio")) {
    if (argc >= 3) { tab5_musica_definir_aleatorio(eh(argv[2], "on") || eh(argv[2], "sim") || eh(argv[2], "1")); }
    printf(tr("aleatorio: %s\n"), tab5_musica_aleatorio() ? tr("ligado") : tr("desligado"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "repetir")) {
    static const char* const R[] = { "nao", "tudo", "uma" };
    if (argc >= 3) {
      for (int i = 0; i < 3; ++i) { if (eh(argv[2], R[i])) { tab5_musica_definir_repetir((tab5_musica_repetir_t)i); } }
    }
    printf(tr("repetir: %s\n"), R[tab5_musica_repetir()]);
    return;
  }
  if (argc >= 2 && eh(argv[1], "playlists")) {
    tab5_musica_item_t* it = (tab5_musica_item_t*)heap_caps_malloc(sizeof(tab5_musica_item_t) * 64, MALLOC_CAP_SPIRAM);
    if (!it) { printf("%s", tr("sem memoria\n")); return; }
    const int n = tab5_musica_playlists(it, 64);
    if (n < 0) { printf("%s", tr("sem cartao\n")); }
    for (int i = 0; i < n; ++i) {
      char u[TAB5_MUSICA_NOME_MAX * 2];
      tab5_musica_nome_utf8(it[i].nome, u, sizeof(u));
      printf("  %s\n", u);
    }
    if (n == 0) { printf("%s", tr("nenhuma playlist (musica playlist <nome> criar)\n")); }
    heap_caps_free(it);
    return;
  }
  if (argc >= 3 && eh(argv[1], "playlist")) {
    // musica playlist <nome> [criar|apagar|tocar [n]|por <caminho>|tirar <caminho>]
    const char* nome = argv[2];
    const char* o = argc >= 4 ? argv[3] : "";
    if (eh(o, "criar"))  { char nc[TAB5_MUSICA_NOME_MAX]; printf(tab5_musica_playlist_criar(nome, nc, sizeof(nc)) ? tr("criada: %s\n") : tr("nao criou (ja existe?): %s\n"), nc); return; }
    if (eh(o, "apagar")) { printf(tab5_musica_playlist_apagar(nome) ? tr("apagada\n") : tr("nao existe\n")); return; }
    if (eh(o, "tocar"))  { printf(tab5_musica_tocar_playlist(nome, argc >= 5 ? atoi(argv[4]) : -1, nullptr) ? tr("tocando\n") : tr("nao tocou\n")); return; }
    if (eh(o, "por") && argc >= 5)   { printf(tab5_musica_playlist_por(nome, argv[4]) ? tr("posta\n") : tr("ja estava (ou erro)\n")); return; }
    if (eh(o, "tirar") && argc >= 5) { printf(tab5_musica_playlist_tirar(nome, argv[4]) ? tr("tirada\n") : tr("nao estava\n")); return; }
    tab5_musica_faixa_t* fx = (tab5_musica_faixa_t*)heap_caps_malloc(sizeof(tab5_musica_faixa_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM);
    if (!fx) { printf("%s", tr("sem memoria\n")); return; }
    const int n = tab5_musica_playlist_itens(nome, fx, TAB5_MUSICA_FILA_MAX);
    if (n < 0) { printf(tr("'%s' nao existe\n"), nome); }
    for (int i = 0; i < n; ++i) {
      char u[TAB5_MUSICA_CAMINHO_MAX * 2];
      tab5_musica_nome_utf8(fx[i].caminho, u, sizeof(u));
      const uint32_t ms = tab5_musica_duracao(fx[i].caminho);
      printf("  %3d  %2lu:%02lu  %s\n", i, (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60), u);
    }
    heap_caps_free(fx);
    return;
  }
  if (argc >= 4 && eh(argv[1], "capa") && eh(argv[2], "internet")) {
    ajustes_definir_capa_internet(eh(argv[3], "ligar"));
    printf(tr("capa pela internet: %s\n"), ajustes_capa_internet() ? tr("ligada") : tr("desligada (so arquivo e pasta)"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "capa")) {
    char arq[200];
    uint32_t v;
    static const char* const E[] = { "nenhuma", "buscando", "pronta" };
    const tab5_musica_capa_t c = tab5_musica_capa(arq, sizeof(arq), &v);
    printf(tr("capa: %s %s (versao %lu)\n"), E[c], arq, (unsigned long)v);
    return;
  }
  if (argc >= 3 && eh(argv[1], "duracao")) {
    const uint32_t ms = tab5_musica_duracao(argv[2]);
    printf(tr("%lu ms (%lu:%02lu)\n"), (unsigned long)ms, (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
    return;
  }
  if (argc >= 2 && eh(argv[1], "pausar"))  { tab5_musica_tocar_pausar(); return; }
  if (argc >= 2 && eh(argv[1], "proxima")) { tab5_musica_proxima(); return; }
  if (argc >= 2 && eh(argv[1], "anterior")) { tab5_musica_anterior(); return; }
  if (argc >= 2 && eh(argv[1], "parar"))   { tab5_musica_parar(); return; }
  tab5_musica_estado_t e;
  tab5_musica_estado(&e);
  printf(tr("%s | %s%s%s | %d/%d em '%s' | %lu/%lu s | %lu Hz %u canal(is) %u kbps%s%s\n"),
         e.ativo ? (e.tocando ? tr("tocando") : tr("pausado")) : tr("parado"), e.titulo,
         e.artista[0] ? " - " : "", e.artista, e.idx + 1, e.n, e.rel,
         (unsigned long)(e.pos_ms / 1000), (unsigned long)(e.dur_ms / 1000), (unsigned long)e.hz,
         (unsigned)e.canais, (unsigned)e.kbps, e.erro[0] ? " | " : "", e.erro);
  printf(tr("blocos %lu de %lu ms | fila secou %lu vez(es) | decodificar: media %lu us, pior %lu us\n"),
         (unsigned long)e.blocos, (unsigned long)e.bloco_ms, (unsigned long)e.secas,
         (unsigned long)e.dec_media_us, (unsigned long)e.dec_max_us);
  printf(tr("lendo o cartao: %lu ms no total, %lu us no pior bloco | espectro: pior %lu us por bloco\n"),
         (unsigned long)e.ler_soma_ms, (unsigned long)e.ler_no_pior_us, (unsigned long)e.esp_max_us);
}

// VIDEO - o tocador de video do sistema (services/tab5_video), pela serial:
// da para testar a decodificacao e o sincronismo sem olhar a tela.
void c_video(int argc, char** argv)
{
  if (argc >= 3 && eh(argv[1], "abrir")) {
    char caminho[TAB5_ARQ_CAMINHO_MAX];
    resolver(argv[2], caminho, sizeof(caminho));
    printf(tab5_video_abrir(caminho) ? tr("abrindo (veja 'video estado')\n") : tr("nao abriu (o caminho termina em .mp4 ou .avi?)\n"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "pausar")) { tab5_video_pausar_tocar(); printf("%s", tr("pedido\n")); return; }
  if (argc >= 3 && eh(argv[1], "ir")) { tab5_video_ir_ms((uint32_t)(atof(argv[2]) * 1000.0)); printf("%s", tr("pedido\n")); return; }
  if (argc >= 2 && eh(argv[1], "fechar")) { tab5_video_fechar(); printf("%s", tr("fechado\n")); return; }
  if (argc >= 3 && eh(argv[1], "h264")) {
    // BANCADA do decodificador H.264 (services/tab5_h264): sem tela, mede a
    // decodificacao de um .h264 cru e, com 'hash', imprime o FNV de cada
    // quadro para comparar com o teste do Mac (testes/rodar_h264.sh).
    char caminho[TAB5_ARQ_CAMINHO_MAX];
    resolver(argv[2], caminho, sizeof(caminho));
    tab5_h264_onde_t onde = TAB5_H264_AUTO;
    bool hash = false;
    for (int i = 3; i < argc; ++i) {
      if (eh(argv[i], "sram")) { onde = TAB5_H264_SRAM; }
      else if (eh(argv[i], "psram")) { onde = TAB5_H264_PSRAM; }
      else if (eh(argv[i], "hash")) { hash = true; }
      else if (eh(argv[i], "fundo")) {
        printf("%s", tab5_h264_bancada_fundo(caminho, onde) ? tr("medindo em segundo plano (resultado no log)\n") : tr("nao comecou\n"));
        return;
      }
    }
    tab5_h264_bancada_t r;
    tab5_h264_bancada(caminho, onde, hash, &r);
    if (r.erro[0]) { printf(tr("erro: %s\n"), r.erro); }
    printf(tr("%lu quadros %dx%d em %lu ms = %.1f quadros/s | %lu com erro | arena %lu KB na %s | SRAM livre %lu KB (minimo %lu KB) | CPU %lu MHz\n"),
           (unsigned long)r.quadros, r.l, r.a, (unsigned long)r.ms,
           r.ms ? (double)r.quadros * 1000.0 / (double)r.ms : 0.0, (unsigned long)r.com_erro,
           (unsigned long)r.arena_kb, r.na_sram ? "SRAM" : "PSRAM",
           (unsigned long)r.interna_livre_kb, (unsigned long)r.interna_min_kb, (unsigned long)r.cpu_mhz);
    return;
  }
  if (argc >= 2) { mostrar_uso("video"); return; }
  tab5_video_estado_t e;
  tab5_video_estado(&e);
  if (!e.aberto) { printf("%s", tr("nenhum video aberto (video abrir <caminho>)\n")); return; }
  printf(tr("%s: %s%s%s | %lu / %lu s | %dx%d | com som: %s\n"), e.nome,
         e.erro ? tr("erro") : (e.fim ? tr("fim") : (e.tocando ? tr("tocando") : tr("pausado"))),
         e.erro ? ": " : "", e.erro ? e.erro_txt : "",
         (unsigned long)(e.pos_ms / 1000), (unsigned long)(e.dur_ms / 1000), e.l, e.a,
         e.com_som ? tr("sim") : tr("nao"));
  printf(tr("%.1f quadros/s | %lu mostrados de %lu | %lu pulados (o relogio ja tinha passado)\n"),
         (double)e.fps, (unsigned long)e.mostrados, (unsigned long)e.quadros, (unsigned long)e.perdidos);
}

void c_fundo(int argc, char** argv)
{
  if (argc >= 3 && eh(argv[1], "parar")) {
    printf(sistema_fundo_parar_nome(argv[2]) ? tr("pedido de parada enviado para '%s'\n")
                                             : tr("'%s' nao esta rodando\n"), argv[2]);
    return;
  }
  if (argc >= 2) { mostrar_uso("fundo"); return; }
  fundo_info_t l[FUNDO_MAX];
  const int n = sistema_fundo_listar(l, FUNDO_MAX);
  if (n == 0) { printf("%s", tr("nenhuma parte de fundo rodando\n")); }
  for (int i = 0; i < n; ++i) {
    printf(tr("  %-12s ha %5lus  %s\n"), l[i].nome, (unsigned long)l[i].ha_s, l[i].resumo);
  }
}

// Devolve (ou tira) o barulho de uma etiqueta em tempo de execucao. O sistema
// silencia "eh_sdio" e "sdmmc_io" no boot - eles inundam o console quando o
// enlace com o C6 morre. Silenciar sem deixar como reverter seria esconder
// informacao; com isto, o cru esta a um comando de distancia.
static const char* const NIVEL_NOME[6] = { "nada", "erro", "aviso", "info", "debug", "verboso" };

static int nivel_log(const char* s)
{
  for (int i = 0; i < 6; ++i) { if (eh(s, NIVEL_NOME[i])) { return i; } }
  char* fim = nullptr;
  const long n = strtol(s, &fim, 10);
  return (*s && fim && *fim == 0 && n >= 0 && n <= 5) ? (int)n : -1;
}

void c_log(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("log"); return; }
  if (argc < 3) {
    const int n = (int)esp_log_level_get(argv[1]);
    printf(tr("'%s' esta em %s (%d)\n"), argv[1], (n >= 0 && n <= 5) ? NIVEL_NOME[n] : "?", n);
    return;
  }
  const int n = nivel_log(argv[2]);
  if (n < 0) {
    printf(tr("nivel '%s' nao existe - use nada|erro|aviso|info|debug|verboso (ou 0-5)\n"), argv[2]);
    return;
  }
  esp_log_level_set(argv[1], (esp_log_level_t)n);
  printf(tr("'%s' agora em %s (%d)\n"), argv[1], NIVEL_NOME[n], n);
}

// 'boasvindas': abre o assistente de primeiro uso agora (kernel/boas_vindas).
// Para testar num aparelho que ja foi configurado - ele so abre sozinho num
// aparelho novo. Concluir ou fechar marca como feito de novo.
void c_boasvindas(int, char**)
{
  extern const app_t APP_BOAS_VINDAS;
  ajustes_definir_config_inicial(false);
  sistema_simular_abrir(&APP_BOAS_VINDAS);
  printf("%s", tr("abrindo o assistente de boas-vindas\n"));
}

// fuso: the time-zone region (with daylight saving), the manual offset and
// the 12/24-hour clock - the same as Settings > Date and time.
void c_fuso(int argc, char** argv)
{
  if (argc >= 2 && (eh(argv[1], "12h") || eh(argv[1], "24h"))) {
    ajustes_definir_relogio_12h(eh(argv[1], "12h"));
  } else if (argc >= 2 && eh(argv[1], "lista")) {
    for (int i = 0; i < tab5_fuso_n(); ++i) {
      const int m = tab5_fuso_padrao_min(i);
      printf("  %2d  %s%s  (UTC%c%d:%02d%s)\n", i, i == ajustes_fuso_zona() ? "* " : "", tab5_fuso_nome(i),
             m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60, tab5_fuso_tem_verao(i) ? tr(", com horário de verão") : "");
    }
    return;
  } else if (argc >= 3 && eh(argv[1], "manual")) {
    ajustes_definir_fuso_zona(TAB5_FUSO_MANUAL);
    ajustes_definir_fuso_min((int16_t)atoi(argv[2]));
  } else if (argc >= 2) {
    char* fim = nullptr;
    const long z = strtol(argv[1], &fim, 10);
    if (!fim || *fim || z < 0 || z >= tab5_fuso_n()) { printf("%s", tr("região inválida - 'fuso lista' mostra as regiões\n")); return; }
    ajustes_definir_fuso_zona((uint8_t)z);
    if (z != TAB5_FUSO_MANUAL) { ajustes_definir_fuso_min((int16_t)tab5_fuso_padrao_min((int)z)); }
  }
  char f[40], hm[8] = "--:--";
  const uint32_t e = tab5_hora_epoch();
  tab5_fuso_texto(e ? e : (uint32_t)time(nullptr), f, sizeof(f));
  if (e) { struct tm t; tab5_hora_local(e, &t); tab5_hora_hhmm(&t, hm, sizeof(hm)); }
  printf(tr("região: %s\nagora: %s  %s  (relógio de %s)\n"), tab5_fuso_nome(ajustes_fuso_zona()), hm, f,
         ajustes_relogio_12h() ? "12 h" : "24 h");
}

#if M5_COM_BERRY
// ---------------------------------------------------------------------------
// BERRY - o interpretador de script. A maquina do console e uma so, criada no
// primeiro uso e mantida entre comandos: o que uma linha define, a proxima
// enxerga (como um REPL). Roda na tarefa do Berry (services/berry_vm), com o
// mesmo prazo de um app: um laco infinito vira erro, nao trava o console.
// ---------------------------------------------------------------------------
#include "berry_vm.hpp"
#include "berry_apps.hpp"
#include "esp_heap_caps.h"
#include <atomic>

static bvm* s_berry = nullptr;
// 'saida': o stdout de quem pediu - o Berry roda na tarefa dele, e o print
// tem que cair onde o comando foi digitado (a serial ou o Terminal).
typedef struct { const char* codigo; bool arquivo; bool apagar; FILE* saida; } berry_cmd_t;

static void berry_rodar_console(const berry_cmd_t* c);
static void berry_trabalho_console(void* ctx)
{
  const berry_cmd_t* c = (const berry_cmd_t*)ctx;
  FILE* const antes = stdout;
  stdout = c->saida;
  berry_rodar_console(c);
  fflush(stdout);
  stdout = antes;
}

static void berry_rodar_console(const berry_cmd_t* c)
{
  if (c->apagar) {
    if (s_berry) { be_vm_delete(s_berry); s_berry = nullptr; }
    return;
  }
  if (!s_berry) {
    s_berry = berry_vm_nova();
    if (!s_berry) { printf("%s", tr("sem memoria para o Berry (orcamento cheio - ver 'berry mem')\n")); return; }
    printf(tr("Berry %s pronto (modulos: tela, sistema)\n"), BERRY_VERSION);
  }
  if (c->arquivo && !tab5_sd_montar()) { printf("%s", tr("cartao nao montou\n")); return; }
  berry_executar(s_berry, c->codigo, c->arquivo, !c->arquivo, nullptr, 0);
  if (c->arquivo) { tab5_sd_desmontar(); }
}

void c_berry(int argc, char** argv)
{
  if (argc < 2) { mostrar_uso("berry"); return; }
  berry_cmd_t c = { "", false, false, stdout };
  char buf[192] = "";
  if (eh(argv[1], "tempos")) {
    berry_apps_tempos(argc >= 3 && eh(argv[2], "zerar"));
    return;
  }
  if (eh(argv[1], "mem")) {
    berry_apps_mem(s_berry);
    return;
  }
  if (eh(argv[1], "reler")) {
    berry_apps_reler();
    printf("%s", tr("pasta relida - os icones mudam na tela inicial\n"));
    return;
  }
  if (eh(argv[1], "verificar") && argc >= 3) {
    // Compila o arquivo sem rodar (o mesmo 'Verificar' do Editor).
    char tmp[sizeof(buf)];
    const char* caminho = caminho_sd(argv[2], tmp, sizeof(tmp));
    com_cartao([&]() -> int {
      FILE* f = fopen(caminho, "r");
      if (!f) { printf(tr("nao abriu %s\n"), caminho); return 1; }
      // O arquivo INTEIRO: com um buffer fixo de 16 KB, um app maior era
      // cortado no meio e "dava erro" de sintaxe na linha do corte (o
      // Mostruario, 18 KB, aparecia quebrado na linha 445 sem estar).
      const long tam = tab5_sd_tamanho(f);
      if (tam < 0 || tam > 512 * 1024) {
        fclose(f);
        printf(tr("%s: tamanho %ld fora do limite (512 KB)\n"), caminho, tam);
        return 1;
      }
      char* t = (char*)heap_caps_malloc((size_t)tam + 1, MALLOC_CAP_SPIRAM);
      if (!t) { fclose(f); printf("%s", tr("sem memoria\n")); return 1; }
      const size_t n = fread(t, 1, (size_t)tam, f);
      t[n] = 0;
      fclose(f);
      char erro[160];
      int linha = 0;
      if (berry_verificar(t, n, erro, sizeof(erro), &linha)) { printf(tr("%s: sem erros de sintaxe\n"), caminho); }
      else if (linha > 0) { printf(tr("%s, linha %d: %s\n"), caminho, linha, erro); }
      else                { printf("%s: %s\n", caminho, erro); }
      heap_caps_free(t);
      return 0;
    });
    return;
  }
  if (eh(argv[1], "reiniciar")) {
    c.apagar = true;
  } else if (eh(argv[1], "rodar") && argc >= 3) {
    char tmp[sizeof(buf)];
    strlcpy(buf, caminho_sd(argv[2], tmp, sizeof(tmp)), sizeof(buf));
    c.arquivo = true;
  } else {
    // A linha inteira depois de 'berry' (o executar() quebrou em palavras).
    for (int i = 1; i < argc; ++i) {
      if (i > 1) { strlcat(buf, " ", sizeof(buf)); }
      strlcat(buf, argv[i], sizeof(buf));
    }
  }
  c.codigo = buf;
  const int64_t t0 = esp_timer_get_time();
  if (!berry_rodar(berry_trabalho_console, &c, 10000)) { printf("%s", tr("a tarefa do Berry nao subiu\n")); return; }
  if (c.apagar) { printf("%s", tr("maquina do Berry apagada\n")); return; }
  printf(tr("(%.1f ms, pilha livre %u B, %u KB livres na PSRAM)\n"),
         (esp_timer_get_time() - t0) / 1000.0, (unsigned)berry_pilha_livre(),
         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
#endif

// BANCADA DO PENDRIVE: ligar e devolver sem tocar na tela. Ligar tira o
// console do USB-C (o computador passa a ver o disco, nao a porta serial); ele
// volta quando o computador ejeta. O que aconteceu fica no log do cartao.
void c_pendrive(int argc, char** argv)
{
  if (argc >= 2 && eh(argv[1], "ligar")) {
    printf(tab5_pendrive_iniciar() ? tr("ligando - o console sai do USB-C ate o computador ejetar\n")
                                   : tr("nao ligou (ja havia sessao, ou recusado - veja 'pendrive')\n"));
    return;
  }
  if (argc >= 2 && eh(argv[1], "devolver")) { tab5_pendrive_devolver(); printf("%s", tr("devolvendo\n")); return; }
  tab5_pendrive_estado_t e;
  tab5_pendrive_estado(&e);
  static const char* const FASE[] = { "parado", "preparando", "ativo", "devolvendo" };
  printf(tr("fase %s, motivo %d (%s), ultima por ejecao %s, sem ejetar %s, oferta %s, pc %s\n"),
         FASE[e.fase & 3], (int)e.motivo, tab5_pendrive_motivo_texto(e.motivo), e.por_ejecao ? tr("sim") : tr("nao"), e.sem_ejetar ? tr("sim") : tr("nao"),
         e.oferta ? tr("sim") : tr("nao"), tab5_pendrive_pc_presente() ? tr("sim") : tr("nao"));
}

// O USB-A (services/tab5_usb): o que esta plugado, a velocidade que ele
// negociou e quanto o pendrive rende lendo.
void c_usb(int argc, char** argv)
{
  if (argc >= 2 && (eh(argv[1], "ligar") || eh(argv[1], "desligar"))) {
    tab5_usb_ligar(eh(argv[1], "ligar"));
    printf(tr("USB-A %s (ajuste gravado)\n"), eh(argv[1], "ligar") ? tr("ligado") : tr("desligado - os 5V do conector cortados"));
    return;
  }
  if (argc >= 3 && eh(argv[1], "ler")) {
    // Le o arquivo inteiro em pedacos de 32 KB (o mesmo do tocador) e mede.
    char c[300];
    snprintf(c, sizeof(c), argv[2][0] == '/' ? "%s" : TAB5_USB_RAIZ "/%s", argv[2]);
    if (!tab5_volume_montar(c)) { printf(tr("sem o disco de %s\n"), c); return; }
    uint8_t* b = (uint8_t*)heap_caps_aligned_alloc(128, 32 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    const int fd = b ? open(c, O_RDONLY) : -1;
    if (fd < 0) { printf(tr("nao abriu %s\n"), c); }
    else {
      const int64_t t0 = esp_timer_get_time();
      uint64_t tot = 0;
      ssize_t r;
      while ((r = read(fd, b, 32 * 1024)) > 0) { tot += (uint64_t)r; }
      const int64_t us = esp_timer_get_time() - t0;
      close(fd);
      printf(tr("%llu bytes em %lld ms = %.0f KB/s\n"), (unsigned long long)tot, us / 1000,
             us > 0 ? tot * 1000000.0 / us / 1024.0 : 0.0);
    }
    heap_caps_free(b);
    tab5_volume_desmontar(c);
    return;
  }
  if (argc >= 2 && eh(argv[1], "ejetar")) {
    switch (tab5_volume_ejetar(TAB5_VOL_PENDRIVE_RAIZ, 3000)) {
      case TAB5_VOL_EJETADO:       printf("%s", tr("ejetado: pode tirar o pendrive\n")); break;
      case TAB5_VOL_EJETAR_EM_USO: printf("%s", tr("em uso (copia ou musica): continua montado\n")); break;
      default:                     printf("%s", tr("sem pendrive montado\n")); break;
    }
    return;
  }
  if (argc >= 2 && eh(argv[1], "espaco")) {
    uint64_t t = 0, l = 0;
    if (tab5_usb_espaco(&t, &l)) { printf(tr("%.1f GB, livres %.1f GB\n"), t / 1e9, l / 1e9); }
    else                         { printf("%s", tr("sem pendrive montado\n")); }
    return;
  }
  tab5_usb_estado_t e;
  tab5_usb_estado(&e);
  static const char* const VEL[] = { "baixa (1,5 Mb/s)", "cheia (12 Mb/s)", "ALTA (480 Mb/s)" };
  char nome[40];
  tab5_usb_nome(nome, sizeof(nome));
  printf(tr("USB-A: %s (ajuste %s), %u aparelho(s) visto(s) desde o boot\n"),
         e.ligado ? tr("pronto") : tr("desligado"), ajustes_usb_a() ? tr("ligado") : tr("desligado"), (unsigned)e.conexoes);
  if (e.aparelho) {
    printf(tr("plugado: %04x:%04x '%s', classe %u, velocidade %s\n"), e.vid, e.pid, e.produto, e.classe,
           e.velocidade <= 2 ? VEL[e.velocidade] : "?");
  } else {
    printf("%s", tr("nada plugado\n"));
  }
  if (e.disco) {
    printf("disco: %lu setores de %lu bytes (%.1f GB), %s em " TAB5_USB_RAIZ " '%s', %d segurando\n",
           (unsigned long)e.setores, (unsigned long)e.setor_bytes, (double)e.setores * e.setor_bytes / 1e9,
           e.ejetado ? tr("EJETADO (pode tirar)") : e.montado ? tr("montado") : tr("NAO montado"), nome, e.refs);
  }
  if (e.erro[0]) { printf(tr("ultimo problema: %s\n"), e.erro); }
}
