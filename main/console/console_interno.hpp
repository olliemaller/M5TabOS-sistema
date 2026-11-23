// Declaracoes comuns aos arquivos do console (main/console.cpp e main/console/*.cpp).
// So para eles: nenhum app inclui este header - app fala com o console por
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
// Comuns (definidos em console.cpp)
// ---------------------------------------------------------------------------
void ceder(int i);                                   // cede a CPU nas listagens longas
console_sessao_t* sessao(void);                      // a sessao da tarefa que pergunta
void resolver(const char* c, char* out, size_t n);   // caminho relativo a pasta atual
const char* caminho_sd(const char* c, char* buf, size_t n);
bool nome_de_app(const char* nome, const char* s);   // "Musica" casa com "musica"
void mostrar_uso(const char* nome);                  // a linha de uso do comando
void juntar(char* out, size_t n, int argc, char** argv, int de);
inline bool eh(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

// Monta o cartao, roda o que voce passar, desmonta - sempre, inclusive quando
// o comando falha. Escrito como funcao porque cada comando esquecer o
// desmontar seria um vazamento de contagem: o cartao ficaria montado para
// sempre e o app que o usa nunca conseguiria desmontar de verdade.
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
// Usado por mais de um arquivo
// ---------------------------------------------------------------------------
void listar_pasta(const char* pasta, const char* filtro);   // console_cartao.cpp

// ---------------------------------------------------------------------------
// Comandos (console_<grupo>.cpp) - a tabela COMANDOS em console.cpp os chama
// ---------------------------------------------------------------------------
// SISTEMA
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

// TELA
void c_tela(int argc, char** argv);
void c_brilho(int argc, char** argv);
void c_piscada(int argc, char** argv);
void c_toque(int argc, char** argv);
void c_tocar(int argc, char** argv);
void c_abrir(int argc, char** argv);
void c_ordem(int argc, char** argv);
void c_ui(int, char**);

// CARTAO
void c_arquivos(int argc, char** argv);
void cmd_cartao(void);
int  cmd_lista(int argc, char** argv);
int  cmd_ler(int argc, char** argv);
int  cmd_despejar(int argc, char** argv);
int  cmd_apagar(int argc, char** argv);
void cmd_receber(int argc, char** argv);

// REDE
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

// BANCADA
void c_grove(int argc, char** argv);
int  cmd_i2c(int argc, char** argv);
void cmd_camera(int argc, char** argv);
void cmd_foto(void);

// TESTES
void cmd_concorrencia(int argc, char** argv);
void cmd_simulacao(int argc, char** argv);
