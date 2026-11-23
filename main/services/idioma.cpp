#include "idioma.h"
#include "ajustes.h"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <atomic>

static const char* TAG = "idioma";

// O ingles embutido (main/CMakeLists.txt, EMBED_TXTFILES).
extern const char EN_PO[] asm("_binary_en_po_start");

typedef struct { const char* pt; const char* tr; } par_t;
typedef struct { char codigo[8]; char nome[24]; par_t* pares; int n; } lingua_t;

#define LINGUAS_MAX 8
static lingua_t              s_l[LINGUAS_MAX] = { { "pt", "Português", nullptr, 0 } };
static int                   s_n = 1;
static std::atomic<int>      s_atual{0};
static std::atomic<uint32_t> s_versao{1};

// ---------------------------------------------------------------------------
// O .po: msgid "..." / msgstr "..." (com linhas "..." de continuacao), '#'
// e comentario. O texto e copiado para a PSRAM e decodificado ali mesmo: cada
// frase vira um ponteiro para dentro dessa copia.
// ---------------------------------------------------------------------------
// Decodifica a string entre aspas que comeca em 'r', escrevendo em 'w'.
// Devolve onde 'w' parou; 'r' anda ate depois da aspa final.
static char* aspas(char*& r, char* w)
{
  r = strchr(r, '"');
  if (!r) { return w; }
  for (++r; *r && *r != '"' && *r != '\n'; ++r) {
    if (*r != '\\' || !r[1]) { *w++ = *r; continue; }
    ++r;
    *w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
  }
  if (*r == '"') { ++r; }
  return w;
}

static int comparar(const void* a, const void* b) { return strcmp(((const par_t*)a)->pt, ((const par_t*)b)->pt); }

// Valor de um campo do cabecalho ("Language: fr\n...").
static void campo(const char* cab, const char* nome, char* out, size_t n)
{
  const char* p = strstr(cab, nome);
  if (!p) { return; }
  p += strlen(nome);
  while (*p == ' ') { ++p; }
  size_t k = 0;
  while (p[k] && p[k] != '\n' && k < n - 1) { out[k] = p[k]; ++k; }
  out[k] = 0;
}

static bool carregar(const char* po, size_t tam, lingua_t* l)
{
  char* t = (char*)heap_caps_malloc(tam + 1, MALLOC_CAP_SPIRAM);
  int cap = 1;
  for (const char* p = po; (p = strstr(p, "msgid")) != nullptr; p += 5) { ++cap; }
  par_t* pares = (par_t*)heap_caps_malloc(sizeof(par_t) * cap, MALLOC_CAP_SPIRAM);
  if (!t || !pares) { heap_caps_free(t); heap_caps_free(pares); return false; }
  memcpy(t, po, tam);
  t[tam] = 0;

  int n = 0;
  char* id = nullptr;
  char* atual = nullptr;   // a string em montagem (id ou str)
  char* w = nullptr;
  auto fechar = [&]() { if (atual) { *w = 0; atual = nullptr; } };
  char* r = t;
  while (*r) {
    char* fim = strchr(r, '\n');
    if (!fim) { fim = r + strlen(r); }
    if (!strncmp(r, "msgid ", 6) || !strncmp(r, "msgstr ", 7)) {
      const bool eh_id = r[3] == 'i';
      fechar();
      if (!eh_id && id && n < cap) { pares[n].pt = id; }
      atual = w = r;
      w = aspas(r, w);
      if (eh_id) { id = atual; } else if (id && n < cap) { pares[n++].tr = atual; id = nullptr; }
    } else if (*r == '"' && atual) {
      w = aspas(r, w);   // continuacao: emenda na mesma string (w sempre atras de r)
    } else {
      fechar();
    }
    r = *fim ? fim + 1 : fim;
  }
  fechar();

  // O cabecalho (msgid "") traz o codigo e o nome; traducao vazia nao conta.
  int k = 0;
  for (int i = 0; i < n; ++i) {
    if (!pares[i].pt[0]) {
      campo(pares[i].tr, "Language:", l->codigo, sizeof(l->codigo));
      campo(pares[i].tr, "X-Language-Name:", l->nome, sizeof(l->nome));
    } else if (pares[i].tr[0]) {
      pares[k++] = pares[i];
    }
  }
  qsort(pares, k, sizeof(par_t), comparar);
  if (!l->nome[0]) { snprintf(l->nome, sizeof(l->nome), "%s", l->codigo); }
  if (!l->codigo[0]) {   // no "Language:" header: nothing will keep these, so free them
    heap_caps_free(pares);
    heap_caps_free(t);
    return false;
  }
  l->pares = pares;
  l->n = k;
  return true;
}

// Entra na lista; o mesmo codigo de novo (en.po no cartao) substitui.
static void entrar(const char* po, size_t tam, const char* de_onde)
{
  lingua_t l = {};
  if (!carregar(po, tam, &l)) { ESP_LOGW(TAG, "%s sem \"Language:\" - ignorado", de_onde); return; }
  int i = 0;
  while (i < s_n && strcmp(s_l[i].codigo, l.codigo) != 0) { ++i; }
  if (i == 0) { ESP_LOGW(TAG, "%s: o portugues e o idioma do codigo", de_onde); return; }
  if (i == s_n) {
    if (s_n == LINGUAS_MAX) { return; }
    ++s_n;
  }
  s_l[i] = l;   // o anterior fica na memoria: tr() pode ter devolvido um ponteiro dele
  ESP_LOGI(TAG, "%s (%s): %d frases, de %s", l.nome, l.codigo, l.n, de_onde);
}

static void ler_arquivo(const tab5_arq_item_t* it)
{
  if (it->tipo == TAB5_ARQ_PASTA || it->bytes == 0 || it->bytes > 512 * 1024) { return; }
  const size_t n = (size_t)it->bytes;
  char* buf = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  FILE* f = buf ? tab5_sd_abrir(IDIOMAS_PASTA, it->nome, "r") : nullptr;
  if (f) {
    const size_t lidos = fread(buf, 1, n, f);
    fclose(f);
    entrar(buf, lidos, it->nome);
  }
  heap_caps_free(buf);   // carregar() copiou
}

void idioma_iniciar(void)
{
  entrar(EN_PO, strlen(EN_PO), "firmware");
  char pasta[96];
  if (tab5_sd_pasta_caminho(IDIOMAS_PASTA, pasta, sizeof(pasta))) {
    tab5_arq_item_t itens[LINGUAS_MAX];
    const int n = tab5_arq_listar(pasta, itens, LINGUAS_MAX, nullptr, TAB5_ARQ_ORDEM_NOME, ".po");
    for (int i = 0; i < n; ++i) { ler_arquivo(&itens[i]); }
  }
  const char* escolhido = ajustes_idioma();
  for (int i = 0; i < s_n; ++i) { if (!strcmp(s_l[i].codigo, escolhido)) { s_atual = i; } }
  ++s_versao;
}

int         idioma_n(void)        { return s_n; }
const char* idioma_codigo(int i)  { return (i >= 0 && i < s_n) ? s_l[i].codigo : "pt"; }
const char* idioma_nome(int i)    { return (i >= 0 && i < s_n) ? s_l[i].nome : "Português"; }
int         idioma_atual(void)    { return s_atual; }
uint32_t    idioma_versao(void)   { return s_versao; }

void idioma_definir(int i)
{
  if (i < 0 || i >= s_n || i == s_atual) { return; }
  s_atual = i;
  ajustes_definir_idioma(s_l[i].codigo);
  ++s_versao;
}

const char* tr(const char* pt)
{
  const int i = s_atual;
  if (!pt || i == 0) { return pt; }
  const lingua_t& l = s_l[i];
  const par_t chave = { pt, nullptr };
  const par_t* p = (const par_t*)bsearch(&chave, l.pares, l.n, sizeof(par_t), comparar);
  return p ? p->tr : pt;
}

// Troca {nome} por 'valor' em 'out' (uma vez).
static void trocar(char* out, size_t n, const char* nome, const char* valor)
{
  char* p = strstr(out, nome);
  if (!p) { return; }
  char resto[160];   // what follows the placeholder (96 cut long translated date formats)
  snprintf(resto, sizeof(resto), "%s", p + strlen(nome));
  snprintf(p, n - (size_t)(p - out), "%s%s", valor, resto);
}

void idioma_data_por_extenso(const struct tm* t, char* out, size_t n)
{
  static const char* const DIAS[7] = { N_("domingo"), N_("segunda-feira"), N_("terça-feira"), N_("quarta-feira"),
                                       N_("quinta-feira"), N_("sexta-feira"), N_("sábado") };
  static const char* const MESES[12] = { N_("janeiro"), N_("fevereiro"), N_("março"), N_("abril"), N_("maio"),
                                         N_("junho"), N_("julho"), N_("agosto"), N_("setembro"), N_("outubro"),
                                         N_("novembro"), N_("dezembro") };
  char dia_n[4];
  snprintf(dia_n, sizeof(dia_n), "%d", t->tm_mday);
  snprintf(out, n, "%s", tr("{dia}, {n} de {mes}"));
  trocar(out, n, "{dia}", tr(DIAS[t->tm_wday % 7]));
  trocar(out, n, "{n}", dia_n);
  trocar(out, n, "{mes}", tr(MESES[t->tm_mon % 12]));
}
