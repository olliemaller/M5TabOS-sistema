#include "notas.h"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_volumes.hpp"
#include "trabalho.hpp"
#include "esp_heap_caps.h"
#include "tab5_unicode.hpp"
#include <stdio.h>
#include <string.h>

#define FORMATO "nota_%04lu.txt"

// The first two lines with text (the title and the preview), without spaces at the
// ends, cut on a whole character. Reads only the start of the file.
static void ler_titulo(const char* arquivo, char titulo[NOTA_TITULO_MAX], char previa[NOTA_TITULO_MAX])
{
  titulo[0] = previa[0] = 0;
  FILE* f = tab5_sd_abrir(NOTAS_APP, arquivo, "r");
  if (!f) { return; }
  char linha[NOTA_TITULO_MAX * 2];
  char* destino[2] = { titulo, previa };
  int k = 0;
  for (int lidas = 0; k < 2 && lidas < 40 && fgets(linha, sizeof(linha), f); ++lidas) {
    char* p = linha;
    while (*p == ' ' || *p == '\t') { ++p; }
    size_t n = strcspn(p, "\r\n");
    if (!n) { continue; }
    if (n > NOTA_TITULO_MAX - 1) { n = NOTA_TITULO_MAX - 1; }
    n = tab5_utf8_corte(p, (int)n);
    while (n > 0 && p[n - 1] == ' ') { --n; }
    memcpy(destino[k], p, n);
    destino[k][n] = 0;
    ++k;
  }
  fclose(f);
}

int notas_listar(nota_t* out, int max, char prox[NOTA_ARQ_MAX])
{
  char pasta[96];
  if (!tab5_sd_pasta_caminho(NOTAS_APP, pasta, sizeof(pasta))) { return -1; }
  snprintf(prox, NOTA_ARQ_MAX, FORMATO, 1ul);
  tab5_arq_item_t* it = (tab5_arq_item_t*)heap_caps_calloc(max, sizeof(tab5_arq_item_t), MALLOC_CAP_SPIRAM);
  if (!it) { return -1; }
  const int n = tab5_arq_listar(pasta, it, max, nullptr, TAB5_ARQ_ORDEM_DATA, ".txt");
  // The folder doesn't exist yet (no notes): with the SD card present, the list is empty.
  const int r = n >= 0 ? n : (tab5_volume_disponivel(pasta) ? 0 : -1);
  unsigned long maior = 0;
  for (int i = 0; i < n; ++i) {
    unsigned long num = 0;
    if (sscanf(it[i].nome, "nota_%lu.txt", &num) == 1 && num > maior) { maior = num; }
    snprintf(out[i].arquivo, NOTA_ARQ_MAX, "%s", it[i].nome);
    out[i].data = it[i].data;
    ler_titulo(it[i].nome, out[i].titulo, out[i].previa);
  }
  heap_caps_free(it);
  snprintf(prox, NOTA_ARQ_MAX, FORMATO, maior + 1);
  return r;
}

bool notas_ler(const char* arquivo, char* buf, int cap)
{
  buf[0] = 0;
  FILE* f = tab5_sd_abrir(NOTAS_APP, arquivo, "r");
  if (!f) { return false; }
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  n = tab5_utf8_corte(buf, (int)n);   // larger file: cut on a whole character
  buf[n] = 0;
  return true;
}

bool notas_gravar(const char* arquivo, const char* texto)
{
  if (!texto[0]) { notas_apagar(arquivo); return true; }
  return tab5_sd_substituir(NOTAS_APP, arquivo, texto, strlen(texto));
}

// The copy of the text that the detached job writes and frees.
typedef struct { char arq[NOTA_ARQ_MAX]; char txt[]; } copia_t;

static void trab_gravar_copia(void* a)
{
  copia_t* c = (copia_t*)a;
  notas_gravar(c->arq, c->txt);
  heap_caps_free(c);
}

bool notas_gravar_em_fundo(const char* arquivo, const char* texto)
{
  const size_t n = strlen(texto);
  copia_t* c = (copia_t*)heap_caps_malloc(sizeof(copia_t) + n + 1, MALLOC_CAP_SPIRAM);
  if (!c) { return false; }
  snprintf(c->arq, sizeof(c->arq), "%s", arquivo);
  memcpy(c->txt, texto, n + 1);
  if (sistema_trabalho_solto(trab_gravar_copia, c)) { return true; }
  heap_caps_free(c);
  return false;
}

bool notas_apagar(const char* arquivo) { return tab5_sd_apagar(NOTAS_APP, arquivo); }
