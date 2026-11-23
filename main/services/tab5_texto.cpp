// tab5_texto: see the .hpp for the why.
#include "tab5_texto.hpp"
#include "tab5_volumes.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_linhas.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

static const char* TAG = "tab5_texto";

#define PONTOS_MAX 256   // how many jump points the index keeps

struct tab5_texto {
  char     caminho[TAB5_ARQ_CAMINHO_MAX + 96];
  int      total;                  // lines in the file
  int      passo;                  // one jump point every 'passo' blocks of lines
  uint64_t ponto[PONTOS_MAX];      // where block k * passo starts (UINT64_MAX = none)
  bool     csv;
};

// CSV wraps at the field: a space after each separator.
static void espacar_csv(const char* de, char* para, size_t n)
{
  size_t k = 0;
  for (const char* p = de; *p && k + 1 < n; ++p) {
    para[k++] = *p;
    if ((*p == ',' || *p == ';') && p[1] != ' ' && p[1] != 0 && k + 1 < n) { para[k++] = ' '; }
  }
  para[k] = 0;
}

// ---------------------------------------------------------------------------
// Open: count the lines and build the index
// ---------------------------------------------------------------------------
tab5_texto_t* tab5_texto_abrir(const char* caminho, bool (*cancelado)(void))
{
  if (!caminho || strlen(caminho) >= sizeof(((tab5_texto_t*)0)->caminho)) { return nullptr; }
  // The index goes into PSRAM: it only exists while the text is open.
  tab5_texto_t* t = (tab5_texto_t*)heap_caps_calloc(1, sizeof(tab5_texto_t), MALLOC_CAP_SPIRAM);
  if (!t) { return nullptr; }
  strcpy(t->caminho, caminho);
  t->csv = tab5_arq_extensao_e(caminho, ".csv");

  tab5_leitor_t* r = tab5_leitor_abrir(caminho);
  if (!r) { ESP_LOGW(TAG, "could not open '%s'", caminho); heap_caps_free(t); return nullptr; }

  // One point per block of lines; afterwards 'passo' shrinks the table to what fits.
  uint64_t* fronteira = (uint64_t*)heap_caps_malloc(PONTOS_MAX * sizeof(uint64_t), MALLOC_CAP_SPIRAM);
  if (!fronteira) { tab5_leitor_fechar(r); heap_caps_free(t); return nullptr; }
  int n_fronteiras = 0;
  fronteira[n_fronteiras++] = 0;

  static const int TAM_BLOCO = 4096;
  uint8_t* bloco = (uint8_t*)heap_caps_malloc(TAM_BLOCO, MALLOC_CAP_SPIRAM);
  if (!bloco) { heap_caps_free(fronteira); tab5_leitor_fechar(r); heap_caps_free(t); return nullptr; }

  int total = 0, voltas = 0;
  uint64_t pos = 0;
  uint8_t ultimo = '\n';
  int lidos;
  while ((lidos = tab5_leitor_ler(r, bloco, TAM_BLOCO)) > 0) {
    ultimo = bloco[lidos - 1];
    const uint8_t* p = bloco;
    const uint8_t* fim = bloco + lidos;
    while ((p = (const uint8_t*)memchr(p, '\n', (size_t)(fim - p))) != nullptr) {
      total++;
      p++;
      if ((total % TAB5_TEXTO_BLOCO) == 0 && n_fronteiras < PONTOS_MAX) {
        fronteira[n_fronteiras++] = pos + (uint64_t)(p - bloco);
      }
    }
    pos += (uint64_t)lidos;
    // Every 64 KB, yield the CPU and check for cancellation (does not freeze the interface).
    if ((++voltas % 16) == 0) {
      if (cancelado && cancelado()) { lidos = -2; break; }
      vTaskDelay(1);
    }
  }
  heap_caps_free(bloco);
  tab5_leitor_fechar(r);
  if (lidos == -2) { heap_caps_free(fronteira); heap_caps_free(t); return nullptr; }

  if (ultimo != '\n') { total++; }
  t->total = total;
  const int blocos = (total > 0) ? (total + TAB5_TEXTO_BLOCO - 1) / TAB5_TEXTO_BLOCO : 1;
  t->passo = (blocos + PONTOS_MAX - 1) / PONTOS_MAX;
  if (t->passo < 1) { t->passo = 1; }
  for (int k = 0; k < PONTOS_MAX; ++k) {
    const int b = k * t->passo;
    t->ponto[k] = (b < n_fronteiras) ? fronteira[b] : UINT64_MAX;
  }
  heap_caps_free(fronteira);
  return t;
}

int tab5_texto_linhas(const tab5_texto_t* t) { return t ? t->total : 0; }

// ---------------------------------------------------------------------------
// Read a section
// ---------------------------------------------------------------------------
int tab5_texto_ler(tab5_texto_t* t, int inicio, int max, char* saida, size_t larg)
{
  if (!t || !saida || larg < 6 || max <= 0 || inicio < 0) { return -1; }
  tab5_linhas_t* l = tab5_linhas_abrir(t->caminho);
  if (!l) { ESP_LOGW(TAG, "could not open '%s'", t->caminho); return -1; }

  // Jump to the nearest index point before the line and skip the rest.
  const int casa = (inicio / TAB5_TEXTO_BLOCO) / t->passo;
  int pular = inicio;
  if (casa < PONTOS_MAX && t->ponto[casa] != UINT64_MAX && tab5_linhas_ir(l, t->ponto[casa])) {
    pular = inicio - casa * t->passo * TAB5_TEXTO_BLOCO;
  }

  char* linha = (char*)heap_caps_malloc(larg, MALLOC_CAP_SPIRAM);
  if (!linha) { tab5_linhas_fechar(l); return -1; }

  bool cortada;
  int voltas = 0;
  while (pular > 0 && tab5_linhas_proxima(l, linha, larg, &cortada)) {
    pular--;
    if ((++voltas % 256) == 0) { vTaskDelay(1); }
  }
  int n = 0;
  while (n < max && tab5_linhas_proxima(l, linha, larg, &cortada)) {
    char* dst = saida + (size_t)n * larg;
    if (t->csv) { espacar_csv(linha, dst, larg); }
    else        { snprintf(dst, larg, "%s", linha); }
    if (cortada) {
      size_t m = strlen(dst);
      if (m > larg - 5) { m = larg - 5; }
      while (m > 0 && ((unsigned char)dst[m] & 0xC0) == 0x80) { --m; }   // do not split a UTF-8 character
      memcpy(dst + m, " ...", 5);
    }
    n++;
  }
  heap_caps_free(linha);
  tab5_linhas_fechar(l);
  return n;
}

void tab5_texto_fechar(tab5_texto_t* t) { heap_caps_free(t); }
