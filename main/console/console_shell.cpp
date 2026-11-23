// Console commands - SHELL: cd, pwd, ls, df, cp/mv/rm/mkdir (shortcuts for arquivos), echo, clear.
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "tab5_volumes.hpp"
#include "tab5_arquivos.hpp"
#include "tab5_tela.hpp"
#include "console/console_interno.hpp"

// ---------------------------------------------------------------------------
// SHELL - the session's current folder and the usual names, on top of the file
// engine (services/tab5_arquivos) and the disks (services/tab5_volumes).
// "/" is the root of the disks: /sdcard and /usb.
// ---------------------------------------------------------------------------
static bool nada(const tab5_arq_item_t*, void*) { return false; }

void c_cd(int argc, char** argv)
{
  char p[sizeof(console_sessao_t::cwd)];
  resolver(argc >= 2 ? argv[1] : "/sdcard", p, sizeof(p));
  if (strcmp(p, "/") != 0 && tab5_arq_percorrer(p, nada, nullptr) < 0) {
    printf(tr("cd: %s nao e uma pasta (ou o disco nao esta aqui)\n"), p);
    return;
  }
  snprintf(sessao()->cwd, sizeof(console_sessao_t::cwd), "%s", p);
}

void c_pwd(int, char**) { printf("%s\n", sessao()->cwd); }

void c_ls(int argc, char** argv)
{
  char p[TAB5_ARQ_CAMINHO_MAX];
  resolver(argc >= 2 ? argv[1] : ".", p, sizeof(p));
  if (strcmp(p, "/") != 0) { listar_pasta(p, nullptr); return; }
  tab5_vol_t v[4];
  const int n = tab5_volumes_listar(v, 4);
  for (int i = 0; i < n; ++i) {
    printf("  %s/  %s%s%s\n", v[i].raiz + 1, v[i].tipo, v[i].nome[0] ? "  " : "", v[i].nome);
  }
}

void c_df(int, char**)
{
  tab5_vol_t v[4];
  const int n = tab5_volumes_listar(v, 4);
  printf("  %-8s %10s %10s  %s\n", tr("disco"), tr("total"), tr("livre"), tr("tipo"));
  for (int i = 0; i < n; ++i) {
    uint64_t t = 0, l = 0;
    if (v[i].disponivel && tab5_volume_espaco(v[i].raiz, &t, &l)) {
      printf("  %-8s %8.1f G %8.1f G  %s\n", v[i].raiz, t / 1e9, l / 1e9, v[i].tipo);
    } else {
      printf("  %-8s %10s %10s  %s\n", v[i].raiz, "-", "-", v[i].tipo);
    }
  }
}

// cp, mv, rm and mkdir are 'arquivos' with the verb in front.
void c_atalho_arquivos(int argc, char** argv)
{
  char* a[24];
  char nome[] = "arquivos";
  a[0] = nome;
  for (int i = 0; i < argc && i < 23; ++i) { a[i + 1] = argv[i]; }
  c_arquivos(argc + 1 < 24 ? argc + 1 : 24, a);
}

// Clears the screen: the usual ANSI sequence (the Terminal and the serial monitor understand it).
void c_clear(int, char**) { printf("\x1b[2J\x1b[H"); }

void c_echo(int argc, char** argv)
{
  for (int i = 1; i < argc; ++i) { printf(i > 1 ? " %s" : "%s", argv[i]); }
  printf("\n");
}
