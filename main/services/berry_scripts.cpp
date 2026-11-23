// The Berry scripts on the card - see berry_scripts.hpp.
#include "berry_scripts.hpp"
#include "berry_apps.hpp"
#include "tab5_sd.hpp"
#include <stdio.h>
#include <string.h>

int berry_scripts_listar(tab5_arq_item_t* out, int max)
{
  // Mount: tab5_sd_pasta only answers with the card mounted (without this, the
  // first listing after boot said "no card").
  if (!tab5_sd_montar()) { return -1; }
  char pasta[96];
  int n = tab5_sd_pasta(BERRY_PASTA, pasta, sizeof(pasta))
        ? tab5_arq_listar(pasta, out, max, nullptr, TAB5_ARQ_ORDEM_NOME, ".be") : -1;
  tab5_sd_desmontar();
  // The listing always brings folders (the apps' data folders): only the scripts.
  int k = 0;
  for (int i = 0; i < n; ++i) { if (out[i].tipo != TAB5_ARQ_PASTA) { out[k++] = out[i]; } }
  return n > 0 ? k : n;
}

berry_ler_t berry_script_ler(const char* arq, char* buf, size_t tam)
{
  buf[0] = 0;
  const bool montou = tab5_sd_montar();
  FILE* f = montou ? tab5_sd_abrir(BERRY_PASTA, arq, "r") : nullptr;
  berry_ler_t r = BERRY_LER_FALHOU;
  if (f) {
    const size_t n = fread(buf, 1, tam - 1, f);
    // The buffer filled and there are still bytes: too big. No half files.
    if (n == tam - 1 && fgetc(f) != EOF) { buf[0] = 0; r = BERRY_LER_GRANDE; }
    else                                  { buf[n] = 0; r = BERRY_LER_OK; }
    fclose(f);
  }
  if (montou) { tab5_sd_desmontar(); }
  return r;
}

bool berry_script_gravar(const char* arq, const char* txt, size_t n)
{
  if (!tab5_sd_montar()) { return false; }
  const bool ok = tab5_sd_substituir(BERRY_PASTA, arq, txt, n);
  tab5_sd_desmontar();
  return ok;
}

bool berry_script_nome_valido(const char* arq)
{
  const size_t n = arq ? strlen(arq) : 0;
  if (n < 4 || n >= 64 || arq[0] == '.' || strcasecmp(arq + n - 3, ".be")) { return false; }
  for (size_t i = 0; i < n; ++i) {
    const char c = arq[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                 || c == ' ' || c == '_' || c == '-' || c == '.';
    if (!ok) { return false; }
  }
  return !strstr(arq, "..");
}

void berry_script_nome_app(const char* txt, const char* arq, char* out, size_t n)
{
  const char* p = strstr(txt, "# nome:");
  if (p && p - txt < 400) {
    p += 7;
    while (*p == ' ') { ++p; }
    size_t k = strcspn(p, "\r\n");
    while (k && p[k - 1] == ' ') { --k; }
    snprintf(out, n, "%.*s", (int)k, p);
    return;
  }
  snprintf(out, n, "%s", arq);
  char* ponto = strrchr(out, '.');
  if (ponto) { *ponto = 0; }
}
