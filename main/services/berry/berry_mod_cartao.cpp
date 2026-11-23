// Berry modules 'cartao' (the app's folder) and 'arquivos' (the disks, via the system's
// file engine). See berry_apps.cpp (the engine) and berry_interno.hpp.
// The SD card only through the doors in hal/tab5_sd.hpp.

#include <stdio.h>
#include "idioma.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "tab5_sd.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_volumes.hpp"
#include "berry_interno.hpp"

// MODULE 'cartao' - files in the app's folder: /sdcard/apps/Berry/<app name>/
// ---------------------------------------------------------------------------
// THE DATA LIVES NEXT TO THE SCRIPTS: everything Berry stays inside
// apps/Berry/ (the .be and the folder of the same name) - nothing scattered in apps/
// among the system apps' folders, and a Berry app "Azimutal" does not
// mix its files with the C Azimutal. Reading the folder (ler_pasta) only picks up
// .be, so the data folders do not become icons.
// The folder is created (mkdir on the card) ONCE per opening, and the path is
// kept: reading/writing on every frame does not pay a stat + mkdir each time.
static bool pasta_do_app(void)
{
  if (C->app->pasta[0]) { return true; }
  if (!tab5_sd_montar()) { return false; }
  char rel[48];
  pasta_rel(C->app->nome, rel, sizeof(rel));
  if (!tab5_sd_pasta(rel, C->app->pasta, sizeof(C->app->pasta))) { C->app->pasta[0] = 0; }
  tab5_sd_desmontar();
  return C->app->pasta[0] != 0;
}

// Simple names only (no slash, no ..): the script does not leave its folder.
static bool caminho(bvm* vm, char* out, size_t tam)
{
  const char* nome = arg_txt(vm, 1);
  if (!C->app || !nome[0] || strchr(nome, '/') || strchr(nome, '\\') || strstr(nome, "..") || strlen(nome) > 48) {
    be_raise(vm, "cartao_error", tr("nome de arquivo invalido (so o nome, sem pasta)"));
    return false;
  }
  if (!pasta_do_app()) { return false; }
  snprintf(out, tam, "%s/%s", C->app->pasta, nome);
  return true;
}

static const long LER_MAX = 64 * 1024;

// ler(nome) - the whole file as text, or nil if it does not exist (up to 64 KB).
static int c_ler(bvm* vm)
{
  char cam[160];
  if (!caminho(vm, cam, sizeof(cam)) || !tab5_sd_montar()) { be_return_nil(vm); }
  FILE* f = fopen(cam, "rb");
  char* buf = nullptr;
  long n = 0;
  if (f) {
    n = tab5_sd_tamanho(f);
    if (n > LER_MAX) { n = LER_MAX; }
    buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (buf) { n = (long)fread(buf, 1, n, f); }
    fclose(f);
  }
  tab5_sd_desmontar();
  if (!buf) { be_return_nil(vm); }
  be_pushnstring(vm, buf, (size_t)n);
  heap_caps_free(buf);
  be_return(vm);
}

static int escrever(bvm* vm, const char* modo)
{
  char cam[160];
  if (!caminho(vm, cam, sizeof(cam)) || !tab5_sd_montar()) { be_pushbool(vm, false); be_return(vm); }
  const char* t = arg_txt(vm, 2);
  FILE* f = fopen(cam, modo);
  bool ok = false;
  if (f) { ok = fwrite(t, 1, strlen(t), f) == strlen(t); ok = (fclose(f) == 0) && ok; }
  tab5_sd_desmontar();
  be_pushbool(vm, ok);
  be_return(vm);
}
static int c_gravar(bvm* vm)  { return escrever(vm, "wb"); }   // gravar(nome, texto): replaces
static int c_anexar(bvm* vm)  { return escrever(vm, "ab"); }   // anexar(nome, texto): at the end

static int c_existe(bvm* vm)
{
  char cam[160];
  struct stat st;
  bool ok = caminho(vm, cam, sizeof(cam)) && tab5_sd_montar();
  if (ok) { ok = stat(cam, &st) == 0; tab5_sd_desmontar(); }
  be_pushbool(vm, ok);
  be_return(vm);
}

// lista() - the names of the files in the app's folder.
static int c_lista(bvm* vm)
{
  lista_nova(vm);
  if (C->app && pasta_do_app()) {
    tab5_arq_percorrer(C->app->pasta, [](const tab5_arq_item_t* it, void* v) {
      l_txt((bvm*)v, it->nome);
      return true;
    }, vm);
  }
  fim(vm);
  be_return(vm);
}

// ---------------------------------------------------------------------------
// MODULE 'arquivos' - the whole disks, via the system's file engine
// ---------------------------------------------------------------------------
// THE RULE: the script SEES the whole card and the flash drive (with the
// system's rules: passwords and names starting with '.' do not appear), but only CHANGES its own folder
// (apps/Berry/<nome>/). From outside it, the only thing it does is COPY
// into it - reading the content only after bringing it in. Nothing outside the folder
// is written, deleted, moved or renamed by a script: a third-party app
// does not damage the system or the person's files.
static char s_arq_dono[24] = "";   // the app that started the copy in progress ("" = none, or the system)

static const char* arq_tipo_txt(tab5_arq_tipo_t t)
{
  switch (t) {
    case TAB5_ARQ_PASTA:  return "pasta";
    case TAB5_ARQ_MUSICA: return "musica";
    case TAB5_ARQ_IMAGEM: return "imagem";
    case TAB5_ARQ_TEXTO:  return "texto";
    case TAB5_ARQ_BERRY:  return "berry";
    default:              return "outro";
  }
}

// A path INSIDE the app's folder: "" = the folder itself; "sub/x.txt".
// Leaving it (absolute, "..", "\\") is an error in the script.
static bool arq_meu(bvm* vm, const char* rel, char* out, size_t tam)
{
  if (!C->app) { be_raise(vm, "arquivos_error", tr("arquivos so dentro de um app")); return false; }
  if (rel[0] == '/' || strstr(rel, "..") || strchr(rel, '\\') || strlen(rel) > 160) {
    be_raise(vm, "arquivos_error", tr("caminho invalido: relativo a pasta do app (\"sub/x.txt\"), sem .."));
    return false;
  }
  if (!pasta_do_app()) { be_raise(vm, "arquivos_error", tr("sem cartao")); return false; }
  if (rel[0]) { snprintf(out, tam, "%s/%s", C->app->pasta, rel); }
  else        { snprintf(out, tam, "%s", C->app->pasta); }
  size_t n = strlen(out);
  while (n > 1 && out[n - 1] == '/') { out[--n] = '\0'; }
  return true;
}

// Absolute ("/usb/Rock") as is; relative, inside the app's folder.
static bool arq_qualquer(bvm* vm, const char* c, char* out, size_t tam)
{
  if (c[0] != '/') { return arq_meu(vm, c, out, tam); }
  snprintf(out, tam, "%s", c);
  size_t n = strlen(out);
  while (n > 1 && out[n - 1] == '/') { out[--n] = '\0'; }
  return true;
}

// volumes() - [{raiz, tipo, nome, disponivel}]: the card and, when plugged in, the flash drive.
static int a_volumes(bvm* vm)
{
  tab5_vol_t v[TAB5_VOL_N];
  const int n = tab5_volumes_listar(v, TAB5_VOL_N);
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    mapa_novo(vm);
    m_txt(vm, "raiz", v[i].raiz);
    m_txt(vm, "tipo", v[i].tipo);
    m_txt(vm, "nome", v[i].nome);
    m_bool(vm, "disponivel", v[i].disponivel);
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  be_return(vm);
}

// pasta() - the absolute path of the app's folder (where it can do anything).
static int a_pasta(bvm* vm)
{
  if (!C->app || !pasta_do_app()) { be_return_nil(vm); }
  be_pushstring(vm, C->app->pasta);
  be_return(vm);
}

// lista(pasta [, filtro]) - [{nome, tipo, pasta, bytes, data, caminho}] of
// ANY visible folder (absolute; relative = inside the app's folder), folders
// first. 'filtro': ".mp3;.wav". nil = disk absent or a folder that will not open.
#define ARQ_LISTA_MAX 256
static int a_lista(bvm* vm)
{
  char pasta[TAB5_ARQ_CAMINHO_MAX];
  if (!arq_qualquer(vm, arg_txt(vm, 1), pasta, sizeof(pasta))) { be_return_nil(vm); }
  tab5_arq_item_t* it = (tab5_arq_item_t*)heap_caps_malloc(sizeof(tab5_arq_item_t) * ARQ_LISTA_MAX, MALLOC_CAP_SPIRAM);
  if (!it) { be_return_nil(vm); }
  const char* filtro = be_top(vm) >= 2 ? arg_txt(vm, 2) : nullptr;
  const int n = tab5_arq_listar(pasta, it, ARQ_LISTA_MAX, nullptr, TAB5_ARQ_ORDEM_NOME, filtro);
  if (n < 0) { heap_caps_free(it); be_return_nil(vm); }
  lista_nova(vm);
  for (int i = 0; i < n; ++i) {
    char cam[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
    snprintf(cam, sizeof(cam), "%s/%s", pasta, it[i].nome);
    mapa_novo(vm);
    m_txt(vm, "nome", it[i].nome);
    m_txt(vm, "tipo", arq_tipo_txt(it[i].tipo));
    m_bool(vm, "pasta", it[i].tipo == TAB5_ARQ_PASTA);
    m_int(vm, "bytes", it[i].bytes > 0x7FFFFFFF ? 0x7FFFFFFF : (int)it[i].bytes);
    m_int(vm, "data", (int)it[i].data);
    m_txt(vm, "caminho", cam);
    fim(vm);
    be_data_push(vm, -2);
    be_pop(vm, 1);
  }
  fim(vm);
  heap_caps_free(it);
  be_return(vm);
}

// ler(caminho) - the content as text (up to 64 KB), ONLY inside the app's folder
// ("sub/x.txt"). For what is outside: copiar() first.
static int a_ler(bvm* vm)
{
  char cam[TAB5_ARQ_CAMINHO_MAX];
  if (!arq_meu(vm, arg_txt(vm, 1), cam, sizeof(cam)) || !tab5_sd_montar()) { be_return_nil(vm); }
  FILE* f = fopen(cam, "rb");
  char* buf = nullptr;
  long n = 0;
  if (f) {
    n = tab5_sd_tamanho(f);
    if (n > LER_MAX) { n = LER_MAX; }
    buf = n >= 0 ? (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : nullptr;
    if (buf) { n = (long)fread(buf, 1, n, f); }
    fclose(f);
  }
  tab5_sd_desmontar();
  if (!buf) { be_return_nil(vm); }
  be_pushnstring(vm, buf, (size_t)n);
  heap_caps_free(buf);
  be_return(vm);
}

// copiar(origem [, subpasta]) - brings a file or folder (or a list of them,
// up to 64) from ANY disk into the app's folder (or an existing subfolder
// of it). In the background: follow it with estado() and '# avisos: arquivos'.
// true = started; otherwise, the text of the reason ("ja ha uma copia em andamento").
static int a_copiar(bvm* vm)
{
  char destino[TAB5_ARQ_CAMINHO_MAX];
  if (!arq_meu(vm, be_top(vm) >= 2 ? arg_txt(vm, 2) : "", destino, sizeof(destino))) { be_return_nil(vm); }
  // The sources: a text or a list of texts.
  // There is only one Berry task: a static is enough, and in PSRAM (16 KB).
  EXT_RAM_BSS_ATTR static char origens[TAB5_ARQ_ITENS_MAX][TAB5_ARQ_CAMINHO_MAX];
  const char* ptrs[TAB5_ARQ_ITENS_MAX];
  int n = 0;
  if (be_top(vm) >= 1 && be_islistinstance(vm, 1)) {
    be_getmember(vm, 1, ".p");
    const int total = be_data_size(vm, -1);
    for (int i = 0; i < total && n < TAB5_ARQ_ITENS_MAX; ++i) {
      be_pushint(vm, i);
      be_getindex(vm, -2);
      const char* c = be_isstring(vm, -1) ? be_tostring(vm, -1) : "";
      if (c[0]) { arq_qualquer(vm, c, origens[n], sizeof(origens[n])); ptrs[n] = origens[n]; ++n; }
      be_pop(vm, 2);
    }
    be_pop(vm, 1);
  } else if (be_top(vm) >= 1) {
    arq_qualquer(vm, arg_txt(vm, 1), origens[0], sizeof(origens[0]));
    ptrs[0] = origens[0];
    n = 1;
  }
  const tab5_arq_erro_t r = n ? tab5_arq_copiar(ptrs, n, destino, false) : TAB5_ARQ_ERRO_NOME;
  if (r == TAB5_ARQ_OK) {
    snprintf(s_arq_dono, sizeof(s_arq_dono), "%s", C->app->nome);
    be_pushbool(vm, true);
  } else {
    be_pushstring(vm, tab5_arq_erro_texto(r));
  }
  be_return(vm);
}

// estado() - {fase, pct, itens, feitos, atual, erro, meu}. fase: "parado",
// "contando", "copiando", "apagando", "feito", "falhou", "cancelado".
// 'meu' = it was this app that started it.
static int a_estado(bvm* vm)
{
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  static const char* const FASE[] = { "parado", "contando", "copiando", "apagando", "feito", "falhou", "cancelado" };
  const int pct = t.bytes ? (int)(t.bytes_feitos * 100 / t.bytes) : t.itens ? t.itens_feitos * 100 / t.itens : 0;
  mapa_novo(vm);
  m_txt(vm, "fase", FASE[t.fase <= TAB5_ARQ_CANCELADO ? t.fase : 0]);
  m_int(vm, "pct", pct > 100 ? 100 : pct);
  m_int(vm, "itens", t.itens);
  m_int(vm, "feitos", t.itens_feitos);
  m_txt(vm, "atual", t.atual);
  m_txt(vm, "erro", t.erro);
  m_bool(vm, "meu", C->app && !strcmp(s_arq_dono, C->app->nome));
  fim(vm);
  be_return(vm);
}

// cancelar() - only the copy THIS app started.
static int a_cancelar(bvm* vm)
{
  if (C->app && !strcmp(s_arq_dono, C->app->nome)) { tab5_arq_cancelar(); }
  be_return_nil(vm);
}

// apagar(caminho), renomear(caminho, novo_nome), criar_pasta(nome_ou_sub/nome):
// ONLY inside the app's folder. Deleting goes to the card's recycle bin.
static int a_apagar(bvm* vm)
{
  char c[TAB5_ARQ_CAMINHO_MAX];
  const char* rel = arg_txt(vm, 1);
  if (!rel[0]) { be_raise(vm, "arquivos_error", tr("apagar(caminho): o que apagar?")); be_return_nil(vm); }
  if (!arq_meu(vm, rel, c, sizeof(c))) { be_return_nil(vm); }
  be_pushbool(vm, tab5_arq_apagar(c) == TAB5_ARQ_OK);
  be_return(vm);
}

static int a_renomear(bvm* vm)
{
  char c[TAB5_ARQ_CAMINHO_MAX];
  const char* rel = arg_txt(vm, 1);
  if (!rel[0]) { be_raise(vm, "arquivos_error", "renomear(caminho, nome): o que renomear?"); be_return_nil(vm); }
  if (!arq_meu(vm, rel, c, sizeof(c))) { be_return_nil(vm); }
  be_pushbool(vm, tab5_arq_renomear(c, arg_txt(vm, 2)) == TAB5_ARQ_OK);
  be_return(vm);
}

static int a_criar_pasta(bvm* vm)
{
  char c[TAB5_ARQ_CAMINHO_MAX];
  const char* rel = arg_txt(vm, 1);
  if (!arq_meu(vm, rel, c, sizeof(c))) { be_return_nil(vm); }
  char* b = strrchr(c, '/');
  if (!rel[0] || !b) { be_raise(vm, "arquivos_error", "criar_pasta(nome): que nome?"); be_return_nil(vm); }
  *b = '\0';
  be_pushbool(vm, tab5_arq_criar_pasta(c, b + 1) == TAB5_ARQ_OK);
  be_return(vm);
}

const membro_t BERRY_MOD_CARTAO[] = {
  { "ler", c_ler }, { "gravar", c_gravar }, { "anexar", c_anexar }, { "existe", c_existe },
  { "lista", c_lista },
  { nullptr },
};

const membro_t BERRY_MOD_ARQUIVOS[] = {
  { "volumes", a_volumes }, { "pasta", a_pasta }, { "lista", a_lista }, { "ler", a_ler },
  { "copiar", a_copiar }, { "estado", a_estado }, { "cancelar", a_cancelar },
  { "apagar", a_apagar }, { "renomear", a_renomear }, { "criar_pasta", a_criar_pasta },
  { nullptr },
};
