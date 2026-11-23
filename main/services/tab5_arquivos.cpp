#include "tab5_arquivos.hpp"
#include "tab5_volumes.hpp"
#include "tab5_imagem.hpp"
#include "tab5_sd.hpp"
#include "tab5_ui.hpp"   // tab5_bytes_txt
#include "notificacao.h"
#include "ajustes.h"
#include "app.h"
#include "idioma.h"
#include "pool_tarefas.h"
#include "tab5_mutex.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_vfs_fat.h"

#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* TAG = "arquivos";

// Bloco de copia: o mesmo tamanho que mede 4,6 MB/s lendo do cartao.
#define BLOCO            (64 * 1024)
// Pastas dentro de pastas: alem disso o resto e pulado (e avisado no log).
#define PROFUNDIDADE     8
// De quanto em quanto o progresso acorda a interface e a barra.
#define AVISO_US         (250 * 1000)
// O que sobra no destino alem dos bytes (o FAT gasta por cluster e diretorio).
#define FOLGA_ESPACO     (1024 * 1024)
// Caminhos da recursao: cabem os mais longos que o FAT aceita aqui (LFN 128
// por nome, varios niveis).
#define CAMINHO_TRAB     512

#define APPS_RAIZ   TAB5_SD_APPS
#define BERRY_RAIZ  TAB5_SD_APPS "/Berry"

// ---------------------------------------------------------------------------
// CAMINHOS
// ---------------------------------------------------------------------------
static bool comeca(const char* c, const char* prefixo)
{
  const size_t n = strlen(prefixo);
  return c && !strncmp(c, prefixo, n) && (c[n] == '\0' || c[n] == '/');
}

static const char* nome_de(const char* c)
{
  const char* b = strrchr(c, '/');
  return b ? b + 1 : c;
}

static void pai_de(const char* c, char* out, size_t tam)
{
  snprintf(out, tam, "%s", c);
  char* b = strrchr(out, '/');
  if (b && b != out) { *b = '\0'; }
}

static const char* raiz_de(const char* c)
{
  switch (tab5_volume_de(c)) {
    case TAB5_VOL_CARTAO:   return TAB5_VOL_CARTAO_RAIZ;
    case TAB5_VOL_PENDRIVE: return TAB5_VOL_PENDRIVE_RAIZ;
    default:                return nullptr;
  }
}

static bool eh_raiz(const char* c)
{
  const char* r = raiz_de(c);
  return r && !strcmp(c, r);
}

static bool eh_pasta(const char* c)
{
  struct stat st;
  return stat(c, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool existe(const char* c)
{
  struct stat st;
  return stat(c, &st) == 0;
}

// ---------------------------------------------------------------------------
// PERMISSOES
// ---------------------------------------------------------------------------
// O que os sistemas deixam nos discos e ninguem quer ver. O '.' cobre a nossa
// .Lixeira, o "." e o ".." (sem subir de pasta por nome) e o lixo do Mac.
static bool nome_escondido(const char* n)
{
  return n[0] == '.' || !strcasecmp(n, "System Volume Information") || !strcasecmp(n, "$RECYCLE.BIN");
}

// Arquivos com senha (cifrada, mas ainda assim) que o sistema guarda em apps/.
static const char* const SIGILOSOS[] = { "wifi.txt", "wifi_no.txt" };

bool tab5_arq_visivel(const char* c)
{
  const char* r = raiz_de(c);
  if (!r) { return false; }
  // Cada parte do caminho depois da raiz.
  const char* p = c + strlen(r);
  while (*p) {
    while (*p == '/') { ++p; }
    if (!*p) { break; }
    const char* fim = strchr(p, '/');
    const size_t n = fim ? (size_t)(fim - p) : strlen(p);
    char parte[TAB5_ARQ_NOME_MAX];
    const size_t k = n < sizeof(parte) - 1 ? n : sizeof(parte) - 1;
    memcpy(parte, p, k);
    parte[k] = '\0';
    if (nome_escondido(parte)) { return false; }
    p += n;
  }
  if (comeca(c, APPS_RAIZ)) {
    const char* n = nome_de(c);
    for (const char* s : SIGILOSOS) { if (!strcasecmp(n, s)) { return false; } }
  }
  return true;
}

bool tab5_arq_gravavel(const char* pasta)
{
  if (!tab5_arq_visivel(pasta)) { return false; }
  return !comeca(pasta, APPS_RAIZ) || comeca(pasta, BERRY_RAIZ);
}

bool tab5_arq_mexivel(const char* c)
{
  if (!tab5_arq_visivel(c) || eh_raiz(c)) { return false; }
  if (!strcmp(c, APPS_RAIZ) || !strcmp(c, BERRY_RAIZ)) { return false; }
  char pai[TAB5_ARQ_CAMINHO_MAX];
  pai_de(c, pai, sizeof(pai));
  return tab5_arq_gravavel(pai);
}

bool tab5_arq_nome_valido(const char* n)
{
  if (!n || !n[0] || n[0] == '.' || strlen(n) >= TAB5_ARQ_NOME_MAX) { return false; }
  for (const unsigned char* p = (const unsigned char*)n; *p; ++p) {
    if (*p < 0x20 || strchr("/\\:*?\"<>|", *p)) { return false; }
  }
  const char u = n[strlen(n) - 1];
  return u != ' ' && u != '.';   // o Windows nao abre nome que termina assim
}

// ---------------------------------------------------------------------------
// TIPOS
// ---------------------------------------------------------------------------
static bool ext_em(const char* ponto, const char* const* lista)
{
  for (; *lista; ++lista) { if (!strcasecmp(ponto, *lista)) { return true; } }
  return false;
}

bool tab5_arq_extensao_e(const char* nome, const char* ext)
{
  const char* ponto = strrchr(nome, '.');
  return ponto && ponto != nome && !strcasecmp(ponto, ext);
}

tab5_arq_tipo_t tab5_arq_tipo(const char* nome)
{
  static const char* const MUSICA[] = { ".mp3", ".wav", nullptr };
  static const char* const TEXTO[]  = { ".txt", ".csv", ".log", ".json", ".md", ".m3u", ".gpx", ".nmea", nullptr };
  const char* ponto = strrchr(nome, '.');
  if (!ponto || ponto == nome) { return TAB5_ARQ_OUTRO; }
  if (ext_em(ponto, MUSICA)) { return TAB5_ARQ_MUSICA; }
  if (tab5_imagem_suportada(nome)) { return TAB5_ARQ_IMAGEM; }   // as extensoes de imagem sao do servico
  if (ext_em(ponto, TEXTO))  { return TAB5_ARQ_TEXTO; }
  if (!strcasecmp(ponto, ".be")) { return TAB5_ARQ_BERRY; }
  return TAB5_ARQ_OUTRO;
}

// ---------------------------------------------------------------------------
// LISTAR
// ---------------------------------------------------------------------------
static bool passa_filtro(const char* nome, const char* filtro)
{
  if (!filtro || !filtro[0]) { return true; }
  const char* ponto = strrchr(nome, '.');
  if (!ponto) { return false; }
  const size_t le = strlen(ponto);
  for (const char* p = filtro; *p;) {
    const char* fim = strchr(p, ';');
    const size_t n = fim ? (size_t)(fim - p) : strlen(p);
    if (n == le && !strncasecmp(p, ponto, n)) { return true; }
    p += n;
    if (*p == ';') { ++p; }
  }
  return false;
}

// Tres comparadores em vez de um com a ordem numa estatica: listar e chamado
// de mais de uma tarefa (o worker do app, o Berry).
static int pastas_antes(const tab5_arq_item_t* a, const tab5_arq_item_t* b)
{
  const bool pa = a->tipo == TAB5_ARQ_PASTA, pb = b->tipo == TAB5_ARQ_PASTA;
  return pa == pb ? 0 : (pa ? -1 : 1);
}
static int por_nome(const void* x, const void* y)
{
  const tab5_arq_item_t* a = (const tab5_arq_item_t*)x;
  const tab5_arq_item_t* b = (const tab5_arq_item_t*)y;
  const int p = pastas_antes(a, b);
  return p ? p : strcasecmp(a->nome, b->nome);
}
static int por_nome_inversa(const void* x, const void* y)
{
  const tab5_arq_item_t* a = (const tab5_arq_item_t*)x;
  const tab5_arq_item_t* b = (const tab5_arq_item_t*)y;
  const int p = pastas_antes(a, b);
  return p ? p : strcasecmp(b->nome, a->nome);
}
static int por_data(const void* x, const void* y)
{
  const tab5_arq_item_t* a = (const tab5_arq_item_t*)x;
  const tab5_arq_item_t* b = (const tab5_arq_item_t*)y;
  const int p = pastas_antes(a, b);
  if (p) { return p; }
  if (a->data != b->data) { return a->data > b->data ? -1 : 1; }
  return strcasecmp(a->nome, b->nome);
}

int tab5_arq_listar(const char* pasta, tab5_arq_item_t* out, int max, int* total,
                    tab5_arq_ordem_t ordem, const char* filtro)
{
  if (total) { *total = 0; }
  if (!tab5_arq_visivel(pasta) || !tab5_volume_montar(pasta)) { return -1; }
  // O que o gravador do cartao ainda segura tem que aparecer (tamanho, arquivo novo).
  if (tab5_volume_de(pasta) == TAB5_VOL_CARTAO) { tab5_sd_gravar_pendentes(); }
  DIR* d = opendir(pasta);
  if (!d) { tab5_volume_desmontar(pasta); return -1; }
  int n = 0, achados = 0;
  char cheio[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (nome_escondido(e->d_name)) { continue; }
    const bool dir = e->d_type == DT_DIR;
    if (!dir && !passa_filtro(e->d_name, filtro)) { continue; }
    snprintf(cheio, sizeof(cheio), "%s/%s", pasta, e->d_name);
    if (!dir && !tab5_arq_visivel(cheio)) { continue; }
    ++achados;
    if (n >= max) { continue; }
    tab5_arq_item_t* it = &out[n++];
    snprintf(it->nome, sizeof(it->nome), "%s", e->d_name);
    it->tipo  = dir ? TAB5_ARQ_PASTA : tab5_arq_tipo(e->d_name);
    it->bytes = 0;
    it->data  = 0;
    // stat le so a entrada de diretorio (nao abre o arquivo).
    struct stat st;
    if (stat(cheio, &st) == 0) {
      if (!dir && st.st_size > 0) { it->bytes = (uint64_t)st.st_size; }
      it->data = (int64_t)st.st_mtime;
    }
  }
  closedir(d);
  tab5_volume_desmontar(pasta);
  if (n > 1) {
    qsort(out, n, sizeof(tab5_arq_item_t),
          ordem == TAB5_ARQ_ORDEM_DATA ? por_data
          : ordem == TAB5_ARQ_ORDEM_NOME_INVERSA ? por_nome_inversa : por_nome);
  }
  if (total) { *total = achados; }
  return n;
}

int tab5_arq_percorrer(const char* pasta, tab5_arq_visita_t visita, void* ctx)
{
  if (!visita || !tab5_arq_visivel(pasta) || !tab5_volume_montar(pasta)) { return -1; }
  if (tab5_volume_de(pasta) == TAB5_VOL_CARTAO) { tab5_sd_gravar_pendentes(); }
  DIR* d = opendir(pasta);
  if (!d) { tab5_volume_desmontar(pasta); return -1; }
  int n = 0;
  tab5_arq_item_t it;
  char cheio[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (nome_escondido(e->d_name)) { continue; }
    const bool dir = e->d_type == DT_DIR;
    if (!dir) {
      snprintf(cheio, sizeof(cheio), "%s/%s", pasta, e->d_name);
      if (!tab5_arq_visivel(cheio)) { continue; }
    }
    snprintf(it.nome, sizeof(it.nome), "%s", e->d_name);
    it.tipo  = dir ? TAB5_ARQ_PASTA : tab5_arq_tipo(e->d_name);
    it.bytes = 0;
    it.data  = 0;
    ++n;
    if (!visita(&it, ctx)) { break; }
  }
  closedir(d);
  tab5_volume_desmontar(pasta);
  return n;
}

typedef struct { const char* formato; uint32_t maior; } maior_ctx_t;

static bool visita_maior(const tab5_arq_item_t* it, void* ctx)
{
  maior_ctx_t* m = (maior_ctx_t*)ctx;
  unsigned long n;
  if (it->tipo != TAB5_ARQ_PASTA && sscanf(it->nome, m->formato, &n) == 1 && (uint32_t)n > m->maior) {
    m->maior = (uint32_t)n;
  }
  return true;
}

uint32_t tab5_arq_maior_numero(const char* pasta, const char* formato)
{
  maior_ctx_t m = { formato, 0 };
  tab5_arq_percorrer(pasta, visita_maior, &m);
  return m.maior;
}

// ---------------------------------------------------------------------------
// O ESTADO DO TRABALHO
// ---------------------------------------------------------------------------
static Trava s_trava;   // s_t
static tab5_arq_trabalho_t s_t = { };
static std::atomic<bool>   s_ocupado{false};
static std::atomic<bool>   s_cancelar{false};
static int                 s_notif = -1;
static int64_t             s_inicio_us = 0;
static int64_t             s_aviso_us = 0;

// A barra e a interface: "Copiando 45%". 'forcar' = agora, sem esperar o passo.
static void publicar(bool forcar)
{
  const int64_t agora = esp_timer_get_time();
  if (!forcar && agora - s_aviso_us < AVISO_US) { return; }
  s_aviso_us = agora;
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  if (s_notif < 0) {
    s_notif = notif_registrar("Arquivos");
    if (s_notif >= 0) { notif_associar_app(s_notif, "Arquivos"); }
  }
  if (s_notif >= 0) {
    char r[NOTIF_RESUMO];
    const int pct = t.bytes ? (int)(t.bytes_feitos * 100 / t.bytes)
                  : t.itens ? t.itens_feitos * 100 / t.itens : 0;
    switch (t.fase) {
      case TAB5_ARQ_CONTANDO:
      case TAB5_ARQ_COPIANDO:
        snprintf(r, sizeof(r), "%s %d%%", t.mover ? "Movendo" : "Copiando", pct);
        notif_publicar(s_notif, 1, NOTIF_ESTADO, r);
        break;
      case TAB5_ARQ_APAGANDO:
        notif_publicar(s_notif, 1, NOTIF_ESTADO, t.mover ? tr("Movendo: limpando a origem") : tr("Esvaziando a lixeira"));
        break;
      case TAB5_ARQ_FEITO:
        snprintf(r, sizeof(r), tr("%s: %d arquivo(s)"), t.mover ? tr("Movido") : tr("Copiado"), t.itens_feitos);
        notif_publicar(s_notif, 1, NOTIF_INFO, r);
        break;
      case TAB5_ARQ_FALHOU:
        notif_publicar(s_notif, 1, NOTIF_ATENCAO, t.mover ? tr("Mover falhou") : tr("Copia falhou"));
        break;
      default:
        notif_limpar(s_notif);
        break;
    }
  }
  sistema_avisar(AVISO_ARQUIVOS);
}

static void mudar_fase(tab5_arq_fase_t f)
{
  s_trava.travar();
  s_t.fase = f;
  ++s_t.versao;
  s_trava.destravar();
  publicar(true);
}

static void anotar_erro(const char* fmt, const char* detalhe)
{
  char msg[sizeof(s_t.erro)];
  snprintf(msg, sizeof(msg), fmt, detalhe ? detalhe : "");
  s_trava.travar();
  if (!s_t.erro[0]) { snprintf(s_t.erro, sizeof(s_t.erro), "%s", msg); }   // o primeiro erro e a causa
  s_trava.destravar();
  ESP_LOGW(TAG, "%s", msg);
}

// Um pedaco andou: 'bytes' copiados e/ou um arquivo terminado.
static void passo(uint64_t bytes, bool arquivo, const char* atual)
{
  const int64_t ms = (esp_timer_get_time() - s_inicio_us) / 1000;
  s_trava.travar();
  s_t.bytes_feitos += bytes;
  if (arquivo) { ++s_t.itens_feitos; }
  if (atual) { snprintf(s_t.atual, sizeof(s_t.atual), "%s", atual); }
  s_t.kbps = ms > 0 ? (uint32_t)(s_t.bytes_feitos * 1000 / (uint64_t)ms / 1024) : 0;
  ++s_t.versao;
  s_trava.destravar();
  publicar(false);
}

void tab5_arq_trabalho(tab5_arq_trabalho_t* out)
{
  s_trava.travar();
  *out = s_t;
  s_trava.destravar();
}

bool tab5_arq_ocupado(void) { return s_ocupado.load(); }

void tab5_arq_cancelar(void) { if (s_ocupado.load()) { s_cancelar = true; } }

void tab5_arq_trabalho_visto(void)
{
  if (s_ocupado.load()) { return; }
  s_trava.travar();
  const bool fim = s_t.fase == TAB5_ARQ_FEITO || s_t.fase == TAB5_ARQ_FALHOU || s_t.fase == TAB5_ARQ_CANCELADO;
  if (fim) { s_t.fase = TAB5_ARQ_PARADO; ++s_t.versao; }
  s_trava.destravar();
  if (fim && s_notif >= 0) { notif_limpar(s_notif); }
}

// ---------------------------------------------------------------------------
// OPERACOES RAPIDAS
// ---------------------------------------------------------------------------
const char* tab5_arq_erro_texto(tab5_arq_erro_t e)
{
  switch (e) {
    case TAB5_ARQ_OK:             return "ok";
    case TAB5_ARQ_ERRO_PROTEGIDO: return N_("área protegida do sistema");
    case TAB5_ARQ_ERRO_NOME:      return N_("nome inválido");
    case TAB5_ARQ_ERRO_EXISTE:    return N_("já existe com esse nome");
    case TAB5_ARQ_ERRO_DISCO:     return N_("o disco não está disponível");
    case TAB5_ARQ_ERRO_OCUPADO:   return N_("já há uma cópia em andamento");
    case TAB5_ARQ_ERRO_ESPACO:    return N_("não cabe no destino");
    case TAB5_ARQ_ERRO_DENTRO:    return N_("uma pasta não vai para dentro dela mesma");
    default:                      return N_("o disco recusou");
  }
}

// O primeiro nome livre em 'pasta': "x.mp3", "x (2).mp3", "x (3).mp3"...
// Pasta nao tem extensao: "Rock (2)".
static bool nome_livre(const char* pasta, const char* nome, bool e_pasta, char* out, size_t tam)
{
  snprintf(out, tam, "%s/%s", pasta, nome);
  if (!existe(out)) { return true; }
  const char* ponto = e_pasta ? nullptr : strrchr(nome, '.');
  if (ponto == nome) { ponto = nullptr; }
  const int base = ponto ? (int)(ponto - nome) : (int)strlen(nome);
  for (int k = 2; k < 100; ++k) {
    snprintf(out, tam, "%s/%.*s (%d)%s", pasta, base, nome, k, ponto ? ponto : "");
    if (!existe(out)) { return true; }
  }
  return false;
}

tab5_arq_erro_t tab5_arq_criar_pasta(const char* pasta, const char* nome)
{
  if (!tab5_arq_gravavel(pasta)) { return TAB5_ARQ_ERRO_PROTEGIDO; }
  if (!tab5_arq_nome_valido(nome)) { return TAB5_ARQ_ERRO_NOME; }
  if (!tab5_volume_montar(pasta)) { return TAB5_ARQ_ERRO_DISCO; }
  char c[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
  snprintf(c, sizeof(c), "%s/%s", pasta, nome);
  tab5_arq_erro_t r = TAB5_ARQ_OK;
  if (existe(c))               { r = TAB5_ARQ_ERRO_EXISTE; }
  else if (mkdir(c, 0775) != 0) { r = TAB5_ARQ_ERRO_FALHOU; ESP_LOGW(TAG, "mkdir %s: %d", c, errno); }
  tab5_volume_desmontar(pasta);
  return r;
}

tab5_arq_erro_t tab5_arq_novo_abrir(tab5_arq_novo_t* a, const char* pasta, const char* nome)
{
  a->fd = -1;
  if (!tab5_arq_gravavel(pasta)) { return TAB5_ARQ_ERRO_PROTEGIDO; }
  if (!tab5_arq_nome_valido(nome)) { return TAB5_ARQ_ERRO_NOME; }
  if (!tab5_volume_montar(pasta)) { return TAB5_ARQ_ERRO_DISCO; }
  snprintf(a->pasta, sizeof(a->pasta), "%s", pasta);
  if (!eh_pasta(pasta)) { tab5_volume_desmontar(pasta); return TAB5_ARQ_ERRO_DISCO; }
  if (!nome_livre(pasta, nome, false, a->caminho, sizeof(a->caminho))) { tab5_volume_desmontar(pasta); return TAB5_ARQ_ERRO_EXISTE; }
  a->fd = open(a->caminho, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (a->fd < 0) { ESP_LOGW(TAG, "criar %s: %d", a->caminho, errno); tab5_volume_desmontar(pasta); return TAB5_ARQ_ERRO_FALHOU; }
  return TAB5_ARQ_OK;   // o disco fica seguro ate tab5_arq_novo_fechar
}

bool tab5_arq_novo_escrever(tab5_arq_novo_t* a, const void* dados, size_t n)
{
  if (a->fd < 0) { return false; }
  const uint8_t* p = (const uint8_t*)dados;
  while (n) {
    const ssize_t k = write(a->fd, p, n);
    if (k <= 0) { return false; }   // disco cheio ou saiu
    p += k;
    n -= (size_t)k;
  }
  return true;
}

tab5_arq_erro_t tab5_arq_novo_fechar(tab5_arq_novo_t* a, bool manter)
{
  if (a->fd < 0) { return TAB5_ARQ_ERRO_FALHOU; }
  const bool fechou = close(a->fd) == 0;
  a->fd = -1;
  if (!manter || !fechou) { unlink(a->caminho); }
  tab5_volume_desmontar(a->pasta);
  return (manter && fechou) ? TAB5_ARQ_OK : TAB5_ARQ_ERRO_FALHOU;
}

tab5_arq_erro_t tab5_arq_renomear(const char* caminho, const char* novo)
{
  if (!tab5_arq_mexivel(caminho)) { return TAB5_ARQ_ERRO_PROTEGIDO; }
  if (!tab5_arq_nome_valido(novo)) { return TAB5_ARQ_ERRO_NOME; }
  if (!tab5_volume_montar(caminho)) { return TAB5_ARQ_ERRO_DISCO; }
  char pai[TAB5_ARQ_CAMINHO_MAX];
  pai_de(caminho, pai, sizeof(pai));
  char c[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
  snprintf(c, sizeof(c), "%s/%s", pai, novo);
  tab5_arq_erro_t r = TAB5_ARQ_OK;
  // So trocar maiuscula ("rock" -> "Rock"): o FAT acha o mesmo item, e vale.
  const bool mesmo = !strcasecmp(nome_de(caminho), novo);
  if (!strcmp(nome_de(caminho), novo))    { r = TAB5_ARQ_OK; }
  else if (!mesmo && existe(c))           { r = TAB5_ARQ_ERRO_EXISTE; }
  else if (rename(caminho, c) != 0)       { r = TAB5_ARQ_ERRO_FALHOU; ESP_LOGW(TAG, "rename %s: %d", caminho, errno); }
  tab5_volume_desmontar(caminho);
  return r;
}

// ---------------------------------------------------------------------------
// O INDICE DA LIXEIRA
// ---------------------------------------------------------------------------
// <raiz>/.Lixeira/.indice, texto, uma linha por item:
//   nome_na_lixeira<TAB>caminho_original<TAB>epoch_segundos
// Epoch 0 = o relogio estava sem hora certa. Escondido como a propria lixeira.
#define INDICE_NOME      ".indice"
#define INDICE_MAX       (128 * 1024)   // o que se le de uma vez (cerca de 400 itens)
#define EPOCH_VALIDA     1700000000     // abaixo disso o relogio nao tinha hora certa

static void indice_caminho(const char* raiz, char* out, size_t tam)
{
  snprintf(out, tam, "%s/" TAB5_ARQ_LIXEIRA "/" INDICE_NOME, raiz);
}

// O indice inteiro, terminado em '\0', na PSRAM (quem chama libera com
// heap_caps_free). nullptr = nao ha indice.
static char* indice_ler(const char* raiz, size_t* tam)
{
  char c[80];
  indice_caminho(raiz, c, sizeof(c));
  const int fd = open(c, O_RDONLY);
  if (fd < 0) { return nullptr; }
  struct stat st;
  size_t n = (fstat(fd, &st) == 0 && st.st_size > 0) ? (size_t)st.st_size : 0;
  if (n > INDICE_MAX) { n = INDICE_MAX; }
  char* b = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t lido = 0;
  while (b && lido < n) {
    const ssize_t k = read(fd, b + lido, n - lido);
    if (k <= 0) { break; }
    lido += (size_t)k;
  }
  close(fd);
  if (!b) { return nullptr; }
  b[lido] = '\0';
  if (tam) { *tam = lido; }
  return b;
}

// Acha a linha de 'nome' em 'indice'; devolve o inicio dela ou nullptr.
static const char* indice_linha(const char* indice, const char* nome)
{
  const size_t ln = strlen(nome);
  for (const char* p = indice; p && *p;) {
    if (!strncmp(p, nome, ln) && p[ln] == '\t') { return p; }
    const char* f = strchr(p, '\n');
    p = f ? f + 1 : nullptr;
  }
  return nullptr;
}

// Tira do indice as linhas de 'nome' (apaga o arquivo se nao sobrar nenhuma).
static void indice_tirar(const char* raiz, const char* nome)
{
  size_t n = 0;
  char* b = indice_ler(raiz, &n);
  if (!b) { return; }
  if (indice_linha(b, nome)) {
    const size_t ln = strlen(nome);
    size_t w = 0;
    for (size_t r = 0; r < n;) {
      const char* f = (const char*)memchr(b + r, '\n', n - r);
      const size_t len = f ? (size_t)(f - (b + r)) + 1 : n - r;
      if (!(len > ln && !strncmp(b + r, nome, ln) && b[r + ln] == '\t')) {
        memmove(b + w, b + r, len);
        w += len;
      }
      r += len;
    }
    char c[80];
    indice_caminho(raiz, c, sizeof(c));
    if (w == 0) {
      unlink(c);
    } else {
      const int fd = open(c, O_WRONLY | O_TRUNC);
      if (fd >= 0) {
        size_t e = 0;
        while (e < w) {
          const ssize_t k = write(fd, b + e, w - e);
          if (k <= 0) { break; }
          e += (size_t)k;
        }
        close(fd);
      }
    }
  }
  heap_caps_free(b);
}

// Acrescenta a linha do item que acabou de ir para a lixeira.
static void indice_acrescentar(const char* raiz, const char* nome, const char* origem)
{
  // Nome com TAB ou quebra de linha estragaria o formato: fica sem linha (origem desconhecida).
  if (strpbrk(nome, "\t\n") || strpbrk(origem, "\t\n")) { return; }
  indice_tirar(raiz, nome);   // sobra de um item que saiu da lixeira por fora
  const time_t agora = time(nullptr);
  char linha[TAB5_ARQ_NOME_MAX + TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX + 32];
  const int n = snprintf(linha, sizeof(linha), "%s\t%s\t%lu\n", nome, origem,
                         (unsigned long)(agora > EPOCH_VALIDA ? agora : 0));
  if (n <= 0 || n >= (int)sizeof(linha)) { return; }
  char c[80];
  indice_caminho(raiz, c, sizeof(c));
  const int fd = open(c, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd < 0) { ESP_LOGW(TAG, "indice da lixeira: %d", errno); return; }
  if (write(fd, linha, (size_t)n) != n) { ESP_LOGW(TAG, "indice da lixeira: escrita incompleta"); }
  close(fd);
}

tab5_arq_erro_t tab5_arq_apagar(const char* caminho)
{
  if (!tab5_arq_mexivel(caminho)) { return TAB5_ARQ_ERRO_PROTEGIDO; }
  if (!tab5_volume_montar(caminho)) { return TAB5_ARQ_ERRO_DISCO; }
  char lixeira[64];
  snprintf(lixeira, sizeof(lixeira), "%s/" TAB5_ARQ_LIXEIRA, raiz_de(caminho));
  if (!existe(lixeira)) { mkdir(lixeira, 0775); }
  char dst[TAB5_ARQ_CAMINHO_MAX + TAB5_ARQ_NOME_MAX];
  tab5_arq_erro_t r = TAB5_ARQ_OK;
  if (!existe(caminho)) {
    r = TAB5_ARQ_ERRO_FALHOU;
  } else if (!nome_livre(lixeira, nome_de(caminho), eh_pasta(caminho), dst, sizeof(dst))) {
    r = TAB5_ARQ_ERRO_EXISTE;
  } else if (rename(caminho, dst) != 0) {
    r = TAB5_ARQ_ERRO_FALHOU;
    ESP_LOGW(TAG, "para a lixeira %s: %d", caminho, errno);
  }
  if (r == TAB5_ARQ_OK) { indice_acrescentar(raiz_de(caminho), nome_de(dst), caminho); }
  tab5_volume_desmontar(caminho);
  return r;
}

// ---------------------------------------------------------------------------
// O TRABALHO DE FUNDO
// ---------------------------------------------------------------------------
typedef enum { OP_COPIAR, OP_ESVAZIAR } op_t;

typedef struct {
  op_t     op;
  bool     mover;
  uint32_t area_geracao;   // recortar+colar: a area que deu origem (0 = nao veio dela)
  int      n;
  char     origens[TAB5_ARQ_ITENS_MAX][TAB5_ARQ_CAMINHO_MAX];
  char     destino[TAB5_ARQ_CAMINHO_MAX];
  bool     segura[TAB5_VOL_N];   // os discos que o trabalho segura do comeco ao fim
  char     a[CAMINHO_TRAB];      // a origem da vez (a recursao anda nela)
  char     b[CAMINHO_TRAB];      // o destino da vez
  uint8_t* buf;
} job_t;

static void soltar_discos(job_t* j)
{
  for (int v = 0; v < TAB5_VOL_N; ++v) {
    if (!j->segura[v]) { continue; }
    tab5_volume_desmontar(v == TAB5_VOL_CARTAO ? TAB5_VOL_CARTAO_RAIZ : TAB5_VOL_PENDRIVE_RAIZ);
    j->segura[v] = false;
  }
}

static bool segurar_disco(job_t* j, const char* caminho)
{
  const tab5_vol_id_t v = tab5_volume_de(caminho);
  if (v == TAB5_VOL_NENHUM) { return false; }
  if (j->segura[v]) { return true; }
  if (!tab5_volume_montar(caminho)) { return false; }
  j->segura[v] = true;
  return true;
}

// Arquivos e bytes de um item (pasta: tudo o que ha dentro e se ve).
static void contar(job_t* j, int prof, int* itens, uint64_t* bytes)
{
  struct stat st;
  if (s_cancelar.load() || stat(j->a, &st) != 0) { return; }
  if (!S_ISDIR(st.st_mode)) { ++*itens; *bytes += (uint64_t)(st.st_size > 0 ? st.st_size : 0); return; }
  if (prof >= PROFUNDIDADE) { return; }
  DIR* d = opendir(j->a);
  if (!d) { return; }
  const size_t la = strlen(j->a);
  struct dirent* e;
  while ((e = readdir(d)) != nullptr && !s_cancelar.load()) {
    if (nome_escondido(e->d_name)) { continue; }
    const size_t ln = strlen(e->d_name);
    if (la + 1 + ln >= sizeof(j->a)) { continue; }
    j->a[la] = '/';
    memcpy(j->a + la + 1, e->d_name, ln + 1);
    if (tab5_arq_visivel(j->a)) { contar(j, prof + 1, itens, bytes); }
    j->a[la] = '\0';
  }
  closedir(d);
}

static bool copiar_arquivo(job_t* j)
{
  const char* nome = nome_de(j->a);
  passo(0, false, nome);
  const int fa = open(j->a, O_RDONLY);
  if (fa < 0) { anotar_erro(tr("não abriu %s"), nome); return false; }
  const int fb = open(j->b, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fb < 0) { close(fa); anotar_erro(tr("não criou %s no destino"), nome); return false; }
  bool ok = true;
  for (;;) {
    if (s_cancelar.load()) { ok = false; break; }
    if (!tab5_volume_disponivel(j->a) || !tab5_volume_disponivel(j->b)) {
      anotar_erro(tr("o disco saiu no meio (%s)"), nome);
      ok = false;
      break;
    }
    const ssize_t r = read(fa, j->buf, BLOCO);
    if (r < 0) { anotar_erro(tr("erro lendo %s"), nome); ok = false; break; }
    if (r == 0) { break; }
    ssize_t w = 0;
    while (w < r) {
      const ssize_t k = write(fb, j->buf + w, (size_t)(r - w));
      if (k <= 0) { anotar_erro(tr("erro gravando %s (disco cheio?)"), nome); ok = false; break; }
      w += k;
    }
    if (!ok) { break; }
    passo((uint64_t)r, false, nullptr);
    // O cartao e um so: entre os blocos o tocador e o gravador do sistema leem.
    vTaskDelay(1);
  }
  close(fa);
  if (close(fb) != 0 && ok) { anotar_erro(tr("erro fechando %s"), nome); ok = false; }
  if (!ok) { unlink(j->b); return false; }   // nada de arquivo pela metade
  passo(0, true, nullptr);
  return true;
}

// Copia j->a para j->b (j->b ainda nao existe). Pasta: a arvore visivel.
static bool copiar_item(job_t* j, int prof)
{
  if (!eh_pasta(j->a)) { return copiar_arquivo(j); }
  if (mkdir(j->b, 0775) != 0 && errno != EEXIST) { anotar_erro(tr("não criou a pasta %s"), nome_de(j->b)); return false; }
  if (prof >= PROFUNDIDADE) { ESP_LOGW(TAG, "funda demais, pulada: %s", j->a); return true; }
  DIR* d = opendir(j->a);
  if (!d) { anotar_erro(tr("não abriu a pasta %s"), nome_de(j->a)); return false; }
  const size_t la = strlen(j->a), lb = strlen(j->b);
  bool ok = true;
  struct dirent* e;
  while (ok && (e = readdir(d)) != nullptr) {
    if (s_cancelar.load()) { ok = false; break; }
    if (nome_escondido(e->d_name)) { continue; }
    const size_t ln = strlen(e->d_name);
    if (la + 1 + ln >= sizeof(j->a) || lb + 1 + ln >= sizeof(j->b)) {
      ESP_LOGW(TAG, "caminho longo demais, pulado: %s/%s", j->a, e->d_name);
      continue;
    }
    j->a[la] = '/'; memcpy(j->a + la + 1, e->d_name, ln + 1);
    j->b[lb] = '/'; memcpy(j->b + lb + 1, e->d_name, ln + 1);
    if (tab5_arq_visivel(j->a)) { ok = copiar_item(j, prof + 1); }
    j->a[la] = '\0';
    j->b[lb] = '\0';
  }
  closedir(d);
  return ok;
}

// Apaga 'c' (arquivo ou arvore). 'so_visiveis': a origem de um mover - o
// que ficou escondido (e por isso nao foi copiado) nao e apagado, e a pasta
// que o contem fica.
// 'contar': cada arquivo apagado conta no progresso (a lixeira; no mover os
// arquivos ja contaram ao copiar).
static bool apagar_tudo(char* c, size_t cap, bool so_visiveis, bool contar, int prof)
{
  if (!eh_pasta(c)) {
    const bool ok = unlink(c) == 0;
    if (ok && contar) { passo(0, true, nome_de(c)); }
    return ok;
  }
  if (prof < 2 * PROFUNDIDADE) {
    DIR* d = opendir(c);
    if (d) {
      const size_t lc = strlen(c);
      struct dirent* e;
      while ((e = readdir(d)) != nullptr && !s_cancelar.load()) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { continue; }
        if (so_visiveis && nome_escondido(e->d_name)) { continue; }
        const size_t ln = strlen(e->d_name);
        if (lc + 1 + ln >= cap) { continue; }
        c[lc] = '/';
        memcpy(c + lc + 1, e->d_name, ln + 1);
        apagar_tudo(c, cap, so_visiveis, contar, prof + 1);
        c[lc] = '\0';
      }
      closedir(d);
    }
  }
  return rmdir(c) == 0;
}

static tab5_arq_fase_t executar_copia(job_t* j)
{
  const bool mesmo_disco_tudo = [&] {
    for (int i = 0; i < j->n; ++i) { if (tab5_volume_de(j->origens[i]) != tab5_volume_de(j->destino)) { return false; } }
    return true;
  }();
  const bool so_renomear = j->mover && mesmo_disco_tudo;

  // 1. MEDIR - para o progresso e para saber se cabe. Mover no mesmo disco
  // nao copia nada: so muda de pasta.
  int itens = 0;
  uint64_t bytes = 0;
  if (so_renomear) {
    itens = j->n;
  } else {
    // O que o gravador do cartao ainda segura tem que ir junto na copia.
    for (int i = 0; i < j->n; ++i) {
      if (tab5_volume_de(j->origens[i]) == TAB5_VOL_CARTAO) { tab5_sd_gravar_pendentes(); break; }
    }
    for (int i = 0; i < j->n && !s_cancelar.load(); ++i) {
      snprintf(j->a, sizeof(j->a), "%s", j->origens[i]);
      contar(j, 0, &itens, &bytes);
    }
    uint64_t total = 0, livre = 0;
    if (esp_vfs_fat_info(raiz_de(j->destino), &total, &livre) == ESP_OK && livre < bytes + FOLGA_ESPACO) {
      char d[64];
      snprintf(d, sizeof(d), "%.1f MB, livres %.1f MB", bytes / 1048576.0, livre / 1048576.0);
      anotar_erro(tr("não cabe: precisa de %s"), d);
      return TAB5_ARQ_FALHOU;
    }
  }
  s_trava.travar();
  s_t.itens = itens;
  s_t.bytes = bytes;
  s_trava.destravar();
  if (s_cancelar.load()) { return TAB5_ARQ_CANCELADO; }

  // 2. COPIAR (ou mudar de pasta)
  mudar_fase(TAB5_ARQ_COPIANDO);
  s_inicio_us = esp_timer_get_time();
  for (int i = 0; i < j->n; ++i) {
    if (s_cancelar.load()) { return TAB5_ARQ_CANCELADO; }
    const char* orig = j->origens[i];
    if (!existe(orig)) { anotar_erro(tr("não existe mais: %s"), nome_de(orig)); return TAB5_ARQ_FALHOU; }
    if (!nome_livre(j->destino, nome_de(orig), eh_pasta(orig), j->b, sizeof(j->b))) {
      anotar_erro("nomes repetidos demais: %s", nome_de(orig));
      return TAB5_ARQ_FALHOU;
    }
    if (so_renomear) {
      if (rename(orig, j->b) != 0) { anotar_erro(tr("não moveu %s"), nome_de(orig)); return TAB5_ARQ_FALHOU; }
      passo(0, true, nome_de(orig));
      continue;
    }
    snprintf(j->a, sizeof(j->a), "%s", orig);
    if (!copiar_item(j, 0)) { return s_cancelar.load() ? TAB5_ARQ_CANCELADO : TAB5_ARQ_FALHOU; }
    if (j->mover) {
      // Entre discos: a origem so sai depois de o item inteiro estar no destino.
      snprintf(j->a, sizeof(j->a), "%s", orig);
      if (!apagar_tudo(j->a, sizeof(j->a), true, false, 0) && existe(orig)) {
        ESP_LOGW(TAG, "a origem %s ficou (havia escondidos dentro?)", orig);
      }
    }
  }
  return TAB5_ARQ_FEITO;
}

static tab5_arq_fase_t executar_esvaziar(job_t* j)
{
  mudar_fase(TAB5_ARQ_APAGANDO);
  s_inicio_us = esp_timer_get_time();
  if (!existe(j->a)) { return TAB5_ARQ_FEITO; }
  apagar_tudo(j->a, sizeof(j->a), false, true, 0);
  return s_cancelar.load() ? TAB5_ARQ_CANCELADO : TAB5_ARQ_FEITO;
}

// A area de transferencia (definida mais abaixo) - o fim de um recortar+colar
// a esvazia.
static Trava s_area_trava;
static uint32_t          s_area_geracao = 1;   // muda a cada area_por/limpar (sob s_area_trava)
static void area_limpar_se(uint32_t geracao);

static void corpo(void* arg)
{
  job_t* j = (job_t*)arg;
  sistema_recurso(RECURSO_CARTAO, "Arquivos", true);
  const tab5_arq_fase_t fim = j->op == OP_ESVAZIAR ? executar_esvaziar(j) : executar_copia(j);
  soltar_discos(j);
  sistema_recurso(RECURSO_CARTAO, "Arquivos", false);
  if (fim == TAB5_ARQ_FEITO && j->area_geracao) {
    // Recortar + colar deu certo: a area esvazia - se ninguem pos outra coisa nela.
    area_limpar_se(j->area_geracao);
  }
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  ESP_LOGI(TAG, "%s: fase %d, %d/%d arquivo(s), %llu KB, %lu KB/s%s%s",
           j->op == OP_ESVAZIAR ? "esvaziar" : (j->mover ? "mover" : "copiar"), (int)fim,
           t.itens_feitos, t.itens, (unsigned long long)(t.bytes_feitos / 1024), (unsigned long)t.kbps,
           t.erro[0] ? " - " : "", t.erro);
  heap_caps_free(j->buf);
  heap_caps_free(j);
  s_ocupado = false;   // antes de publicar: quem acordar com o aviso ja pode pedir outro
  mudar_fase(fim);
}

// Comeca: o job ja tem os discos seguros. false = nao subiu (e tudo foi solto).
static bool comecar(job_t* j)
{
  s_trava.travar();
  s_t = tab5_arq_trabalho_t{};
  s_t.fase = TAB5_ARQ_CONTANDO;
  s_t.mover = j->mover;
  snprintf(s_t.destino, sizeof(s_t.destino), "%s", j->destino);
  s_t.versao = esp_timer_get_time() & 0x7FFFFFFF;
  s_trava.destravar();
  s_cancelar = false;
  s_inicio_us = esp_timer_get_time();
  j->buf = (uint8_t*)heap_caps_aligned_alloc(128, BLOCO, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  // Prioridade 1: abaixo do gravador do cartao (2), do tocador e da interface.
  if (!j->buf || !pool_criar(nullptr, corpo, j, "arquivos", 4096, 1)) {
    ESP_LOGE(TAG, "o trabalho nao subiu (memoria?)");
    soltar_discos(j);
    heap_caps_free(j->buf);
    heap_caps_free(j);
    s_trava.travar();
    s_t.fase = TAB5_ARQ_PARADO;
    s_trava.destravar();
    s_ocupado = false;
    return false;
  }
  publicar(true);
  return true;
}

static job_t* job_novo(void)
{
  return (job_t*)heap_caps_calloc(1, sizeof(job_t), MALLOC_CAP_SPIRAM);
}

static tab5_arq_erro_t copiar_de_area(const char* const* origens, int n, const char* destino, bool mover,
                                      uint32_t area_geracao)
{
  if (n <= 0 || n > TAB5_ARQ_ITENS_MAX || !destino) { return TAB5_ARQ_ERRO_NOME; }
  if (!tab5_arq_gravavel(destino)) { return TAB5_ARQ_ERRO_PROTEGIDO; }
  for (int i = 0; i < n; ++i) {
    const char* o = origens[i];
    if (!o || strlen(o) >= TAB5_ARQ_CAMINHO_MAX) { return TAB5_ARQ_ERRO_NOME; }
    if (mover ? !tab5_arq_mexivel(o) : (!tab5_arq_visivel(o) || eh_raiz(o))) { return TAB5_ARQ_ERRO_PROTEGIDO; }
    if (comeca(destino, o)) { return TAB5_ARQ_ERRO_DENTRO; }   // a pasta para dentro dela (ou dela mesma)
    char pai[TAB5_ARQ_CAMINHO_MAX];
    pai_de(o, pai, sizeof(pai));
    if (mover && !strcmp(pai, destino)) { return TAB5_ARQ_ERRO_EXISTE; }   // ja esta aqui
  }
  if (s_ocupado.exchange(true)) { return TAB5_ARQ_ERRO_OCUPADO; }
  job_t* j = job_novo();
  if (!j) { s_ocupado = false; return TAB5_ARQ_ERRO_FALHOU; }
  j->op = OP_COPIAR;
  j->mover = mover;
  j->area_geracao = area_geracao;
  j->n = n;
  for (int i = 0; i < n; ++i) { snprintf(j->origens[i], sizeof(j->origens[i]), "%s", origens[i]); }
  snprintf(j->destino, sizeof(j->destino), "%s", destino);
  bool discos = segurar_disco(j, destino);
  for (int i = 0; discos && i < n; ++i) { discos = segurar_disco(j, origens[i]); }
  if (!discos) {
    soltar_discos(j);
    heap_caps_free(j);
    s_ocupado = false;
    return TAB5_ARQ_ERRO_DISCO;
  }
  // Com o disco seguro, os itens tem que existir de verdade.
  for (int i = 0; i < n; ++i) {
    if (!existe(origens[i])) {
      soltar_discos(j);
      heap_caps_free(j);
      s_ocupado = false;
      return TAB5_ARQ_ERRO_FALHOU;
    }
  }
  return comecar(j) ? TAB5_ARQ_OK : TAB5_ARQ_ERRO_FALHOU;
}

tab5_arq_erro_t tab5_arq_copiar(const char* const* origens, int n, const char* destino, bool mover)
{
  return copiar_de_area(origens, n, destino, mover, 0);
}

int tab5_arq_lixeira_itens(const char* raiz)
{
  if (!eh_raiz(raiz) || !tab5_volume_montar(raiz)) { return 0; }
  char c[64];
  snprintf(c, sizeof(c), "%s/" TAB5_ARQ_LIXEIRA, raiz);
  int n = 0;
  if (DIR* d = opendir(c)) {
    while (struct dirent* e = readdir(d)) {
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, INDICE_NOME)) { ++n; }
    }
    closedir(d);
  }
  tab5_volume_desmontar(raiz);
  return n;
}

// ---------------------------------------------------------------------------
// LIXEIRA POR ITEM (listar, restaurar, excluir)
// ---------------------------------------------------------------------------
// Tamanho de um item: arquivo = o tamanho; pasta = a soma de tudo dentro.
static uint64_t tamanho_de(char* c, size_t cap, int prof)
{
  struct stat st;
  if (stat(c, &st) != 0) { return 0; }
  if (!S_ISDIR(st.st_mode)) { return (uint64_t)(st.st_size > 0 ? st.st_size : 0); }
  if (prof >= 2 * PROFUNDIDADE) { return 0; }
  DIR* d = opendir(c);
  if (!d) { return 0; }
  uint64_t soma = 0;
  const size_t lc = strlen(c);
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { continue; }
    const size_t ln = strlen(e->d_name);
    if (lc + 1 + ln >= cap) { continue; }
    c[lc] = '/';
    memcpy(c + lc + 1, e->d_name, ln + 1);
    soma += tamanho_de(c, cap, prof + 1);
    c[lc] = '\0';
  }
  closedir(d);
  return soma;
}

// Do mais recente para o mais antigo; sem data (0) no fim.
static int comparar_lixo(const void* a, const void* b)
{
  const tab5_arq_lixo_t* x = (const tab5_arq_lixo_t*)a;
  const tab5_arq_lixo_t* y = (const tab5_arq_lixo_t*)b;
  if ((x->quando == 0) != (y->quando == 0)) { return x->quando == 0 ? 1 : -1; }
  if (x->quando != y->quando) { return x->quando > y->quando ? -1 : 1; }
  return strcmp(x->nome, y->nome);
}

int tab5_arq_lixeira_listar(const char* raiz, tab5_arq_lixo_t* out, int max, uint64_t* total_bytes)
{
  if (total_bytes) { *total_bytes = 0; }
  if (!eh_raiz(raiz) || !out || max <= 0 || !tab5_volume_montar(raiz)) { return 0; }
  char c[CAMINHO_TRAB];
  snprintf(c, sizeof(c), "%s/" TAB5_ARQ_LIXEIRA, raiz);
  char* indice = indice_ler(raiz, nullptr);
  int n = 0;
  uint64_t total = 0;
  if (DIR* d = opendir(c)) {
    const size_t lc = strlen(c);
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, INDICE_NOME)) { continue; }
      const size_t ln = strlen(e->d_name);
      if (ln >= TAB5_ARQ_NOME_MAX || lc + 1 + ln >= sizeof(c)) {
        ESP_LOGW(TAG, "lixeira: nome longo demais, fora da lista: %s", e->d_name);
        continue;
      }
      c[lc] = '/';
      memcpy(c + lc + 1, e->d_name, ln + 1);
      const bool pasta = eh_pasta(c);
      const uint64_t bytes = tamanho_de(c, sizeof(c), 0);
      c[lc] = '\0';
      total += bytes;
      if (n >= max) { continue; }   // o total continua somando o que nao coube
      tab5_arq_lixo_t* it = &out[n++];
      memset(it, 0, sizeof(*it));
      memcpy(it->nome, e->d_name, ln + 1);
      it->bytes = bytes;
      it->pasta = pasta;
      if (const char* l = indice_linha(indice, e->d_name)) {
        const char* o = l + ln + 1;
        const char* t = strchr(o, '\t');
        const char* f = strchr(o, '\n');
        if (t && (!f || t < f)) {
          const size_t lo = (size_t)(t - o) < sizeof(it->origem) - 1 ? (size_t)(t - o) : sizeof(it->origem) - 1;
          memcpy(it->origem, o, lo);
          it->origem[lo] = '\0';
          const unsigned long q = strtoul(t + 1, nullptr, 10);
          it->quando = q > EPOCH_VALIDA ? (uint32_t)q : 0;
        }
      }
    }
    closedir(d);
  }
  if (indice) { heap_caps_free(indice); }
  tab5_volume_desmontar(raiz);
  qsort(out, (size_t)n, sizeof(out[0]), comparar_lixo);
  if (total_bytes) { *total_bytes = total; }
  return n;
}

// O nome de um item da lixeira tem que ser so um nome (nada de subir de pasta).
static bool nome_lixo_valido(const char* nome)
{
  return nome && nome[0] && strlen(nome) < TAB5_ARQ_NOME_MAX && !strchr(nome, '/') &&
         strcmp(nome, ".") && strcmp(nome, "..") && strcmp(nome, INDICE_NOME);
}

// Cria as pastas que faltam de 'pasta' (sempre dentro de 'raiz').
static bool criar_pastas(const char* raiz, const char* pasta)
{
  char c[TAB5_ARQ_CAMINHO_MAX];
  snprintf(c, sizeof(c), "%s", pasta);
  for (char* p = c + strlen(raiz) + 1; *p; ++p) {
    if (*p != '/') { continue; }
    *p = '\0';
    if (!existe(c) && mkdir(c, 0775) != 0) { return false; }
    *p = '/';
  }
  return eh_pasta(c) || mkdir(c, 0775) == 0;
}

tab5_arq_erro_t tab5_arq_lixeira_restaurar(const char* raiz, const char* nome)
{
  if (!eh_raiz(raiz) || !nome_lixo_valido(nome)) { return TAB5_ARQ_ERRO_NOME; }
  if (!tab5_volume_montar(raiz)) { return TAB5_ARQ_ERRO_DISCO; }
  char orig[CAMINHO_TRAB];
  snprintf(orig, sizeof(orig), "%s/" TAB5_ARQ_LIXEIRA "/%s", raiz, nome);
  tab5_arq_erro_t r = TAB5_ARQ_OK;
  if (!existe(orig)) {
    r = TAB5_ARQ_ERRO_FALHOU;
  } else {
    // Para onde volta: a origem do indice; sem ela (ou fora do disco, ou numa
    // area que nao aceita gravar) vai para a raiz do disco.
    char pasta[TAB5_ARQ_CAMINHO_MAX];
    char nome_final[TAB5_ARQ_NOME_MAX];
    snprintf(pasta, sizeof(pasta), "%s", raiz);
    snprintf(nome_final, sizeof(nome_final), "%s", nome);
    char* indice = indice_ler(raiz, nullptr);
    if (const char* l = indice_linha(indice, nome)) {
      const char* o = l + strlen(nome) + 1;
      const char* t = strchr(o, '\t');
      if (t && (size_t)(t - o) < TAB5_ARQ_CAMINHO_MAX) {
        char origem[TAB5_ARQ_CAMINHO_MAX];
        memcpy(origem, o, (size_t)(t - o));
        origem[t - o] = '\0';
        char pai[TAB5_ARQ_CAMINHO_MAX];
        pai_de(origem, pai, sizeof(pai));
        if (comeca(origem, raiz) && tab5_arq_gravavel(pai) && nome_de(origem)[0] &&
            strlen(nome_de(origem)) < sizeof(nome_final)) {
          snprintf(pasta, sizeof(pasta), "%s", pai);
          snprintf(nome_final, sizeof(nome_final), "%s", nome_de(origem));
        }
      }
    }
    if (indice) { heap_caps_free(indice); }
    char dst[CAMINHO_TRAB];
    if (!criar_pastas(raiz, pasta)) {
      r = TAB5_ARQ_ERRO_FALHOU;
    } else if (!nome_livre(pasta, nome_final, eh_pasta(orig), dst, sizeof(dst))) {
      r = TAB5_ARQ_ERRO_EXISTE;
    } else if (rename(orig, dst) != 0) {
      r = TAB5_ARQ_ERRO_FALHOU;
      ESP_LOGW(TAG, "restaurar %s: %d", orig, errno);
    }
  }
  if (r == TAB5_ARQ_OK) { indice_tirar(raiz, nome); }
  tab5_volume_desmontar(raiz);
  return r;
}

// Apaga um item (sem trava de disco: quem chama ja montou).
static tab5_arq_erro_t excluir_item(const char* raiz, const char* nome)
{
  char c[CAMINHO_TRAB];
  snprintf(c, sizeof(c), "%s/" TAB5_ARQ_LIXEIRA "/%s", raiz, nome);
  tab5_arq_erro_t r = TAB5_ARQ_OK;
  if (!existe(c)) {
    r = TAB5_ARQ_ERRO_FALHOU;
  } else if (!apagar_tudo(c, sizeof(c), false, false, 0)) {
    r = TAB5_ARQ_ERRO_FALHOU;
    ESP_LOGW(TAG, "excluir da lixeira %s: %d", nome, errno);
  }
  indice_tirar(raiz, nome);   // tambem limpa a linha de um item que ja nao existe
  return r;
}

// Fora de um trabalho de fundo o cancelamento nao vale (apagar_tudo o consulta).
static bool pode_apagar_direto(void)
{
  if (s_ocupado.load()) { return false; }
  s_cancelar = false;
  return true;
}

tab5_arq_erro_t tab5_arq_lixeira_excluir(const char* raiz, const char* nome)
{
  if (!eh_raiz(raiz) || !nome_lixo_valido(nome)) { return TAB5_ARQ_ERRO_NOME; }
  if (!pode_apagar_direto()) { return TAB5_ARQ_ERRO_OCUPADO; }
  if (!tab5_volume_montar(raiz)) { return TAB5_ARQ_ERRO_DISCO; }
  const tab5_arq_erro_t r = excluir_item(raiz, nome);
  tab5_volume_desmontar(raiz);
  return r;
}

int tab5_arq_lixeira_limpar_antigos(const char* raiz, int dias)
{
  const time_t agora = time(nullptr);
  if (!eh_raiz(raiz) || dias < 0 || agora <= EPOCH_VALIDA || !pode_apagar_direto()) { return 0; }
  if (!tab5_volume_montar(raiz)) { return 0; }
  int apagados = 0;
  char* indice = indice_ler(raiz, nullptr);   // copia propria: excluir_item reescreve o arquivo
  for (const char* p = indice; p && *p;) {
    const char* f = strchr(p, '\n');
    const char* t1 = strchr(p, '\t');
    const char* t2 = (t1 && (!f || t1 < f)) ? strchr(t1 + 1, '\t') : nullptr;
    if (t2 && (!f || t2 < f)) {
      const unsigned long q = strtoul(t2 + 1, nullptr, 10);
      const size_t ln = (size_t)(t1 - p);
      if (q > EPOCH_VALIDA && (long long)agora - (long long)q > (long long)dias * 86400 && ln < TAB5_ARQ_NOME_MAX) {
        char nome[TAB5_ARQ_NOME_MAX];
        memcpy(nome, p, ln);
        nome[ln] = '\0';
        if (nome_lixo_valido(nome) && excluir_item(raiz, nome) == TAB5_ARQ_OK) { ++apagados; }
      }
    }
    p = f ? f + 1 : nullptr;
  }
  if (indice) { heap_caps_free(indice); }
  tab5_volume_desmontar(raiz);
  return apagados;
}

// ---------------------------------------------------------------------------
// LIMITE E AVISO DA LIXEIRA
// ---------------------------------------------------------------------------
uint64_t tab5_arq_lixeira_limite(const char* raiz)
{
  const uint64_t MB = 1024ULL * 1024;
  uint64_t total = 0, livre = 0;
  if (!tab5_volume_espaco(raiz, &total, &livre)) { total = 0; }
  uint64_t limite = total / 100 * 8;                 // 8% do disco
  if (limite < 256 * MB)        { limite = 256 * MB; }          // minimo
  if (limite > 10 * 1024 * MB)  { limite = 10 * 1024 * MB; }    // maximo: 10 GB
  return limite;
}

// Um disco por vaga: o aviso e UM so (a fonte "Lixeira"), com o primeiro disco
// que passou do limite; a quantidade e quantos passaram.
static int      s_notif_lixo = -1;
static uint64_t s_lixo_acima[TAB5_VOL_N];   // bytes do disco que passou do limite; 0 = abaixo
static char     s_lixo_raiz[TAB5_VOL_N][16];

void tab5_arq_lixeira_manter(const char* raiz)
{
  if (!eh_raiz(raiz)) { return; }
  const int dias = ajustes_lixeira_dias();
  if (dias > 0) { tab5_arq_lixeira_limpar_antigos(raiz, dias); }

  uint64_t total = 0;
  tab5_arq_lixo_t um;   // so a soma interessa: a lista copia no maximo 1 item
  tab5_arq_lixeira_listar(raiz, &um, 1, &total);
  const bool acima = total > tab5_arq_lixeira_limite(raiz);

  const int v = (int)tab5_volume_de(raiz);
  if (v < 0 || v >= TAB5_VOL_N) { return; }
  s_lixo_acima[v] = acima ? total : 0;
  snprintf(s_lixo_raiz[v], sizeof(s_lixo_raiz[v]), "%s", raiz);

  if (s_notif_lixo < 0) {
    s_notif_lixo = notif_registrar("Lixeira");
    if (s_notif_lixo >= 0) { notif_associar_app(s_notif_lixo, "Arquivos"); }
  }
  if (s_notif_lixo < 0) { return; }
  int n = 0, primeiro = -1;
  for (int i = 0; i < TAB5_VOL_N; ++i) {
    if (s_lixo_acima[i]) { ++n; if (primeiro < 0) { primeiro = i; } }
  }
  if (n == 0) { notif_limpar(s_notif_lixo); return; }
  // O resumo da notificacao cabe em NOTIF_RESUMO (32): texto curto; tocar abre o Arquivos.
  char tam[16], r[NOTIF_RESUMO];
  tab5_bytes_txt(tam, sizeof(tam), s_lixo_acima[primeiro]);
  snprintf(r, sizeof(r), tr("Lixeira: %s, %s"), tr(tab5_volume_tipo(s_lixo_raiz[primeiro])), tam);
  notif_publicar(s_notif_lixo, n, NOTIF_ATENCAO, r);
}

tab5_arq_erro_t tab5_arq_esvaziar_lixeira(const char* raiz)
{
  if (!eh_raiz(raiz)) { return TAB5_ARQ_ERRO_NOME; }
  if (s_ocupado.exchange(true)) { return TAB5_ARQ_ERRO_OCUPADO; }
  job_t* j = job_novo();
  if (!j) { s_ocupado = false; return TAB5_ARQ_ERRO_FALHOU; }
  j->op = OP_ESVAZIAR;
  snprintf(j->destino, sizeof(j->destino), "%s", raiz);
  snprintf(j->a, sizeof(j->a), "%s/" TAB5_ARQ_LIXEIRA, raiz);
  if (!segurar_disco(j, raiz)) { heap_caps_free(j); s_ocupado = false; return TAB5_ARQ_ERRO_DISCO; }
  return comecar(j) ? TAB5_ARQ_OK : TAB5_ARQ_ERRO_FALHOU;
}

// ---------------------------------------------------------------------------
// AREA DE TRANSFERENCIA
// ---------------------------------------------------------------------------
EXT_RAM_BSS_ATTR static char s_area[TAB5_ARQ_ITENS_MAX][TAB5_ARQ_CAMINHO_MAX];
static int  s_area_n = 0;
static bool s_area_recortar = false;

bool tab5_arq_area_por(const char* const* caminhos, int n, bool recortar)
{
  if (n <= 0 || n > TAB5_ARQ_ITENS_MAX) { return false; }
  for (int i = 0; i < n; ++i) {
    if (!caminhos[i] || strlen(caminhos[i]) >= TAB5_ARQ_CAMINHO_MAX) { return false; }
    if (recortar ? !tab5_arq_mexivel(caminhos[i]) : !tab5_arq_visivel(caminhos[i])) { return false; }
  }
  s_area_trava.travar();
  for (int i = 0; i < n; ++i) { snprintf(s_area[i], sizeof(s_area[i]), "%s", caminhos[i]); }
  s_area_n = n;
  s_area_recortar = recortar;
  ++s_area_geracao;
  s_area_trava.destravar();
  return true;
}

int tab5_arq_area(bool* recortar)
{
  s_area_trava.travar();
  const int n = s_area_n;
  if (recortar) { *recortar = s_area_recortar; }
  s_area_trava.destravar();
  return n;
}

bool tab5_arq_area_item(int i, char* out, size_t tam)
{
  s_area_trava.travar();
  const bool ok = i >= 0 && i < s_area_n;
  if (ok && tam) { snprintf(out, tam, "%s", s_area[i]); }
  s_area_trava.destravar();
  return ok;
}

void tab5_arq_area_limpar(void)
{
  s_area_trava.travar();
  s_area_n = 0;
  s_area_recortar = false;
  ++s_area_geracao;
  s_area_trava.destravar();
}

static void area_limpar_se(uint32_t geracao)
{
  s_area_trava.travar();
  if (s_area_geracao == geracao) {
    s_area_n = 0;
    s_area_recortar = false;
    ++s_area_geracao;
  }
  s_area_trava.destravar();
}

tab5_arq_erro_t tab5_arq_colar(const char* destino)
{
  // Copia a area antes: conferir e segurar os discos demora (montar o
  // cartao), e a interface pergunta pela area enquanto isso.
  char (*copia)[TAB5_ARQ_CAMINHO_MAX] =
      (char (*)[TAB5_ARQ_CAMINHO_MAX])heap_caps_malloc(sizeof(s_area), MALLOC_CAP_SPIRAM);
  if (!copia) { return TAB5_ARQ_ERRO_FALHOU; }
  s_area_trava.travar();
  const int n = s_area_n;
  const bool recortar = s_area_recortar;
  const uint32_t geracao = s_area_geracao;
  memcpy(copia, s_area, sizeof(s_area[0]) * (size_t)n);
  s_area_trava.destravar();
  const char* ptrs[TAB5_ARQ_ITENS_MAX];
  for (int i = 0; i < n; ++i) { ptrs[i] = copia[i]; }
  const tab5_arq_erro_t r = n ? copiar_de_area(ptrs, n, destino, recortar, recortar ? geracao : 0)
                              : TAB5_ARQ_ERRO_NOME;
  heap_caps_free(copia);
  return r;
}
