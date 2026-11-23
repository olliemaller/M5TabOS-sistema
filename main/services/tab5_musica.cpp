// TOCADOR DE MUSICA - o centro do servico: estado e fila, os controles, o laco
// de reproducao, a parte de fundo e a troca da fila. O resto mora em
// tab5_musica_*.cpp (ver tab5_musica_interno.hpp).
#include "tab5_musica_interno.hpp"
#include "idioma.h"
#include "app.h"
#include "midia.h"
#include "ajustes.h"
#include "tab5_sd.hpp"
#include "tab5_usb.hpp"
#include "tab5_volumes.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <atomic>
#include <cstdio>
#include <cstring>

#include "minimp3_externo.h"

static const char* TAG = "musica";

// A parte de fundo que o servico usa (definida no fim do arquivo).
extern const fundo_def_t g_fundo_musica;
// Quem pediu a fila (o titulo no miniplayer abre este app). Copia: o nome de
// um app Berry pode sumir quando os scripts sao relidos.
static char s_app_dono[32] = "Música";

// ---------------------------------------------------------------------------
// MEMORIA - toda na PSRAM, alocada no ao_iniciar e solta no ao_parar.
// ---------------------------------------------------------------------------
// TRES blocos em rodizio: o alto-falante guarda dois (o que toca e o
// proximo), o terceiro e o que esta sendo decodificado. 16 quadros de MP3
// (18432 amostras por canal) = 418 ms a 44,1 kHz: com a fila cheia, ~0,6 s
// de folga para o laco - leitura do cartao parada atras de uma gravacao de
// outro servico (ja medido: picos de centenas de ms) nao vira buraco no som.
// (BLOCO_QUADROS fica em tab5_musica_interno.hpp.)
#define BLOCOS         3

// LEITURA DO CARTAO - medido no aparelho: lendo direto para a PSRAM, num
// endereco qualquer, 16 KB levavam ~250 ms (60 KB/s) e o som engasgava a
// cada bloco. O driver do SD so faz DMA de varios setores de uma vez quando o
// destino esta ALINHADO (e a posicao no arquivo comeca num setor); senao cai
// num setor de 512 bytes por vez, copiando. Por isso: um buffer de leitura
// alinhado para DMA, lido sempre em pedacos inteiros a partir de inicio de
// setor, e copiado (memcpy, barato) para o buffer do decodificador.
// (LER_PEDACO, ARQ_BUF e SETOR tambem ficam em tab5_musica_interno.hpp.)

static int16_t*  s_bloco[BLOCOS] = { };
// A fila: os caminhos (relativos a pasta de musica), copiados de quem pediu
// (a janela pode fechar). Uma pasta ou uma playlist - numa playlist cada
// faixa pode morar numa pasta diferente.
//
// A ORDEM em que toca e g_musica_ordem (indices em g_musica_fila): a da pasta, ou
// embaralhada (aleatorio). A posicao do laco e na ORDEM - a proxima e
// sempre pos + 1, com ou sem aleatorio.
caminho_t* g_musica_fila = nullptr;
int16_t*   g_musica_ordem = nullptr;
int        g_musica_fila_n = 0;
char       g_musica_fila_raiz[160] = "";   // a pasta de musica, caminho absoluto
// A fila trocou (musica_fila_tocar): o laco confere de novo quais discos segurar.
static std::atomic<uint32_t> s_fila_gen{0};

// ---------------------------------------------------------------------------
// ESTADO - escrito pelo laco, lido pela janela (copia, sob a trava).
// ---------------------------------------------------------------------------
Trava g_musica_trava;   // fila + estado
tab5_musica_estado_t   g_musica_est = { };
static uint64_t s_dec_soma_us = 0;   // diagnostico (tab5_musica_estado_t)

// Pedidos da janela e do miniplayer: o laco atende entre dois blocos.
static std::atomic<int>     s_ped_abrir{-1};     // indice na fila; -1 = nenhum
static std::atomic<bool>    s_ped_alternar{false};
static std::atomic<int>     s_ped_pular{0};      // +1 proxima, -1 anterior
static std::atomic<int64_t> s_ped_ir{-1};        // ms; -1 = nenhum
static std::atomic<bool>    s_ped_ordem{false};  // o aleatorio mudou: refazer a ordem

// ---------------------------------------------------------------------------
// OS CONTROLES - da janela e do miniplayer (tarefa de interface): so pedem.
// ---------------------------------------------------------------------------
void tab5_musica_tocar_pausar(void) { s_ped_alternar = true; }
void tab5_musica_proxima(void)      { s_ped_pular = 1; }
void tab5_musica_anterior(void)     { s_ped_pular = -1; }
void tab5_musica_ir_para(uint32_t ms) { s_ped_ir = (int64_t)ms; }
// Sem esperar (a interface, o Berry e o console chamam): o kernel para no laco
// dele, e o servico sai em milissegundos.
void tab5_musica_parar(void)        { sistema_fundo_parar_nome(g_fundo_musica.nome); }

bool tab5_musica_aleatorio(void) { return ajustes_modo_tocador() & 1; }
tab5_musica_repetir_t tab5_musica_repetir(void)
{
  const int r = (ajustes_modo_tocador() >> 1) & 3;
  return r <= TAB5_MUSICA_REPETIR_UMA ? (tab5_musica_repetir_t)r : TAB5_MUSICA_REPETIR_NAO;
}

void tab5_musica_definir_aleatorio(bool sim)
{
  if (sim == tab5_musica_aleatorio()) { return; }
  ajustes_definir_modo_tocador((uint8_t)((ajustes_modo_tocador() & ~1u) | (sim ? 1u : 0u)));
  s_ped_ordem = true;   // o laco refaz a ordem entre dois blocos, sem cortar o som
  g_musica_trava.travar();
  g_musica_est.aleatorio = sim;
  g_musica_trava.destravar();
}

void tab5_musica_definir_repetir(tab5_musica_repetir_t r)
{
  ajustes_definir_modo_tocador((uint8_t)((ajustes_modo_tocador() & ~6u) | ((unsigned)r << 1)));
  g_musica_trava.travar();
  g_musica_est.repetir = (uint8_t)r;
  g_musica_trava.destravar();
}

// Um numero aleatorio de verdade (o RNG do chip; o Wi-Fi nem precisa estar
// ligado para ele servir a um embaralhamento).
static uint32_t sorteio(uint32_t n) { return n ? esp_random() % n : 0; }

// A ORDEM DA FILA, sob a trava. 'fica' = indice em g_musica_fila que tem que ficar na
// posicao 'pos_fica' (a musica que esta tocando nao muda de lugar no ouvido):
// com aleatorio, ela vai para o comeco e o resto e embaralhado depois dela;
// sem, a ordem e a da pasta e a posicao dela e a natural. Devolve a posicao
// da 'fica' na ordem nova (-1 = sem 'fica').
static int montar_ordem(int fica, bool aleatorio)
{
  if (!g_musica_ordem) { return -1; }
  for (int i = 0; i < g_musica_fila_n; ++i) { g_musica_ordem[i] = (int16_t)i; }
  if (!aleatorio) { return fica; }
  // Fisher-Yates
  for (int i = g_musica_fila_n - 1; i > 0; --i) {
    const int j = (int)sorteio((uint32_t)i + 1);
    const int16_t t = g_musica_ordem[i]; g_musica_ordem[i] = g_musica_ordem[j]; g_musica_ordem[j] = t;
  }
  if (fica < 0) { return -1; }
  for (int i = 0; i < g_musica_fila_n; ++i) {
    if (g_musica_ordem[i] == fica) { g_musica_ordem[i] = g_musica_ordem[0]; g_musica_ordem[0] = (int16_t)fica; break; }
  }
  return 0;
}

static const midia_controles_t CONTROLES = {
  .tocar_pausar = tab5_musica_tocar_pausar,
  .proxima      = tab5_musica_proxima,
  .anterior     = tab5_musica_anterior,
  .ir_para      = tab5_musica_ir_para,
};

void tab5_musica_zerar_diag(void)
{
  g_musica_trava.travar();
  g_musica_est.blocos = g_musica_est.secas = g_musica_est.dec_max_us = g_musica_est.dec_media_us = 0;
  g_musica_est.ler_no_pior_us = g_musica_est.ler_soma_ms = g_musica_est.esp_max_us = 0;
  s_dec_soma_us = 0;
  g_musica_trava.destravar();
}

void tab5_musica_estado(tab5_musica_estado_t* out)
{
  g_musica_trava.travar();
  *out = g_musica_est;
  g_musica_trava.destravar();
  out->ativo = sistema_fundo_ativo(&g_fundo_musica);
  out->aleatorio = tab5_musica_aleatorio();
  out->repetir = (uint8_t)tab5_musica_repetir();
}

// ONDE O SOM ESTA DE VERDADE: g_musica_quadros e o que ja foi ENFILEIRADO, ate dois
// blocos (~0,8 s) a frente do que sai no alto-falante. Para mostrar e para
// pausar, desconta a fila (o bloco tocando conta pela metade).
static uint64_t quadros_tocando(void)
{
  const int f = sistema_som_fluxo_na_fila();
  const uint64_t atras = f > 0 ? (uint64_t)(f - 1) * g_musica_ult_q + g_musica_ult_q / 2 : 0;
  return g_musica_quadros > atras ? g_musica_quadros - atras : 0;
}

// O que a barra, o miniplayer e a pastilha da parte de fundo mostram.
static void publicar(fundo_t* f, bool tocando, bool mudou_faixa)
{
  char titulo[64 + 48 + 4];
  uint32_t pos, dur;
  g_musica_trava.travar();
  g_musica_est.tocando = tocando;
  g_musica_est.pos_ms = g_musica_hz ? (uint32_t)(quadros_tocando() * 1000u / g_musica_hz) : 0;
  if (g_musica_fmt == FMT_MP3 && g_musica_mp3_quadros > 20 && g_musica_hz) {
    g_musica_est.kbps = (uint16_t)(g_musica_mp3_kbps_soma / g_musica_mp3_quadros);
    // VBR sem Xing: a duracao melhora com a media de bitrate de verdade.
    if (!g_musica_dur_exata && g_musica_est.kbps) {
      g_musica_total_quadros = (uint64_t)(g_musica_dados_fim - g_musica_dados_ini) * 8u * g_musica_hz / ((uint64_t)g_musica_est.kbps * 1000u);
      g_musica_est.dur_ms = (uint32_t)(g_musica_total_quadros * 1000u / g_musica_hz);
    }
  }
  if (g_musica_est.artista[0]) { snprintf(titulo, sizeof(titulo), "%s - %s", g_musica_est.titulo, g_musica_est.artista); }
  else                  { snprintf(titulo, sizeof(titulo), "%s", g_musica_est.titulo); }
  pos = g_musica_est.pos_ms;
  dur = g_musica_est.dur_ms;
  g_musica_trava.destravar();
  midia_publicar(titulo, tocando, pos, dur);
  if (mudou_faixa) {
    char r[NOTIF_RESUMO];
    snprintf(r, sizeof(r), "%s", titulo);
    fundo_estado(f, 1, r);
  }
}

void tab5_musica_definir_cor(uint16_t cor565)
{
  ajustes_definir_cor_tocador(cor565);
  midia_definir_cor(cor565);
}

static void calar_fila(void)
{
  sistema_som_fluxo_calar();
  musica_espectro_esquecer();
}

// ---------------------------------------------------------------------------
// OS DISCOS QUE A FILA USA - o laco segura cada um enquanto a fila tiver
// musica nele, e so ele: tocando do pendrive, o cartao fica livre (pode ir
// para o computador sem parar o som), e vice-versa.
// ---------------------------------------------------------------------------
typedef struct { bool cartao, pendrive; } discos_t;

static discos_t discos_da_fila(void)
{
  discos_t d = { false, false };
  g_musica_trava.travar();
  for (int i = 0; i < g_musica_fila_n && !(d.cartao && d.pendrive); ++i) {
    const tab5_vol_id_t v = absoluto(g_musica_fila[i]) ? tab5_volume_de(g_musica_fila[i]) : TAB5_VOL_CARTAO;
    if (v == TAB5_VOL_CARTAO)   { d.cartao = true; }
    if (v == TAB5_VOL_PENDRIVE) { d.pendrive = true; }
  }
  g_musica_trava.destravar();
  return d;
}

// Segura o que a fila precisa e solta o que ela deixou de precisar. 'seg' =
// o que o laco segura agora.
static void segurar_discos(discos_t* seg)
{
  const discos_t q = discos_da_fila();
  if (q.cartao && !seg->cartao)        { seg->cartao = tab5_sd_montar(); }
  else if (!q.cartao && seg->cartao)   { tab5_sd_desmontar(); seg->cartao = false; }
  if (q.pendrive && !seg->pendrive)      { seg->pendrive = tab5_usb_montar(); }
  else if (!q.pendrive && seg->pendrive) { tab5_usb_desmontar(); seg->pendrive = false; }
}

static void soltar_discos(discos_t* seg)
{
  if (seg->cartao)   { tab5_sd_desmontar(); seg->cartao = false; }
  if (seg->pendrive) { tab5_usb_desmontar(); seg->pendrive = false; }
}

static void erro_laco(const char* t)
{
  g_musica_trava.travar();
  snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "%s", t);
  g_musica_trava.destravar();
}

// ---------------------------------------------------------------------------
// O LACO
// ---------------------------------------------------------------------------
static void laco_musica(fundo_t* f)
{
  discos_t seg = { false, false };
  uint32_t gen = s_fila_gen.load();
  segurar_discos(&seg);
  if (!seg.cartao && !seg.pendrive) {
    erro_laco(discos_da_fila().pendrive ? tr("o pendrive não está conectado") : tr("sem cartão SD"));
    return;
  }
  midia_assumir(g_fundo_musica.nome, s_app_dono, &CONTROLES);
  midia_definir_cor(ajustes_cor_tocador());
  bool tocando = false, som = false;
  bool acabou = false;   // a musica terminou sozinha (a proxima entra sem cortar a cauda)
  int vez = 0;
  int idx = -1;
  bool recomeco = true;   // o proximo bloco recomeca a fila (fila vazia e normal)
  int64_t pausado_desde = 0;   // 0 = tocando

  while (fundo_rodando(f)) {
    // A FILA TROCOU: talvez outro disco (do cartao para o pendrive...).
    if (s_fila_gen.load() != gen) { gen = s_fila_gen.load(); segurar_discos(&seg); }
    // O CARTAO VAI PARA O COMPUTADOR (modo pendrive): solta tudo agora - quem
    // empresta espera no maximo 3 s pelos donos do cartao. Tocando so do
    // pendrive do USB-A, o cartao nem esta seguro: o som continua.
    if (seg.cartao && tab5_sd_emprestado()) { ESP_LOGI(TAG, "cartao emprestado: parando"); break; }
    // O PENDRIVE DO USB-A FOI ARRANCADO: quem o tira espera no maximo 3 s.
    if (seg.pendrive && !tab5_usb_presente()) {
      ESP_LOGI(TAG, "o pendrive saiu: parando");
      erro_laco("o pendrive foi retirado");
      break;
    }

    // ---- pedidos ----
    const int abrir = s_ped_abrir.exchange(-1);
    int pular = s_ped_pular.exchange(0);
    // Fim natural: o que ja esta na fila do alto-falante e a cauda desta
    // musica - toca ate o fim, e a proxima emenda sem silencio.
    const bool natural = acabou && !pular && abrir < 0;
    if (natural) { pular = 1; }
    acabou = false;
    if (abrir >= 0 || (pular && !natural) || s_ped_ir.load() >= 0) { recomeco = true; }
    if (abrir >= 0) {
      pular = 0;
      if (som) { calar_fila(); }
      idx = abrir;
      tocando = musica_abrir_item(idx);
      if (!tocando) { pular = 1; }   // arquivo ruim: segue a fila
      s_ped_ir = -1;
      publicar(f, tocando, true);
    }
    if (s_ped_ordem.exchange(false)) {
      // O aleatorio mudou: a musica de agora fica, o resto se reorganiza.
      g_musica_trava.travar();
      const int atual = (idx >= 0 && idx < g_musica_fila_n && g_musica_ordem) ? g_musica_ordem[idx] : -1;
      const int nova = montar_ordem(atual, tab5_musica_aleatorio());
      if (nova >= 0) { idx = nova; g_musica_est.idx = nova; }
      g_musica_trava.destravar();
      publicar(f, tocando, false);
    }
    if (s_ped_alternar.exchange(false) && musica_arquivo_aberto()) {
      tocando = !tocando;
      if (!tocando && som) {
        // Pausa no ponto que se OUVIU: o que estava na fila e descartado, e
        // a leitura volta para la (senao a retomada pularia ~0,8 s).
        const uint64_t ouvido = quadros_tocando();
        sistema_som_fluxo_fechar();
        musica_espectro_esquecer();
        som = false;
        if (g_musica_hz) { musica_ir_para_ms((uint32_t)(ouvido * 1000u / g_musica_hz)); }
      }
      publicar(f, tocando, false);
    }
    if (pular) {
      // "Anterior" depois de 3 s volta ao comeco da mesma, como todo tocador.
      if (pular < 0 && g_musica_hz && g_musica_quadros > (uint64_t)g_musica_hz * 3) {
        musica_ir_para_ms(0);
        if (som) { calar_fila(); }
      } else {
        int n;
        g_musica_trava.travar(); n = g_musica_fila_n; g_musica_trava.destravar();
        const tab5_musica_repetir_t rep = tab5_musica_repetir();
        // Repetir uma: no fim natural, a mesma de novo (sem silencio). Os
        // botoes andam na fila do mesmo jeito.
        int novo = (natural && rep == TAB5_MUSICA_REPETIR_UMA) ? idx : idx + pular;
        const int passo = pular > 0 ? 1 : -1;
        bool achou = false;
        // Pula arquivos que nao abrem, ate dar uma volta.
        if (som && !natural) { calar_fila(); }
        for (int k = 0; k < n; ++k, novo += passo) {
          if (novo < 0 || novo >= n) {
            // Passou da ponta: so segue com repetir tudo.
            if (rep != TAB5_MUSICA_REPETIR_TUDO) { break; }
            if (novo >= n) {
              novo = 0;
              if (tab5_musica_aleatorio() && n > 1) {
                // Embaralha de novo, sem repetir logo de cara a que acabou.
                g_musica_trava.travar();
                const int ultima = (idx >= 0 && idx < n) ? g_musica_ordem[idx] : -1;
                montar_ordem(-1, true);
                if (g_musica_ordem[0] == ultima) { g_musica_ordem[0] = g_musica_ordem[1]; g_musica_ordem[1] = (int16_t)ultima; }
                g_musica_trava.destravar();
              }
            } else {
              novo = n - 1;
            }
          }
          if (musica_abrir_item(novo)) { achou = true; break; }
        }
        if (achou) { idx = novo; tocando = true; }
        else if (pular > 0) { ESP_LOGI(TAG, "fim da fila"); break; }   // acabou: desliga
        else { musica_ir_para_ms(0); }
      }
      publicar(f, tocando, true);
    }
    const int64_t ir = s_ped_ir.exchange(-1);
    if (ir >= 0 && musica_arquivo_aberto()) {
      if (som) { calar_fila(); }
      musica_ir_para_ms((uint32_t)ir);
      publicar(f, tocando, false);
    }

    // ---- pausado: espera o proximo pedido ----
    // PAUSA LONGA ENCERRA (contrato em kernel/fundo.h): esquecido pausado, o
    // tocador ocuparia uma vaga de fundo e a barra para sempre.
    if (!tocando || !musica_arquivo_aberto()) {
      const int64_t agora = esp_timer_get_time();
      // Sem nada para tocar (a lista acabou, parou) nao ha o que retomar:
      // encerra logo e a pastilha sai da barra.
      const int max_s = !musica_arquivo_aberto() ? TAB5_MUSICA_VAZIO_MAX_S : TAB5_MUSICA_PAUSA_MAX_S;
      if (!pausado_desde) { pausado_desde = agora; }
      else if (agora - pausado_desde > (int64_t)max_s * 1000000) {
        ESP_LOGI(TAG, "%s ha %d s: encerrando", !musica_arquivo_aberto() ? "sem nada para tocar" : "pausado", max_s);
        break;
      }
      if (!fundo_esperar(f, 100)) { break; }
      continue;
    }
    pausado_desde = 0;

    // ---- tocando ----
    if (!som) {
      if (!sistema_som_fluxo_abrir(2000)) {
        g_musica_trava.travar();
        snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "alto-falante ocupado");
        g_musica_trava.destravar();
        tocando = false;
        publicar(f, false, false);
        continue;
      }
      som = true;
      recomeco = true;
    }
    if (!sistema_som_fluxo_livre()) {
      if (!fundo_esperar(f, 15)) { break; }
      continue;
    }
    uint32_t hz = 0;
    bool estereo = true;
    g_musica_ler_us = 0;
    const int64_t t0 = esp_timer_get_time();
    const size_t q = musica_decodificar_bloco(s_bloco[vez], &hz, &estereo);
    const uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t0);
    const int na_fila = sistema_som_fluxo_na_fila();   // DEPOIS: o buraco acontece enquanto decodifica
    if (q == 0) { acabou = true; continue; }   // a proxima da fila, na volta de cima
    g_musica_trava.travar();
    ++g_musica_est.blocos;
    if (na_fila == 0 && !recomeco) {
      ++g_musica_est.secas;
      ESP_LOGW(TAG, "a fila do alto-falante secou (bloco %lu: %lu us, %lu deles lendo o cartao)",
               (unsigned long)g_musica_est.blocos, (unsigned long)dec_us, (unsigned long)g_musica_ler_us);
    }
    if (dec_us > g_musica_est.dec_max_us) { g_musica_est.dec_max_us = dec_us; g_musica_est.ler_no_pior_us = g_musica_ler_us; }
    g_musica_est.ler_soma_ms += g_musica_ler_us / 1000;
    s_dec_soma_us += dec_us;
    g_musica_est.dec_media_us = (uint32_t)(s_dec_soma_us / g_musica_est.blocos);
    g_musica_est.bloco_ms = hz ? (uint32_t)(q * 1000u / hz) : 0;
    g_musica_trava.destravar();
    recomeco = false;
    // QUANDO ESTE BLOCO SAI: logo (fila vazia) ou quando acabar o que ja esta
    // na fila - e o relogio das barras do espectro.
    const int64_t agora_us = esp_timer_get_time();
    const int64_t ini_us = (na_fila == 0 || g_musica_fim_prev_us < agora_us) ? agora_us + LATENCIA_US : g_musica_fim_prev_us;
    g_musica_fim_prev_us = ini_us + (int64_t)q * 1000000 / hz;
    if (!sistema_som_fluxo_tocar(s_bloco[vez], q, hz, estereo)) {
      ESP_LOGI(TAG, "o alto-falante foi tomado (pendrive): parando");
      break;
    }
    {
      const int64_t t0e = esp_timer_get_time();
      musica_espectro_bloco(s_bloco[vez], q, hz, estereo, ini_us);
      const uint32_t esp_us = (uint32_t)(esp_timer_get_time() - t0e);
      if (esp_us > g_musica_est.esp_max_us) { g_musica_est.esp_max_us = esp_us; }
    }
    vez = (vez + 1) % BLOCOS;
    g_musica_ult_q = (uint32_t)q;
    const uint32_t seg_antes = g_musica_hz ? (uint32_t)(g_musica_quadros / g_musica_hz) : 0;
    g_musica_quadros += q;
    if (g_musica_hz && (uint32_t)(g_musica_quadros / g_musica_hz) != seg_antes) { publicar(f, true, false); }
  }

  musica_fechar_arquivo();
  if (som) { sistema_som_fluxo_fechar(); }
  musica_espectro_esquecer();
  midia_soltar();
  soltar_discos(&seg);
}

static void liberar(void)
{
  for (int i = 0; i < BLOCOS; ++i) { heap_caps_free(s_bloco[i]); s_bloco[i] = nullptr; }
  heap_caps_free(g_musica_arq_buf); g_musica_arq_buf = nullptr;
  heap_caps_free(g_musica_ler_buf); g_musica_ler_buf = nullptr;
  musica_espectro_soltar();
  heap_caps_free(g_musica_quadro); g_musica_quadro = nullptr;
  heap_caps_free(g_musica_mp3); g_musica_mp3 = nullptr;
  heap_caps_free(minimp3_scratch); minimp3_scratch = nullptr;
}

// Na interface, antes de o kernel subir o laco: a memoria toda, na PSRAM.
// Faltou: solta o que conseguiu e recusa (o kernel nao chama ao_parar).
static bool ao_iniciar(void)
{
  bool ok = true;
  for (int i = 0; i < BLOCOS; ++i) {
    s_bloco[i] = (int16_t*)heap_caps_malloc(BLOCO_QUADROS * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ok = ok && s_bloco[i];
  }
  g_musica_arq_buf = (uint8_t*)heap_caps_malloc(ARQ_BUF, MALLOC_CAP_SPIRAM);
  g_musica_ler_buf = (uint8_t*)heap_caps_aligned_alloc(128, LER_PEDACO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  g_musica_quadro = (int16_t*)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  g_musica_mp3 = (mp3dec_t*)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
  // Na PSRAM: medido A/B contra a RAM interna, o tempo por bloco foi o
  // mesmo (~15-20 ms para 156 ms de som) - nao vale gastar 23 KB internos.
  minimp3_scratch = heap_caps_malloc(minimp3_scratch_bytes(), MALLOC_CAP_SPIRAM);
  ok = ok && g_musica_arq_buf && g_musica_ler_buf && g_musica_quadro && g_musica_mp3 && minimp3_scratch && musica_espectro_alocar();
  if (ok) { sistema_recurso_usar(RECURSO_SOM, "Música"); }
  if (!ok) {
    ESP_LOGE(TAG, "sem PSRAM para o tocador");
    g_musica_trava.travar();
    snprintf(g_musica_est.erro, sizeof(g_musica_est.erro), "sem memória");
    g_musica_trava.destravar();
    liberar();
  }
  return ok;
}

static void ao_parar(void)
{
  sistema_recurso_soltar(RECURSO_SOM, "Música");
  liberar();
  g_musica_trava.travar();
  g_musica_est.tocando = false;
  g_musica_est.pos_ms = 0;
  g_musica_trava.destravar();
  ESP_LOGI(TAG, "parado");
}

static bool sem_bip(void) { return false; }   // musica nao "avisa" com bip

// Pilha 6 KB: a maior vaga do pool. O scratch do minimp3 (16 KB) e os
// buffers estao na PSRAM; na pilha so ficam as variaveis do decodificador.
// Tocando, declara RECURSO_SOM (ao_iniciar/ao_parar): a politica dele segura o
// clock em 180 MHz com a tela apagada - no minimo (40) o MP3 nao decodifica a
// tempo e o som falha.
const fundo_def_t g_fundo_musica = {
  .nome       = "Música",
  .corpo      = laco_musica,
  .pilha      = 6144,
  .perfil     = { .alto_falante = true },
  .ao_iniciar = ao_iniciar,
  .ao_parar   = ao_parar,
  .com_som    = sem_bip,
  // Acima da interface (4) e da captura da camera (3): com as duas no ar o
  // decodificador na 1 ficava sem vez e o som engasgava. Cabe porque o laco
  // decodifica um bloco (~418 ms de som) em uma fracao disso e dorme ate a
  // fila do alto-falante ter lugar - ver fundo_def_t.prioridade.
  .prioridade = 5,
};

// TROCA A FILA e liga o tocador (ou so troca, se ja tocava). 'itens' = 'n'
// caminhos relativos a 'raiz'; 'inicio' = o indice em itens que toca
// primeiro (-1 = o comeco da ordem). 'pasta_rel'/'playlist' dizem de onde a
// fila veio (um dos dois).
bool musica_fila_tocar(const caminho_t* itens, int n, int inicio, const char* raiz,
                       const char* pasta_rel, const char* playlist, const char* app)
{
  if (n <= 0) { return false; }
  if (n > TAB5_MUSICA_FILA_MAX) { n = TAB5_MUSICA_FILA_MAX; }
  if (inicio >= n) { inicio = -1; }
  g_musica_trava.travar();
  if (!g_musica_fila)  { g_musica_fila  = (caminho_t*)heap_caps_malloc(sizeof(caminho_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM); }
  if (!g_musica_ordem) { g_musica_ordem = (int16_t*)heap_caps_malloc(sizeof(int16_t) * TAB5_MUSICA_FILA_MAX, MALLOC_CAP_SPIRAM); }
  if (!g_musica_fila || !g_musica_ordem) { g_musica_trava.destravar(); return false; }
  memcpy(g_musica_fila, itens, sizeof(caminho_t) * (size_t)n);
  g_musica_fila_n = n;
  ++s_fila_gen;
  snprintf(g_musica_fila_raiz, sizeof(g_musica_fila_raiz), "%s", raiz);
  const bool alea = tab5_musica_aleatorio();
  int pos = montar_ordem(inicio, alea);
  if (pos < 0) { pos = 0; }
  if (pasta_rel) {
    snprintf(g_musica_est.rel, sizeof(g_musica_est.rel), "%s", pasta_rel);
    if (absoluto(pasta_rel)) { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s", pasta_rel); }
    else if (pasta_rel[0])   { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s/%s", raiz, pasta_rel); }
    else                     { snprintf(g_musica_est.pasta, sizeof(g_musica_est.pasta), "%s", raiz); }
  } else {
    g_musica_est.rel[0] = g_musica_est.pasta[0] = '\0';
  }
  snprintf(g_musica_est.playlist, sizeof(g_musica_est.playlist), "%s", playlist ? playlist : "");
  g_musica_est.n = n;
  g_musica_est.erro[0] = '\0';
  g_musica_est.aleatorio = alea;
  g_musica_est.repetir = (uint8_t)tab5_musica_repetir();
  snprintf(s_app_dono, sizeof(s_app_dono), "%s", app && app[0] ? app : g_fundo_musica.nome);
  g_musica_trava.destravar();

  s_ped_alternar = false;
  s_ped_pular = 0;
  s_ped_ir = -1;
  s_ped_ordem = false;
  s_ped_abrir = pos;
  if (sistema_fundo_ativo(&g_fundo_musica)) {
    // Ja tocando: a fila troca, e o miniplayer passa a abrir quem pediu agora.
    midia_assumir(g_fundo_musica.nome, s_app_dono, &CONTROLES);
    midia_definir_cor(ajustes_cor_tocador());
  } else if (!sistema_fundo_iniciar(&g_fundo_musica)) {
    s_ped_abrir = -1;
    return false;
  }
  return true;
}

bool tab5_musica_tocar_um(const char* caminho, const char* app)
{
  if (!absoluto(caminho) || strlen(caminho) >= sizeof(caminho_t)) { return false; }
  caminho_t* fila = (caminho_t*)heap_caps_malloc(sizeof(caminho_t), MALLOC_CAP_SPIRAM);
  if (!fila) { return false; }
  snprintf(*fila, sizeof(caminho_t), "%s", caminho);
  // Caminho absoluto: a raiz da fila nao e usada; nao ha pasta de origem.
  const bool ok = musica_fila_tocar(fila, 1, 0, "", nullptr, nullptr, app);
  heap_caps_free(fila);
  return ok;
}

