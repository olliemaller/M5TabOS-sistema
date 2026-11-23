// Declarations shared by the console files (main/console.cpp and main/console/*.cpp).
// For them only: no app includes this header - apps talk to the console through
// console.hpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include "console.hpp"
#include "idioma.h"      // tr()
#include "tab5_sd.hpp"   // com_cartao

// ---------------------------------------------------------------------------
// Shared (defined in console.cpp)
// ---------------------------------------------------------------------------
void ceder(int i);                                   // yields the CPU during long listings
console_sessao_t* sessao(void);                      // the session of the task asking
void resolver(const char* c, char* out, size_t n);   // path relative to the current folder
const char* caminho_sd(const char* c, char* buf, size_t n);
bool nome_de_app(const char* nome, const char* s);   // "Musica" matches "musica"
void mostrar_uso(const char* nome);                  // the command's usage line
void juntar(char* out, size_t n, int argc, char** argv, int de);
inline bool eh(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// Mounts the SD card, runs whatever you pass, unmounts - always, including when
// the command fails. Written as a function because each command forgetting to
// unmount would be a count leak: the card would stay mounted for
// ever and the app using it could never really unmount.
template <typename F>
inline int com_cartao(F&& corpo)
{
  if (!tab5_sd_montar())
  {
    printf(tr("erro: cartao nao montou (%s)\n"), tab5_sd_descricao());
    return 1;
  }
  const int r = corpo();
  tab5_sd_desmontar();
  return r;
}

// ---------------------------------------------------------------------------
// Used by more than one file
// ---------------------------------------------------------------------------
void listar_pasta(const char* pasta, const char* filtro);   // console_cartao.cpp

// ---------------------------------------------------------------------------
// Commands (console_<group>.cpp) - the COMANDOS table in console.cpp calls them
// ---------------------------------------------------------------------------
// SYSTEM
void c_estado(int, char**);
void c_tarefas(int argc, char** argv);
void c_mem(int argc, char** argv);
void c_energia(int argc, char** argv);
void c_musica(int argc, char** argv);
void c_video(int argc, char** argv);
void c_fundo(int argc, char** argv);
void c_log(int argc, char** argv);
void c_boasvindas(int, char**);
void c_fuso(int, char**);
#if M5_COM_BERRY
void c_berry(int argc, char** argv);
#endif
void c_pendrive(int argc, char** argv);
void c_usb(int argc, char** argv);

// SHELL
void c_cd(int argc, char** argv);
void c_pwd(int, char**);
void c_ls(int argc, char** argv);
void c_df(int, char**);
void c_atalho_arquivos(int argc, char** argv);
void c_clear(int, char**);
void c_echo(int argc, char** argv);

// SCREEN
void c_tela(int argc, char** argv);
void c_brilho(int argc, char** argv);
void c_piscada(int argc, char** argv);
void c_toque(int argc, char** argv);
void c_tocar(int argc, char** argv);
void c_abrir(int argc, char** argv);
void c_ordem(int argc, char** argv);
void c_ui(int, char**);

// SD CARD
void c_arquivos(int argc, char** argv);
void cmd_cartao(void);
int  cmd_lista(int argc, char** argv);
int  cmd_ler(int argc, char** argv);
int  cmd_despejar(int argc, char** argv);
int  cmd_apagar(int argc, char** argv);
void cmd_receber(int argc, char** argv);

// NETWORK
void c_wifi(int argc, char** argv);
void c_redes(int, char**);
void c_ble(int argc, char** argv);
void c_espnow(int argc, char** argv);
void c_c6(int argc, char** argv);
void c_farejo(int argc, char** argv);
void c_radio(int argc, char** argv);
void c_ping(int argc, char** argv);
void c_ip(int, char**);
void c_dns(int argc, char** argv);
void c_arp(int argc, char** argv);
void c_traceroute(int argc, char** argv);
void c_tcp(int argc, char** argv);
void c_http(int argc, char** argv);
void c_busca(int argc, char** argv);
void c_ssh(int argc, char** argv);
void c_web(int argc, char** argv);

// MESHTASTIC
void c_mesh(int argc, char** argv);
void c_nos(int, char**);
void c_enviar(int argc, char** argv);

// BENCH
void c_grove(int argc, char** argv);
int  cmd_i2c(int argc, char** argv);
void cmd_camera(int argc, char** argv);
void cmd_foto(void);

// TESTS
void cmd_concorrencia(int argc, char** argv);
void cmd_simulacao(int argc, char** argv);
