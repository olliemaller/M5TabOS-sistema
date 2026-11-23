#include "tab5_volumes.hpp"
#include "tab5_sd.hpp"
#include "tab5_usb.hpp"
#include "app.h"
#include "idioma.h"

#include <atomic>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"


static bool comeca_com(const char* c, const char* raiz)
{
  const size_t n = strlen(raiz);
  return c && !strncmp(c, raiz, n) && (c[n] == '\0' || c[n] == '/');
}

tab5_vol_id_t tab5_volume_de(const char* caminho)
{
  if (comeca_com(caminho, TAB5_VOL_CARTAO_RAIZ))   { return TAB5_VOL_CARTAO; }
  if (comeca_com(caminho, TAB5_VOL_PENDRIVE_RAIZ)) { return TAB5_VOL_PENDRIVE; }
  return TAB5_VOL_NENHUM;
}

const char* tab5_volume_tipo(const char* caminho)
{
  switch (tab5_volume_de(caminho)) {
    case TAB5_VOL_CARTAO:   return N_("Cartão SD");
    case TAB5_VOL_PENDRIVE: return N_("Pendrive");
    default:                return "";
  }
}

int tab5_volumes_listar(tab5_vol_t* out, int max)
{
  int n = 0;
  if (n < max) {
    tab5_vol_t& v = out[n++];
    v.id = TAB5_VOL_CARTAO;
    v.raiz = TAB5_VOL_CARTAO_RAIZ;
    v.tipo = "Cartão SD";
    snprintf(v.nome, sizeof(v.nome), "%s", tab5_sd_nome());
    v.disponivel = !tab5_sd_emprestado();
  }
  if (n < max && tab5_usb_presente()) {
    tab5_vol_t& v = out[n++];
    v.id = TAB5_VOL_PENDRIVE;
    v.raiz = TAB5_VOL_PENDRIVE_RAIZ;
    v.tipo = "Pendrive";
    tab5_usb_nome(v.nome, sizeof(v.nome));
    v.disponivel = true;
  }
  return n;
}

bool tab5_volume_montar(const char* caminho)
{
  switch (tab5_volume_de(caminho)) {
    case TAB5_VOL_CARTAO:   return tab5_sd_montar();
    case TAB5_VOL_PENDRIVE: return tab5_usb_montar();
    default:                return false;
  }
}

void tab5_volume_desmontar(const char* caminho)
{
  switch (tab5_volume_de(caminho)) {
    case TAB5_VOL_CARTAO:   tab5_sd_desmontar(); break;
    case TAB5_VOL_PENDRIVE: tab5_usb_desmontar(); break;
    default: break;
  }
}

FILE* tab5_volume_abrir_leitura(const char* caminho)
{
  if (!tab5_volume_montar(caminho)) { return nullptr; }
  // On the card, a read through fopen does not go through the write queue: write
  // out what is pending first, to see the last write.
  if (tab5_volume_de(caminho) == TAB5_VOL_CARTAO) { tab5_sd_gravar_pendentes(); }
  FILE* f = fopen(caminho, "rb");
  if (!f) { tab5_volume_desmontar(caminho); }
  return f;
}

void tab5_volume_fechar(FILE* f, const char* caminho)
{
  fclose(f);
  tab5_volume_desmontar(caminho);
}

uint8_t* tab5_volume_ler_tudo(const char* caminho, size_t max, size_t* n)
{
  *n = 0;
  FILE* f = tab5_volume_abrir_leitura(caminho);
  if (!f) { return nullptr; }
  uint8_t* buf = nullptr;
  const long tam = tab5_sd_tamanho(f);
  if (tam > 0 && (size_t)tam <= max) {
    buf = (uint8_t*)heap_caps_malloc((size_t)tam, MALLOC_CAP_SPIRAM);
    if (buf && fread(buf, 1, (size_t)tam, f) != (size_t)tam) {   // short read
      heap_caps_free(buf);
      buf = nullptr;
    }
    if (buf) { *n = (size_t)tam; }
  }
  tab5_volume_fechar(f, caminho);
  return buf;
}

// ---------------------------------------------------------------------------
// FAST READER (see the reasons in the .hpp)
// ---------------------------------------------------------------------------
#define LEITOR_BLOCO       (64 * 1024)
#define LEITOR_ALINHAMENTO 128   // L2 cache line: the card's DMA requires it in PSRAM
#define LEITOR_SETOR       512   // lseek goes to the start of the sector

struct tab5_leitor {
  int      fd;
  uint8_t* buf;        // LEITOR_BLOCO bytes, aligned
  size_t   n;          // valid bytes in the buffer
  size_t   pos;        // next byte to hand out, within the buffer
  uint64_t fim_buf;    // file position just after the buffer's last byte
  uint64_t tamanho;
  char     caminho[256];   // only the prefix matters: used to find the disk on release
};

tab5_leitor_t* tab5_leitor_abrir(const char* caminho)
{
  if (!tab5_volume_montar(caminho)) { return nullptr; }
  // As with fopen: on the card the read does not go through the write queue.
  if (tab5_volume_de(caminho) == TAB5_VOL_CARTAO) { tab5_sd_gravar_pendentes(); }
  tab5_leitor_t* r = (tab5_leitor_t*)heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM);
  if (r) {
    r->buf = (uint8_t*)heap_caps_aligned_alloc(LEITOR_ALINHAMENTO, LEITOR_BLOCO, MALLOC_CAP_SPIRAM);
    r->fd = r->buf ? open(caminho, O_RDONLY) : -1;
    struct stat st;
    if (r->fd >= 0 && fstat(r->fd, &st) == 0) { r->tamanho = (uint64_t)st.st_size; }
    else if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    snprintf(r->caminho, sizeof(r->caminho), "%s", caminho);
  }
  if (!r || r->fd < 0) {
    if (r && r->buf) { heap_caps_free(r->buf); }
    heap_caps_free(r);
    tab5_volume_desmontar(caminho);
    return nullptr;
  }
  return r;
}

int tab5_leitor_ler(tab5_leitor_t* r, void* dst, size_t n)
{
  uint8_t* d = (uint8_t*)dst;
  size_t feito = 0;
  while (feito < n) {
    if (r->pos >= r->n) {   // buffer exhausted: fetch the next block
      const ssize_t lidos = read(r->fd, r->buf, LEITOR_BLOCO);
      if (lidos < 0) { return -1; }
      if (lidos == 0) { break; }   // end of file
      r->n = (size_t)lidos;
      r->pos = 0;
      r->fim_buf += (uint64_t)lidos;
    }
    size_t parte = r->n - r->pos;
    if (parte > n - feito) { parte = n - feito; }
    if (d) { memcpy(d + feito, r->buf + r->pos, parte); }
    r->pos += parte;
    feito  += parte;
  }
  return (int)feito;
}

uint64_t tab5_leitor_pos(const tab5_leitor_t* r)
{
  return r->fim_buf - (r->n - r->pos);
}

bool tab5_leitor_ir(tab5_leitor_t* r, uint64_t pos)
{
  if (pos > r->tamanho) { return false; }
  const uint64_t ini_buf = r->fim_buf - r->n;
  if (r->n && pos >= ini_buf && pos <= r->fim_buf) {   // already in the buffer
    r->pos = (size_t)(pos - ini_buf);
    return true;
  }
  // Outside the buffer: lseek goes to the start of the sector (the card's DMA does
  // not like starting in the middle) and the rest up to 'pos' is discarded on reading.
  const uint64_t inicio = pos & ~(uint64_t)(LEITOR_SETOR - 1);
  if (lseek(r->fd, (off_t)inicio, SEEK_SET) != (off_t)inicio) { return false; }
  r->n = r->pos = 0;
  r->fim_buf = inicio;
  const size_t resto = (size_t)(pos - inicio);
  return tab5_leitor_ler(r, nullptr, resto) == (int)resto;
}

uint64_t tab5_leitor_tamanho(const tab5_leitor_t* r)
{
  return r->tamanho;
}

void tab5_leitor_fechar(tab5_leitor_t* r)
{
  if (!r) { return; }
  close(r->fd);
  heap_caps_free(r->buf);
  char caminho[sizeof(r->caminho)];
  memcpy(caminho, r->caminho, sizeof(caminho));
  heap_caps_free(r);
  tab5_volume_desmontar(caminho);
}

bool tab5_volume_espaco(const char* caminho, uint64_t* total, uint64_t* livre)
{
  if (!tab5_volume_montar(caminho)) { return false; }
  const bool ok = tab5_volume_de(caminho) == TAB5_VOL_CARTAO ? tab5_sd_espaco(total, livre)
                                                             : tab5_usb_espaco(total, livre);
  tab5_volume_desmontar(caminho);
  return ok;
}

bool tab5_volume_disponivel(const char* caminho)
{
  switch (tab5_volume_de(caminho)) {
    case TAB5_VOL_CARTAO:   return !tab5_sd_emprestado();
    case TAB5_VOL_PENDRIVE: return tab5_usb_presente();
    default:                return false;
  }
}

bool tab5_volume_ejetavel(const char* caminho)
{
  return tab5_volume_de(caminho) == TAB5_VOL_PENDRIVE;
}

tab5_vol_ejetar_t tab5_volume_ejetar(const char* caminho, uint32_t espera_ms)
{
  if (!tab5_volume_ejetavel(caminho)) { return TAB5_VOL_EJETAR_NAO_DA; }
  switch (tab5_usb_ejetar(espera_ms)) {
    case USB_EJETADO:     return TAB5_VOL_EJETADO;
    case USB_EJETAR_EM_USO: return TAB5_VOL_EJETAR_EM_USO;
    default:              return TAB5_VOL_EJETAR_NAO_DA;
  }
}

void tab5_volumes_mudou(void)
{
  sistema_avisar(AVISO_VOLUMES);
  sistema_acordar_interface();
}
