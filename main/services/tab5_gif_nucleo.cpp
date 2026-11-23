#include "tab5_gif_nucleo.hpp"

#include <string.h>

#define LZW_MAX      4096   // LZW table entries (codes of up to 12 bits)
#define ATRASO_MIN   20     // ms
#define ATRASO_PADRAO 100   // ms, when the file says 0

struct gif_nucleo {
  const uint8_t* dados;
  size_t         n;
  gif_alocar_t   alocar;
  gif_liberar_t  liberar;

  int       largura, altura;
  uint16_t* tela;              // width x height
  uint16_t  fundo;             // background colour (from the global table; black without it)
  uint8_t   global[256 * 3];   // global colour table (RGB)
  int       n_global;          // how many colours
  size_t    inicio;            // where the blocks start (after the global table)
  size_t    pos;               // next block to read
  bool      animado;
  int       vistos;
  bool      nesta_volta;       // has already decoded some frame in this loop

  // What the next frame inherits from the Graphic Control Extension.
  int  gce_atraso;             // in ms, minimum already applied; ATRASO_PADRAO without a GCE
  int  gce_descarte;
  int  gce_transp;             // transparent index or -1

  // The PREVIOUS frame's disposal, applied before drawing the next one.
  int       ant_descarte;
  int       ant_x, ant_y, ant_l, ant_a;   // rectangle already clipped to the screen
  bool      ant_transp;
  uint16_t* copia;             // the rectangle before drawing (disposal 3)
  size_t    copia_cap;         // in pixels

  // LZW, without recursion: each code is (prefix, last byte).
  uint16_t prefixo[LZW_MAX];
  uint8_t  sufixo[LZW_MAX];
  uint8_t  pilha[LZW_MAX + 1];
  uint16_t paleta[256];        // the current frame's table already in RGB565
};

static inline uint16_t cor565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline int u16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Skips the data sub-blocks starting at 'p' (size, bytes, ... 0).
// Returns the position after the final 0; n if the file ended first.
static size_t pular_subblocos(const uint8_t* d, size_t n, size_t p)
{
  while (p < n) {
    const size_t t = d[p++];
    if (t == 0) { return p; }
    if (t > n - p) { return n; }
    p += t;
  }
  return n;
}

// Reads a frame's header (image descriptor at d[p], which is 0x2C):
// returns the position of the first LZW data sub-block, or 0 if it is truncated.
// Fills in the requested information (any pointer may be nullptr).
typedef struct {
  int x, y, l, a;
  bool entrelacado;
  int  n_local;            // local table colours (0 = none)
  size_t tabela_local;     // where it starts
  int  codigo_minimo;
} quadro_t;

static size_t ler_quadro(const uint8_t* d, size_t n, size_t p, quadro_t* q)
{
  if (n - p < 10) { return 0; }   // 0x2C + 9 descriptor bytes
  q->x = u16(d + p + 1);
  q->y = u16(d + p + 3);
  q->l = u16(d + p + 5);
  q->a = u16(d + p + 7);
  const uint8_t flags = d[p + 9];
  q->entrelacado = (flags & 0x40) != 0;
  q->n_local = (flags & 0x80) ? (2 << (flags & 7)) : 0;
  p += 10;
  q->tabela_local = p;
  const size_t bytes = (size_t)q->n_local * 3;
  if (bytes > n - p) { return 0; }
  p += bytes;
  if (p >= n) { return 0; }
  q->codigo_minimo = d[p++];
  return p;
}

// How many frames there are in the file, counting only up to 2 (enough for "animated").
static int contar_quadros(const uint8_t* d, size_t n, size_t p)
{
  int quadros = 0;
  while (p < n && quadros < 2) {
    const uint8_t b = d[p];
    if (b == 0x21) {                           // extension: label + sub-blocks
      if (n - p < 2) { break; }
      p = pular_subblocos(d, n, p + 2);
    } else if (b == 0x2C) {                    // frame
      quadro_t q;
      const size_t dados = ler_quadro(d, n, p, &q);
      if (!dados) { break; }
      ++quadros;
      p = pular_subblocos(d, n, dados);
    } else {                                   // 0x3B (end) or garbage
      break;
    }
  }
  return quadros;
}

gif_nucleo_t* gif_n_abrir(const uint8_t* dados, size_t n, gif_alocar_t alocar, gif_liberar_t liberar)
{
  if (n < 13 || (memcmp(dados, "GIF87a", 6) && memcmp(dados, "GIF89a", 6))) { return nullptr; }
  const int l = u16(dados + 6), a = u16(dados + 8);
  if (l < 1 || a < 1 || l > GIF_TELA_MAX || a > GIF_TELA_MAX) { return nullptr; }

  gif_nucleo_t* g = (gif_nucleo_t*)alocar(sizeof(gif_nucleo_t));
  if (!g) { return nullptr; }
  memset(g, 0, sizeof(*g));
  g->dados = dados;
  g->n = n;
  g->alocar = alocar;
  g->liberar = liberar;
  g->largura = l;
  g->altura = a;

  const uint8_t flags = dados[10];
  size_t p = 13;
  if (flags & 0x80) {                          // global colour table
    g->n_global = 2 << (flags & 7);
    const size_t bytes = (size_t)g->n_global * 3;
    if (bytes > n - p) { gif_n_fechar(g); return nullptr; }
    memcpy(g->global, dados + p, bytes);
    p += bytes;
    const int bg = dados[11];
    if (bg < g->n_global) { g->fundo = cor565(g->global[bg * 3], g->global[bg * 3 + 1], g->global[bg * 3 + 2]); }
  }
  g->inicio = g->pos = p;
  const int quadros = contar_quadros(dados, n, p);
  if (quadros == 0) { gif_n_fechar(g); return nullptr; }
  g->animado = quadros > 1;

  g->tela = (uint16_t*)alocar((size_t)l * a * sizeof(uint16_t));
  if (!g->tela) { gif_n_fechar(g); return nullptr; }
  for (size_t i = 0; i < (size_t)l * a; ++i) { g->tela[i] = g->fundo; }
  g->gce_atraso = ATRASO_PADRAO;
  g->gce_transp = -1;
  return g;
}

void gif_n_fechar(gif_nucleo_t* g)
{
  if (!g) { return; }
  gif_liberar_t liberar = g->liberar;
  if (g->tela)  { liberar(g->tela); }
  if (g->copia) { liberar(g->copia); }
  liberar(g);
}

int  gif_n_largura(const gif_nucleo_t* g)        { return g->largura; }
int  gif_n_altura(const gif_nucleo_t* g)         { return g->altura; }
bool gif_n_animado(const gif_nucleo_t* g)        { return g->animado; }
int  gif_n_quadros_vistos(const gif_nucleo_t* g) { return g->vistos; }
const uint16_t* gif_n_tela(const gif_nucleo_t* g) { return g->tela; }

// ---------------------------------------------------------------------------
// PREVIOUS FRAME DISPOSAL
// ---------------------------------------------------------------------------
static void aplicar_descarte(gif_nucleo_t* g)
{
  if (g->ant_l <= 0 || g->ant_a <= 0) { return; }
  // 2 = back to background (black if the frame had a transparent colour); 3 = restore.
  const uint16_t cor = g->ant_transp ? 0 : g->fundo;
  for (int y = 0; y < g->ant_a; ++y) {
    uint16_t* linha = g->tela + (size_t)(g->ant_y + y) * g->largura + g->ant_x;
    if (g->ant_descarte == 2) {
      for (int x = 0; x < g->ant_l; ++x) { linha[x] = cor; }
    } else if (g->ant_descarte == 3 && g->copia) {
      memcpy(linha, g->copia + (size_t)y * g->ant_l, (size_t)g->ant_l * sizeof(uint16_t));
    }
  }
}

// ---------------------------------------------------------------------------
// SUB-BLOCK BIT READER
// ---------------------------------------------------------------------------
typedef struct {
  const uint8_t* d;
  size_t   n;
  size_t   pos;        // next byte (or the size byte of the next sub-block)
  size_t   resta;      // bytes left in the current sub-block
  uint32_t acumulado;
  int      bits;
  bool     fim;        // data ran out (terminator or end of file)
} bits_t;

static bool proximo_byte(bits_t* b, uint8_t* out)
{
  while (b->resta == 0) {
    if (b->fim || b->pos >= b->n) { b->fim = true; return false; }
    const size_t t = b->d[b->pos++];
    if (t == 0) { b->fim = true; return false; }
    b->resta = t > b->n - b->pos ? b->n - b->pos : t;   // sub-block truncated at the end of the file
  }
  *out = b->d[b->pos++];
  --b->resta;
  return true;
}

// Reads 'tam' bits (LSB first). false = no more data.
static bool pegar_bits(bits_t* b, int tam, int* codigo)
{
  while (b->bits < tam) {
    uint8_t v;
    if (!proximo_byte(b, &v)) { return false; }
    b->acumulado |= (uint32_t)v << b->bits;
    b->bits += 8;
  }
  *codigo = (int)(b->acumulado & ((1u << tam) - 1));
  b->acumulado >>= tam;
  b->bits -= tam;
  return true;
}

// ---------------------------------------------------------------------------
// DECODING A FRAME
// ---------------------------------------------------------------------------
// Puts a pixel (colour index) at the frame's current position and moves to the
// next, following interlaced order if applicable. false = the frame is full.
typedef struct {
  const quadro_t* q;
  int  x, linha;       // position within the frame (row already in file order)
  int  passo;          // interlaced: 0..3
  bool cheio;
} cursor_t;

static const int ENT_INICIO[4] = { 0, 4, 2, 1 };
static const int ENT_SALTO[4]  = { 8, 8, 4, 2 };

static void avancar(cursor_t* c)
{
  const quadro_t* q = c->q;
  if (++c->x < q->l) { return; }
  c->x = 0;
  if (!q->entrelacado) {
    if (++c->linha >= q->a) { c->cheio = true; }
    return;
  }
  c->linha += ENT_SALTO[c->passo];
  while (c->linha >= q->a && c->passo < 3) { c->linha = ENT_INICIO[++c->passo]; }
  if (c->linha >= q->a) { c->cheio = true; }
}

static void decodificar_lzw(gif_nucleo_t* g, const quadro_t* q, size_t dados)
{
  const int minimo = q->codigo_minimo;
  if (minimo < 2 || minimo > 8 || q->l <= 0 || q->a <= 0) { return; }

  bits_t b = { g->dados, g->n, dados, 0, 0, 0, false };
  cursor_t c = { q, 0, 0, 0, false };
  const int limpar = 1 << minimo, fim_cod = limpar + 1;
  int proximo = fim_cod + 1, tam = minimo + 1;
  int anterior = -1, primeiro = 0;

  while (!c.cheio) {
    int codigo;
    if (!pegar_bits(&b, tam, &codigo)) { break; }
    if (codigo == limpar) { proximo = fim_cod + 1; tam = minimo + 1; anterior = -1; continue; }
    if (codigo == fim_cod) { break; }

    int sp = 0;
    int atual = codigo;
    if (anterior < 0) {                       // first code after a clear
      if (codigo >= limpar) { break; }        // corrupt
      primeiro = codigo;
    } else {
      if (codigo > proximo) { break; }        // corrupt
      if (codigo == proximo) {                // doesn't exist yet: previous + its 1st byte
        g->pilha[sp++] = (uint8_t)primeiro;
        atual = anterior;
      }
      while (atual >= limpar) {               // unwinds the chain, from end to start
        if (sp >= LZW_MAX) { sp = -1; break; }
        g->pilha[sp++] = g->sufixo[atual];
        atual = g->prefixo[atual];
      }
      if (sp < 0) { break; }
      primeiro = atual;
    }
    g->pilha[sp++] = (uint8_t)atual;
    if (anterior >= 0 && proximo < LZW_MAX) {   // learns: previous + 1st byte of this one
      g->prefixo[proximo] = (uint16_t)anterior;
      g->sufixo[proximo]  = (uint8_t)primeiro;
      ++proximo;
      if (proximo == (1 << tam) && tam < 12) { ++tam; }
    }
    anterior = codigo;

    // The stack comes out back to front (the last byte went in first).
    while (sp > 0 && !c.cheio) {
      const uint8_t idx = g->pilha[--sp];
      const int gx = q->x + c.x, gy = q->y + c.linha;
      if (gx < g->largura && gy < g->altura && idx != g->gce_transp) {
        g->tela[(size_t)gy * g->largura + gx] = g->paleta[idx];
      }
      avancar(&c);
    }
  }
}

int gif_n_proximo(gif_nucleo_t* g)
{
  const uint8_t* d = g->dados;
  const size_t n = g->n;
  for (;;) {
    if (g->pos >= n || d[g->pos] == 0x3B) {   // end: back to the first frame
      if (!g->nesta_volta) { return -1; }     // a whole loop with no frame: nothing to show
      g->pos = g->inicio;
      g->nesta_volta = false;
      g->ant_descarte = 0;
      g->ant_l = g->ant_a = 0;
      for (size_t i = 0; i < (size_t)g->largura * g->altura; ++i) { g->tela[i] = g->fundo; }
      continue;
    }
    const uint8_t b = d[g->pos];
    if (b == 0x21) {                          // extension
      if (n - g->pos < 2) { g->pos = n; continue; }
      const uint8_t rotulo = d[g->pos + 1];
      if (rotulo == 0xF9 && n - g->pos >= 8 && d[g->pos + 2] == 4) {   // Graphic Control
        const uint8_t f = d[g->pos + 3];
        const int cs = u16(d + g->pos + 4);
        g->gce_descarte = (f >> 2) & 7;
        g->gce_atraso = cs == 0 ? ATRASO_PADRAO : (cs * 10 < ATRASO_MIN ? ATRASO_MIN : cs * 10);
        g->gce_transp = (f & 1) ? d[g->pos + 6] : -1;
      }
      g->pos = pular_subblocos(d, n, g->pos + 2);
      continue;
    }
    if (b != 0x2C) { g->pos = n; continue; }  // garbage: treat as end

    // ---- a frame ----
    quadro_t q;
    const size_t dados = ler_quadro(d, n, g->pos, &q);
    if (!dados) { g->pos = n; continue; }     // truncated

    aplicar_descarte(g);                       // what the previous frame asked for

    // The frame's table: the local one if present, otherwise the global; index outside it = black.
    const uint8_t* tab = q.n_local ? d + q.tabela_local : g->global;
    const int ncores = q.n_local ? q.n_local : g->n_global;
    for (int i = 0; i < 256; ++i) {
      g->paleta[i] = i < ncores ? cor565(tab[i * 3], tab[i * 3 + 1], tab[i * 3 + 2]) : 0;
    }

    // The frame's rectangle clipped to the screen (for the next one's disposal).
    int rx = q.x, ry = q.y, rl = q.l, ra = q.a;
    if (rx >= g->largura || ry >= g->altura) { rl = ra = 0; }
    else {
      if (rx + rl > g->largura) { rl = g->largura - rx; }
      if (ry + ra > g->altura)  { ra = g->altura - ry; }
    }
    if (g->gce_descarte == 3 && rl > 0 && ra > 0) {   // saves what is about to be covered
      const size_t preciso = (size_t)rl * ra;
      if (preciso > g->copia_cap) {
        if (g->copia) { g->liberar(g->copia); }
        g->copia = (uint16_t*)g->alocar(preciso * sizeof(uint16_t));
        g->copia_cap = g->copia ? preciso : 0;
      }
      if (g->copia) {
        for (int y = 0; y < ra; ++y) {
          memcpy(g->copia + (size_t)y * rl, g->tela + (size_t)(ry + y) * g->largura + rx, (size_t)rl * sizeof(uint16_t));
        }
      }
    }

    decodificar_lzw(g, &q, dados);

    const int atraso = g->gce_atraso;
    g->ant_descarte = g->gce_descarte;
    g->ant_x = rx; g->ant_y = ry; g->ant_l = rl; g->ant_a = ra;
    g->ant_transp = g->gce_transp >= 0;
    g->gce_atraso = ATRASO_PADRAO;             // the GCE applies only to the frame that comes right after
    g->gce_descarte = 0;
    g->gce_transp = -1;
    g->pos = pular_subblocos(d, n, dados);
    g->nesta_volta = true;
    ++g->vistos;
    return atraso;
  }
}
