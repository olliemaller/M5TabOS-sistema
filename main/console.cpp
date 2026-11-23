
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  #include "driver/usb_serial_jtag.h"
  #include "driver/usb_serial_jtag_vfs.h"
#else
  #include "driver/uart.h"
  #include "driver/uart_vfs.h"
#endif
#include "tab5_arquivos.hpp"
#include "esp_app_desc.h"
#include "tab5_log.hpp"
#include "internal.hpp"   // sistema_trocar_relogio_painel

#include "console/console_interno.hpp"

static const char *TAG = "console";

// CEDE A CPU NAS LISTAGENS LONGAS. O console tem prioridade acima da IDLE, e
// imprimir uma lista grande sem nunca bloquear prende o nucleo: com ~200 nos
// do Mesh vindos pelo MQTT, o 'mesh' segurou o nucleo 0 por mais de 5 s e o
// watchdog reclamou da IDLE0. Um tick a cada 8 itens nao se nota na tela e
// deixa a IDLE (e quem mais estiver esperando) rodar.
void ceder(int i)
{
  if ((i & 7) == 7) { fflush(stdout); vTaskDelay(1); }
}

// ---------------------------------------------------------------------------
// Comandos dos apps
// ---------------------------------------------------------------------------
// A ideia: o que o app mostra na tela, o console mostra em texto. Nao e
// duplicacao - e o mesmo dado por dois caminhos, e o do console e o unico que
// funciona com o aparelho na bancada, sem ninguem olhando para a tela.


// ---------------------------------------------------------------------------
// SESSAO (console.hpp): quem digita - a serial ou o Terminal na tela. As
// duas rodam AO MESMO TEMPO (um ping longo no Terminal nao trava a serial),
// cada uma na sua tarefa: a sessao e achada pela tarefa que pergunta.
// ---------------------------------------------------------------------------
// A serial pergunta lendo a propria porta: quem roda o comando e a tarefa do
// console, a mesma que le os bytes. O que chegou ate o prazo fica em
// s_ser_linha para a proxima chamada (o shell do ssh pergunta em fatias).
static char   s_ser_linha[192];
static size_t s_ser_n = 0;
static bool   s_ser_perguntou = false;   // a pergunta desta linha ja saiu

static int serial_ler_linha(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms)
{
  if (pergunta && pergunta[0] && !s_ser_perguntou) { printf("%s", pergunta); fflush(stdout); }
  s_ser_perguntou = true;
  const int64_t fim = esp_timer_get_time() + (int64_t)espera_ms * 1000;
  for (;;) {
    const int64_t falta = fim - esp_timer_get_time();
    if (falta <= 0) { return 0; }
    uint8_t c = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(falta / 1000 + 1)) != 1) { continue; }
#else
    if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(falta / 1000 + 1)) != 1) { continue; }
#endif
    if (c == 3) { s_ser_n = 0; s_ser_perguntou = false; printf("^C\n"); return -1; }   // Ctrl+C
    if (c == '\r' || c == '\n') {
      printf("\n");
      s_ser_linha[s_ser_n] = 0;
      snprintf(out, n, "%s", s_ser_linha);
      memset(s_ser_linha, 0, sizeof(s_ser_linha));   // pode ser uma senha
      s_ser_n = 0;
      s_ser_perguntou = false;
      return 1;
    }
    if ((c == 8 || c == 127) && s_ser_n) { --s_ser_n; if (!oculto) { printf("\b \b"); } }
    else if (c >= 32 && s_ser_n < sizeof(s_ser_linha) - 1) {
      s_ser_linha[s_ser_n++] = (char)c;
      if (!oculto) { fputc(c, stdout); }
    }
    fflush(stdout);
  }
}

static console_sessao_t s_serial = { "/sdcard", nullptr, serial_ler_linha };

#define SESSOES_MAX 4
static struct { TaskHandle_t t; console_sessao_t* s; } s_ativas[SESSOES_MAX];
static portMUX_TYPE s_ativas_mux = portMUX_INITIALIZER_UNLOCKED;

// A sessao da tarefa que pergunta; fora de console_executar(), a da serial.
console_sessao_t* sessao(void)
{
  const TaskHandle_t eu = xTaskGetCurrentTaskHandle();
  console_sessao_t* r = &s_serial;
  taskENTER_CRITICAL(&s_ativas_mux);
  for (int i = 0; i < SESSOES_MAX; ++i) { if (s_ativas[i].t == eu) { r = s_ativas[i].s; } }
  taskEXIT_CRITICAL(&s_ativas_mux);
  return r;
}

bool console_interrompido(void) { const console_sessao_t* s = sessao(); return s->interrompido && s->interrompido(); }

int console_ler_linha(const char* pergunta, char* out, size_t n, bool oculto, uint32_t espera_ms)
{
  console_sessao_t* s = sessao();
  if (!s->ler_linha || console_interrompido()) { return -1; }
  return s->ler_linha(pergunta, out, n, oculto, espera_ms);
}

// Caminho do jeito do shell: absoluto como veio, relativo a partir da pasta
// atual da sessao ("/sdcard" no comeco), com "." e ".." resolvidos. Poupa
// digitar o prefixo em todo comando de arquivo ('ler apps/Sistema/x.txt').
void resolver(const char* c, char* out, size_t n)
{
  char tmp[TAB5_ARQ_CAMINHO_MAX];
  if (c[0] == '/') { snprintf(tmp, sizeof(tmp), "%s", c); }
  else             { snprintf(tmp, sizeof(tmp), "%s/%s", sessao()->cwd, c); }
  size_t k = 0;
  out[0] = 0;
  char* resto = nullptr;
  for (char* p = strtok_r(tmp, "/", &resto); p; p = strtok_r(nullptr, "/", &resto)) {
    if (!strcmp(p, ".")) { continue; }
    if (!strcmp(p, "..")) { char* b = strrchr(out, '/'); if (b) { *b = 0; k = (size_t)(b - out); } continue; }
    const int w = snprintf(out + k, n - k, "/%s", p);
    k = (w < 0 || k + (size_t)w >= n) ? n - 1 : k + (size_t)w;
  }
  if (!out[0]) { snprintf(out, n, "/"); }
}

const char* caminho_sd(const char* c, char* buf, size_t n)
{
  resolver(c, buf, n);
  return buf;
}


// ===========================================================================
// OS COMANDOS, EM TABELA
// ===========================================================================
// Cada comando e uma linha da tabela COMANDOS (mais abaixo): nome, grupo, uma
// frase de resumo, as formas de chamar e um paragrafo de para que serve. O
// 'help' e gerado dela - e por isso nao tem como ficar desatualizado: comando
// que nao esta na tabela nao roda, e o que esta aparece na ajuda.
//
// A versao anterior era um if/else de 40 ramos e um texto de ajuda escrito a
// parte. Os dois andavam separados: comando sem linha na ajuda, linha no grupo
// errado, subcomando que so existia no fonte.

// argv[de..] numa frase so, com um espaco entre as palavras.
void juntar(char* out, size_t n, int argc, char** argv, int de)
{
  out[0] = 0;
  for (int k = de; k < argc; ++k) {
    strncat(out, argv[k], n - strlen(out) - 1);
    if (k + 1 < argc) { strncat(out, " ", n - strlen(out) - 1); }
  }
}


// ---------------------------------------------------------------------------
// AJUDA
// ---------------------------------------------------------------------------

typedef enum { G_SISTEMA, G_SHELL, G_TELA, G_CARTAO, G_REDE, G_MESH, G_BANCADA, G_TESTES, G_APPS, G_N, G_OCULTO } grupo_t;

static const char* const GRUPO_TITULO_EN[G_N] = {
  "SYSTEM      what is running and what it costs",
  "SHELL       current folder and the usual commands (cd, ls, cat, cp...)",
  "DISPLAY     panel, brightness and touch",
  "SD CARD     files on the card",
  "NETWORK     Wi-Fi, Bluetooth, ESP-NOW and the C6 coprocessor",
  "MESHTASTIC  LoRa radio over Grove (C6L)",
  "BENCH       external hardware and camera",
  "TESTS       artificial load to prove the services",
  "APPS        app commands: app <app> <command>",
};
static const char* const GRUPO_TITULO[G_N] = {
  "SISTEMA     o que esta rodando e quanto custa",
  "SHELL       pasta atual e os atalhos de sempre (cd, ls, cat, cp...)",
  "TELA        painel, brilho e toque",
  "CARTAO      arquivos no SD",
  "REDE        WiFi, Bluetooth, ESP-NOW e o coprocessador C6",
  "MESHTASTIC  radio LoRa pelo Grove (C6L)",
  "BANCADA     hardware externo e camera",
  "TESTES      carga artificial para provar os servicos",
  "APPS        comandos dos aplicativos: app <app> <comando>",
};

typedef struct {
  const char* nome;
  const char* apelidos;   // separados por espaco; aceitos, mas fora da lista
  grupo_t     grupo;
  const char* resumo;     // a linha do 'help'
  const char* uso;        // uma forma por linha: "forma\to que faz"
  const char* detalhe;    // para que serve e cuidados - so no 'help <comando>'
  void (*fn)(int argc, char** argv);
  const char* exemplos;   // linhas prontas para copiar: "linha\to que faz"
} comando_t;

static void c_ajuda(int argc, char** argv);
static void imprimir_colunas(const char* texto, const char* prefixo, bool traduzir = false);

// ---------------------------------------------------------------------------
// COMANDOS DOS APPS - 'app <app> <comando> ...' (app_comando_t, kernel/app.h)
// ---------------------------------------------------------------------------
// O nome do app na frente e obrigatorio: um app nunca sombreia um comando do
// sistema nem o de outro app, e o 'help' mostra de quem e cada um.

// A proxima letra, minuscula e SEM ACENTO: o console da serial so passa
// ASCII (o "ú" de "Música" nem chega), entao "musica" tem que achar "Música".
// Latin-1 em UTF-8 (C3 80..C3 BF) vira a letra base; o resto passa como esta.
static char letra_simples(const char*& p)
{
  static const char BASE[] = "aaaaaaaceeeeiiiidnooooo*ouuuuy*s" "aaaaaaaceeeeiiiidnooooo*ouuuuy*y";
  const unsigned char c = (unsigned char)*p;
  if (c == 0xC3 && (unsigned char)p[1] >= 0x80 && (unsigned char)p[1] <= 0xBF) {
    const char b = BASE[(unsigned char)p[1] - 0x80];
    p += 2;
    return b;
  }
  ++p;
  return (char)tolower(c);
}

// "ESP-NOW", "esp-now", "espnow" sao o mesmo app; "Música" e "musica" tambem.
bool nome_de_app(const char* nome, const char* s)
{
  for (;;) {
    while (*nome == ' ' || *nome == '-') { ++nome; }
    while (*s == ' ' || *s == '-')       { ++s; }
    if (!*nome || !*s) { return !*nome && !*s; }
    if (letra_simples(nome) != letra_simples(s)) { return false; }
  }
}

static void ajuda_apps(const app_t* so_este)
{
  bool algum = false;
  for (int i = 0; i < SISTEMA_NUM_APPS; ++i) {
    const app_t* a = SISTEMA_APPS[i];
    if (!a->comandos || (so_este && a != so_este)) { continue; }
    for (const app_comando_t* c = a->comandos; c->nome; ++c) {
      char pre[48];
      snprintf(pre, sizeof(pre), "app %s %s ", a->nome, c->nome);
      imprimir_colunas(c->uso, pre);
      algum = true;
    }
  }
  if (!algum) { printf("%s", tr("  (nenhum app instalado oferece comandos)\n")); }
}

static void c_app(int argc, char** argv)
{
  const app_t* a = nullptr;
  for (int i = 0; argc >= 2 && i < SISTEMA_NUM_APPS; ++i) {
    if (nome_de_app(SISTEMA_APPS[i]->nome, argv[1])) { a = SISTEMA_APPS[i]; }
  }
  if (!a) {
    if (argc >= 2) { printf(tr("nao ha app '%s'\n"), argv[1]); }
    printf("%s", tr("uso: app <app> <comando> [argumentos]\n"));
    ajuda_apps(nullptr);
    return;
  }
  const app_comando_t* c = nullptr;
  for (const app_comando_t* k = a->comandos; argc >= 3 && k && k->nome; ++k) {
    if (eh(k->nome, argv[2])) { c = k; }
  }
  if (!c) {
    if (argc >= 3) { printf(tr("o app %s nao tem o comando '%s'\n"), a->nome, argv[2]); }
    ajuda_apps(a);
    return;
  }
  const char* atual = sistema_app_atual();
  if (c->so_aberto && (!atual || strcmp(atual, a->nome) != 0)) {
    printf(tr("abra o app %s primeiro ('abrir %s')\n"), a->nome, a->nome);
    return;
  }
  c->fn(argc - 2, argv + 2);
}


static const comando_t COMANDOS[] = {
  // --- SHELL ---
  { "cd", "", G_SHELL,
    N_("muda a pasta atual"),
    N_("cd <pasta>\trelativa a atual, ou absoluta (/sdcard, /usb)\n"
    "cd ..\tsobe uma\n"
    "cd\tvolta para /sdcard"),
    N_("Cada sessao (a serial e o Terminal) tem a sua pasta atual. Todo comando de\n"
    "arquivo aceita caminho relativo a ela. \"/\" e a raiz dos discos."),
    c_cd,
    N_("cd Música/Rock\tentra na pasta") },
  { "pwd", "", G_SHELL,
    N_("mostra a pasta atual"), N_("pwd\tonde voce esta"), N_("A pasta que os caminhos relativos usam."),
    c_pwd, N_("pwd\tpor exemplo /sdcard/Música") },
  { "ls", "dir", G_SHELL,
    N_("lista uma pasta (a atual, sem nada)"),
    N_("ls [pasta]\tpastas primeiro, com tamanho e tipo\n"
    "ls /\tos discos"),
    N_("O mesmo que 'arquivos ls': escondidos e senhas do sistema nao aparecem."),
    c_ls, N_("ls /usb\to pendrive") },
  { "cp", "", G_SHELL,
    N_("copia (em segundo plano)"), N_("cp <origem> <pasta>\tcopia para dentro da pasta"),
    N_("O mesmo que 'arquivos cp'. Acompanhe com 'arquivos'."), c_atalho_arquivos,
    N_("cp /usb/Rock /sdcard/Música\tcopia a pasta do pendrive") },
  { "mv", "", G_SHELL,
    N_("move (em segundo plano)"), N_("mv <origem> <pasta>\tmove para dentro da pasta"),
    N_("O mesmo que 'arquivos mv'. Para so trocar o nome: 'arquivos ren'."), c_atalho_arquivos,
    N_("mv foto.jpg Fotos\tmove para a pasta Fotos") },
  { "rm", "", G_SHELL,
    N_("manda para a lixeira do disco"), N_("rm <caminho>\tvai para a .Lixeira (nao some)"),
    N_("O mesmo que 'arquivos rm'. Ver e esvaziar: 'arquivos lixeira /sdcard'."), c_atalho_arquivos,
    N_("rm velho.txt\tpara a lixeira") },
  { "mkdir", "", G_SHELL,
    N_("cria uma pasta"), N_("mkdir <pasta nova>\trelativa a atual, ou absoluta"),
    N_("O mesmo que 'arquivos mkdir'."), c_atalho_arquivos, N_("mkdir Fotos\tcria /sdcard/Fotos") },
  { "df", "", G_SHELL,
    N_("espaco dos discos"), N_("df\ttotal e livre do cartao e do pendrive"),
    N_("Monta o disco para medir (o pendrive so se estiver plugado)."), c_df, N_("df\tquanto cabe ainda") },
  { "echo", "", G_SHELL,
    N_("repete o texto"), N_("echo <texto>\tescreve o texto"), N_("Para scripts e testes."), c_echo, N_("echo ola\tescreve ola") },
  { "clear", "cls limpar", G_SHELL,
    N_("limpa a tela"), N_("clear\tlimpa o Terminal (e o monitor serial)"), N_("A sequencia ANSI ESC[2J."),
    c_clear, N_("clear\ttela limpa") },

  // --- SISTEMA ---
  { "estado", "servicos", G_SISTEMA,
    N_("panorama: versao, tempo ligado, RAM e uma linha por servico"),
    N_("estado\tversao, app aberto, RAM, cartao, wifi, ble, mesh, energia"),
    N_("A primeira pergunta quando algo esta estranho: o que esta de pe agora?\n"
    "Cada servico responde em uma linha; o detalhe de cada um esta no comando\n"
    "dele (wifi, mesh, energia...)."),
    c_estado,
    N_("estado\tprimeiro passo antes de investigar qualquer problema") },
  { "tarefas", "tarefa ps top", G_SISTEMA,
    N_("quem esta rodando: CPU%, prioridade e folga de pilha"),
    N_("tarefas\tuma linha por tarefa do FreeRTOS, com o app dono"),
    N_("Le a ultima foto do monitor (1 s) sem parar o sistema. CPU% e do aparelho:\n"
    "os dois nucleos somam 100%, entao um nucleo cheio aparece como 50%.\n"
    "Pilha livre abaixo de ~500 B e risco de estouro. Digitar no serial nao\n"
    "acorda a tela, entao da para medir o repouso de verdade."),
    c_tarefas,
    N_("tarefas\tquem esta gastando CPU agora") },
  { "mem", "memoria free", G_SISTEMA,
    N_("RAM interna e PSRAM: uso, pico e fragmentacao"),
    N_("mem\tresumo: em uso, minimo ja visto, maior bloco\n"
    "mem extrato\tquanto cada etapa do boot consumiu\n"
    "mem mapa\tblocos por regiao e tamanho (fragmentacao)\n"
    "mem marca\ttira uma foto do heap (ponto de partida)\n"
    "mem delta\to que cresceu desde a marca (cacar vazamento)\n"
    "mem integridade\tconfere o heap: alguem escreveu fora do proprio bloco?\n"
    "mem tudo\tos tres juntos\n"
    "mem estresse [n] [linha]\tconfere uma linha da PSRAM zerando 1,8 MB a cada volta"),
    N_("O 'idf.py size' so conta o estatico; aqui aparece o que foi alocado em\n"
    "execucao (pilhas, buffers, framebuffers). O numero que diz se cabe mais um\n"
    "app e o 'minimo ja visto', nao o livre de agora. Muito livre com 'maior\n"
    "bloco' pequeno = fragmentacao."),
    c_mem,
    N_("mem\tquanto de RAM sobra\n"
    "mem extrato\tqual servico consumiu RAM no boot") },
  { "energia", "", G_SISTEMA,
    N_("bateria, corrente, clock e os ultimos 30 min"),
    N_("energia\ttensao, corrente, % e historico por minuto\n"
    "energia quem\tos recursos em uso: quem segura a suspensao e por que\n"
    "energia travas [seg]\tquem impede o light sleep (espera seg antes)\n"
    "energia repouso <mhz>\tforca o repouso agora nesse clock\n"
    "energia teto <mhz>\tlimita o CPU com a tela acesa (o maximo desfaz)\n"
    "energia sono [on|off]\tsono leve do P4 em repouso (contrato de economia)\n"
    "energia ensaio comecar [min] [fases]\tmede na bateria quanto cada servico gasta em repouso (fases 1 = so a base)\n"
    "energia ensaio [parar]\to resultado do ensaio (ou para e devolve os ajustes)"),
    N_("Os valores vem do cache que a interface atualiza 1x/s - o console nunca\n"
    "le o INA226 direto. 'travas' so vale medido em repouso: use um tempo maior\n"
    "que o tempo de tela de Ajustes e nao toque na tela durante a espera.\n"
    "Clocks validos: " TAB5_CPU_DEGRAUS "."),
    c_energia,
    N_("energia\tquanto a bateria esta gastando\n"
    "energia travas 40\tespera a tela apagar e mostra quem impede o sono\n"
    "energia teto " TAB5_CPU_METADE "\tCPU no maximo a " TAB5_CPU_METADE " MHz") },
  { "musica", "", G_SISTEMA,
    N_("o tocador do sistema: o que toca, a pasta, tocar e pausar"),
    N_("musica\to que toca, posicao e se o som falhou (fila secou)\n"
    "musica lista [subpasta]\tpastas e musicas, numeradas\n"
    "musica tocar <n> [subpasta]\ttoca a pasta a partir do item n da lista\n"
    "musica pausar|proxima|anterior|parar\tos controles\n"
    "musica aleatorio [on|off]\tembaralhar a fila\n"
    "musica repetir [nao|tudo|uma]\to que acontece no fim\n"
    "musica playlists\tas playlists (.m3u em Music/Playlists)\n"
    "musica playlist <nome>\tas musicas dela, com a duracao\n"
    "musica playlist <nome> criar|apagar|tocar [n]\tmexer nela\n"
    "musica playlist <nome> por|tirar <caminho>\tcaminho relativo a pasta de musica\n"
    "musica capa\ta capa do que toca (etiqueta, pasta ou internet)\n"
    "musica capa internet ligar|desligar\tbuscar a capa que falta no iTunes (titulo e artista saem do aparelho)\n"
    "musica duracao <caminho>\tquanto dura um arquivo, sem tocar"),
    N_("O mesmo servico do app Musica e do modulo Berry 'musica'. A pasta e a\n"
    "Music (ou Música, Musicas...) da raiz do cartao."),
    c_musica,
    N_("musica tocar 0\ttoca a primeira musica da pasta de musica") },
  { "video", "", G_SISTEMA,
    N_("o tocador de video do sistema: abrir, pausar, ir para, estado"),
    N_("video\to que toca, posicao, quadros por segundo e quantos foram pulados\n"
    "video abrir <caminho>\tabre e toca (.mp4 com o som de dentro; .avi com o .mp3 de mesmo nome)\n"
    "video pausar\tpausa ou volta a tocar (no fim, toca de novo)\n"
    "video ir <s>\tvai para o segundo s\n"
    "video fechar\tpara o video e o som\n"
    "video h264 <arq.h264> [sram|psram] [hash] [fundo]\tbancada do decodificador H.264: mede (e com hash confere com o Mac)"),
    N_("AVI com video MJPEG (ferramentas/video_para_tab5.sh gera no Mac). O som\n"
    "manda no tempo: se a decodificacao atrasa, os quadros sao pulados."),
    c_video,
    N_("video abrir /sdcard/Videos/teste.avi\ttoca o video de teste") },
  { "fundo", "", G_SISTEMA,
    N_("partes de apps que seguem rodando com a janela fechada"),
    N_("fundo\to que esta rodando e ha quanto tempo\n"
    "fundo parar <nome>\tpede para uma delas parar"),
    N_("Parte de fundo e o pedaco de um app que continua com a janela fechada\n"
    "(ex.: o cronometro do Exemplo). A parada acontece na interface, no proximo ciclo."),
    c_fundo,
    N_("fundo parar Exemplo\tpara o cronometro do Exemplo") },
  { "travamento", "", G_SISTEMA,
    N_("o ultimo travamento guardado na flash (core dump)"),
    N_("travamento\ttarefa, PC, RA e os enderecos da pilha"),
    N_("Um panic ou watchdog grava o retrato na particao 'coredump'; o boot\n"
    "seguinte ja anota isto no log do cartao. Com o .elf da mesma versao:\n"
    "riscv32-esp-elf-addr2line -pfiaC -e build/M5tabOS.elf <enderecos>."),
    [](int, char**) { tab5_log_travamento(true); },
    N_("travamento\tdepois de um reinicio inesperado") },
  { "log", "", G_SISTEMA,
    N_("ve ou muda o nivel de log de um modulo"),
    N_("log <etiqueta>\tmostra o nivel atual\n"
    "log <etiqueta> <nivel>\tnada erro aviso info debug verboso (ou 0-5)\n"
    "log * <nivel>\ttodos os modulos de uma vez"),
    N_("O sistema silencia alguns modulos barulhentos no boot (eh_sdio, sdmmc_io).\n"
    "Isto devolve o log cru deles sem regravar. Ex.: log eh_sdio debug.\n"
    "Reiniciar volta tudo ao normal."),
    c_log,
    N_("log eh_sdio debug\tver o log cru do enlace com o C6\n"
    "log mesh\tem que nivel esta o log do Mesh\n"
    "log * aviso\tso avisos e erros, de todos os modulos") },
  { "boasvindas", "primeiro-uso", G_SISTEMA,
    N_("abre o assistente de primeiro uso (idioma, fuso, malha, Wi-Fi)"),
    N_("boasvindas\tabre agora, como num aparelho novo"),
    N_("O assistente so abre sozinho num aparelho que nunca rodou o sistema (ou\n"
    "gravado pelo M5Burner). Isto e para testar. Concluir marca como feito."),
    c_boasvindas },
  { "fuso", "tz", G_SISTEMA,
    N_("fuso horario: a regiao (com horario de verao), o fuso manual e o relogio 12/24 h"),
    N_("fuso\tregiao, deslocamento agora e a hora\n"
    "fuso lista\tas regioes, com o numero de cada uma\n"
    "fuso <n>\tescolhe a regiao n (0 = manual)\n"
    "fuso manual <minutos>\tdeslocamento fixo, ex. 600 = UTC+10\n"
    "fuso 12h|24h\to formato do relogio"),
    N_("O mesmo que Ajustes > Data e hora. Com uma regiao, o horario de verao muda\n"
    "sozinho. So a hora mostrada muda: o relogio interno e os registros ficam em UTC."),
    c_fuso,
    N_("fuso 1\tMelbourne, Sydney (AEST/AEDT)\nfuso 12h\trelogio de 12 horas") },
#if M5_COM_BERRY
  { "berry", "", G_SISTEMA,
    N_("roda codigo Berry (o interpretador de script)"),
    N_("berry <codigo>\troda uma linha e mostra o valor\n"
    "berry rodar <arquivo>\troda um .be do cartao\n"
    "berry verificar <arquivo>\tcompila sem rodar: acha o erro de sintaxe e a linha\n"
    "berry reiniciar\tapaga as variaveis (maquina nova)\n"
    "berry reler\trele apps/Berry do cartao (icones novos sem reiniciar)\n"
    "berry mem\tmemoria de cada maquina Berry (apps, fundos, console) e o orcamento\n"
    "berry tempos [zerar]\tquanto o script leva em cada evento, e a entrega a tarefa"),
    N_("Uma maquina so, mantida entre comandos: o que uma linha define a proxima\n"
    "enxerga. Aspas protegem espacos: berry \"print('ola mundo')\". Limite de\n"
    "10 s por comando. Os apps Berry ficam em /sdcard/apps/Berry (ver docs/BERRY.md)."),
    c_berry,
    N_("berry 1+2*3\tmostra 7\n"
    "berry rodar teste.be\troda /sdcard/teste.be") },
#endif
  { "pendrive", "", G_SISTEMA,
    N_("liga o cartao como disco USB no computador, sem tocar na tela"),
    N_("pendrive\testado da ultima sessao\n"
    "pendrive ligar\to cartao vai para o computador\n"
    "pendrive devolver\to cartao volta (so com o console de volta)"),
    N_("Ligar tira o console do USB-C: a serial some e o computador ve o disco.\n"
    "O console volta quando o computador ejeta (ou devolvendo na tela).\n"
    "O caminho da sessao fica no log do cartao (tag 'pendrive')."),
    c_pendrive,
    N_("pendrive ligar\tmontar o cartao no computador") },
  { "usb", "usba", G_SISTEMA,
    N_("a porta USB-A: o que esta plugado, a velocidade, o pendrive"),
    N_("usb\to aparelho, a velocidade negociada e o pendrive em /usb\n"
    "usb ler <arquivo>\tle o arquivo inteiro do pendrive e mede KB/s\n"
    "usb espaco\ttamanho e espaco livre (pode demorar)\n"
    "usb ejetar\tdesmonta para tirar com seguranca (depois de gravar nele)\n"
    "usb ligar|desligar\to ajuste: os 5V do conector e a pilha USB"),
    N_("Pendrive FAT32 ou exFAT monta sozinho em /usb (NTFS e os do Mac nao).\n"
    "A Musica toca dele: 'musica lista /usb', 'musica tocar 0 /usb'."),
    c_usb,
    N_("usb ler Rock/musica.mp3\tquanto o pendrive rende lendo") },
  { "arquivos", "arq", G_CARTAO,
    N_("o motor de arquivos: listar, copiar, mover, lixeira (cartao e pendrive)"),
    N_("arquivos\to trabalho em curso e a area de transferencia\n"
    "arquivos ls <pasta> [.mp3;.wav]\tlista o que o app Arquivos mostra\n"
    "arquivos cp|mv <origem> <pasta>\tcopia / move em segundo plano\n"
    "arquivos rm <caminho>\tmanda para a lixeira do disco\n"
    "arquivos mkdir <pasta> <nome>\tcria uma pasta\n"
    "arquivos ren <caminho> <nome>\trenomeia\n"
    "arquivos copiar|recortar <caminho>\tpoe na area de transferencia\n"
    "arquivos colar <pasta>\tcola a area ali\n"
    "arquivos lixeira /sdcard|/usb\tlista a lixeira (origem, data, tamanho)\n"
    "arquivos lixeira <raiz> restaurar <nome>\tdevolve o item para onde estava\n"
    "arquivos lixeira <raiz> excluir <nome>\tapaga UM item de vez\n"
    "arquivos lixeira <raiz> esvaziar\tesvazia a lixeira DE VEZ\n"
    "arquivos lixeira <raiz> antigos <dias>\texclui de vez o que esta ha mais de N dias\n"
    "arquivos cancelar\tpara a copia (o arquivo pela metade sai)"),
    N_("Caminhos absolutos: /sdcard/... (cartao) e /usb/... (pendrive).\n"
    "/sdcard/apps e so leitura (menos apps/Berry); nomes com '.' e as senhas\n"
    "do sistema ficam escondidos. Nome repetido no destino vira 'x (2).mp3'.\n"
    "Copiar para o pendrive: 'usb ejetar' antes de tirar."),
    c_arquivos,
    N_("arquivos cp /usb/Rock /sdcard/Música\tcopia a pasta do pendrive para a Musica") },

  // --- TELA ---
  { "tela", "", G_TELA,
    N_("painel: quadros, piscada azul, relogio e prioridade"),
    N_("tela\testado do painel e da janela de medida\n"
    "tela medir\tzera a janela; use o aparelho e rode 'tela'\n"
    "tela foto [nome]\tgrava a tela em apps/Console/<nome>.bmp (sem nome: com data e hora)\n"
    "tela banco\tcompara os motores de copia (PPA, DMA2D, CPU)\n"
    "tela relogio <mhz>\ttroca o relogio de pixel e grava (80 = fabrica)\n"
    "tela prioridade alta|padrao\tprioridade da tela no barramento AXI\n"
    "tela rolar cpu|dma\tquem faz a copia da rolagem\n"
    "tela irqs\tlista as interrupcoes (nucleo, nivel, fonte)\n"
    "tela atrasar <ms> [critica] [nucleo] [vezes]\tprova: segura um nucleo; a tela pisca?"),
    N_("Para investigar piscada e fluidez. 'underrun' acima de zero = o painel\n"
    "ficou sem dado a tempo, que e a piscada azul. Palavra nao reconhecida nao\n"
    "altera nada."),
    c_tela,
    N_("tela medir\tzera; use o aparelho; depois rode tela\n"
    "tela relogio 80\tvolta ao relogio de fabrica do painel\n"
    "tela prioridade padrao\tdesliga a prioridade para comparar") },
  { "brilho", "", G_TELA,
    N_("testa o backlight sem gravar"),
    N_("brilho\tvalor atual e o gravado\n"
    "brilho <1-255> [hz]\taplica (e muda a frequencia do PWM)"),
    N_("Para achar o menor brilho que ainda acende. Nao grava: Ajustes ou\n"
    "reiniciar voltam ao valor salvo. Se a tela sumir, 'brilho 80' a traz de volta."),
    c_brilho,
    N_("brilho 6\to menor brilho ainda acende?\n"
    "brilho 80\ttraz a tela de volta se ela sumir") },
  { "piscada", "", G_TELA,
    N_("dispara UMA causa possivel da piscada azul"),
    N_("piscada brilho\treescreve o mesmo brilho\n"
    "piscada c6\treescreve o pino de energia do C6\n"
    "piscada perfil\treaplica o perfil de energia inteiro\n"
    "piscada pm\treconfigura o gerenciador de energia\n"
    "piscada grove\tcorta o 5V do Grove por 1 s\n"
    "piscada quadro\tredesenho completo da tela"),
    N_("Abrir um app faz varias coisas ao mesmo tempo; cada opcao faz so uma e\n"
    "anota a hora. Olhe a tela e veja qual pisca."),
    c_piscada,
    N_("piscada perfil\treaplicar o perfil faz piscar?") },
  { "toque", "", G_TELA,
    N_("mostra onde cada toque cai na tela"),
    N_("toque\tliga: posicao crua e convertida de cada toque\n"
    "toque off\tdesliga\n"
    "toque reiniciar\treinicia o chip do toque (GT911) pelo pino de reset\n"
    "toque regs\to que o GT911 diz de si (firmware, configuracao) no log"),
    N_("Para conferir se um botao recebe o dedo onde deveria. Quem le o painel e\n"
    "a interface; o console so liga a bandeira."),
    c_toque,
    N_("toque\tdepois toque nos botoes e veja a posicao no log") },
  { "tocar", "", G_TELA,
    N_("clique simulado (bancada)"),
    N_("tocar <x> <y>\tclica em x,y de tela, como o dedo"),
    N_("Para testar uma tela pela serial. Acorda a tela se estiver em repouso\n"
    "(e esse primeiro pedido nao vira clique, como o dedo)."),
    c_tocar,
    N_("tocar 360 640\tclica no meio da tela") },
  { "deslizar", "", G_TELA,
    N_("arraste simulado (bancada)"),
    N_("deslizar <x0> <y0> <x1> <y1>\tarrasta de um ponto a outro, como o dedo"),
    N_("Desbloqueia (para cima), fecha ou rola a central de avisos, troca a\n"
    "pagina da home. So o gesto: o app aberto nao recebe o arraste."),
    [](int c, char** v) {
      if (c < 5) { mostrar_uso("deslizar"); return; }
      sistema_simular_gesto(atoi(v[1]), atoi(v[2]), atoi(v[3]), atoi(v[4]));
      printf("arraste %s,%s -> %s,%s\n", v[1], v[2], v[3], v[4]);
    },
    N_("deslizar 360 1000 360 400\tdesliza para cima (desbloqueia)") },
  { "abrir", "", G_TELA,
    N_("abre um app pela serial (bancada)"),
    N_("abrir\tlista os apps\n"
    "abrir <nome>\tabre (fecha o que estiver aberto)\n"
    "abrir home\tvolta para a tela inicial"),
    N_("Junto de 'tocar' e 'tela foto', testa uma tela sem tocar no aparelho."),
    c_abrir,
    N_("abrir Arquivos\tdepois 'tela foto'") },
  { "ordem", "", G_TELA,
    N_("ordem dos icones na tela inicial"),
    N_("ordem\tlista os apps na ordem da home\n"
    "ordem <nome> <posicao>\tmove o app para a posicao (1 = primeiro)\n"
    "ordem padrao\tvolta para a ordem de apps.cpp"),
    N_("Gravada na NVS: vale depois de reiniciar, sem compilar. Na propria tela:\n"
    "segure um icone ate a moldura aparecer e arraste; para outra pagina,\n"
    "segure o icone na beirada lateral."),
    c_ordem,
    N_("ordem Exemplo 1\tExemplo vira o primeiro icone") },
  { "ui", "", G_TELA,
    N_("onde a interface esta agora (tela travada?)"),
    N_("ui\tetapa atual, voltas do laco, ultimo toque, saude do GT911"),
    N_("Funciona com a tela travada, porque o console e outra tarefa. Rode duas\n"
    "vezes: se 'voltas' nao mudou, o laco da interface parou nessa etapa."),
    c_ui,
    N_("ui\trode duas vezes com a tela travada e compare") },

  // --- CARTAO ---
  { "cartao", "", G_CARTAO,
    N_("espaco livre e o conteudo de cada pasta de app"),
    N_("cartao\tespaco, gravador do sistema e ate 10 arquivos por pasta"),
    N_("O panorama do SD. Para uma pasta especifica, use 'lista'."),
    [](int, char**) { cmd_cartao(); },
    N_("cartao\tquanto espaco sobra no SD") },
  { "lista", "", G_CARTAO,
    N_("lista uma pasta"),
    N_("lista [caminho]\tpadrao /sdcard (ex.: lista apps/Sistema)"),
    N_("Caminho relativo e lido a partir de /sdcard."),
    [](int c, char** v) { cmd_lista(c, v); },
    N_("lista apps/Mesh\tarquivos do app Mesh") },
  { "ler", "cat", G_CARTAO,
    N_("mostra um arquivo de texto"),
    N_("ler <caminho> [linhas]\tate 200 linhas, ou quantas pedir"),
    N_("Para olhar. Para copiar o arquivo inteiro para o Mac, use 'despejar'.\n"
    "Caminho relativo e lido a partir de /sdcard."),
    [](int c, char** v) { cmd_ler(c, v); },
    N_("ler apps/Sistema/logs/log_2026-10-01.txt 50\tas 50 primeiras linhas do log do dia") },
  { "despejar", "", G_CARTAO,
    N_("manda o arquivo inteiro, para salvar no Mac"),
    N_("despejar <caminho>\tsem limite, entre marcadores >>>INICIO/>>>FIM\n"
    "despejar -b <caminho>\tarquivo binario (foto), em base64"),
    N_("Cala o log durante o envio para nenhuma linha cair no meio do arquivo.\n"
    "No Mac, com o monitor fechado: ./ferramentas/baixar.py <caminho>"),
    [](int c, char** v) { cmd_despejar(c, v); },
    N_("despejar apps/Sistema/energia/energia-2026-10-01.csv\tcopia para o Mac (use ferramentas/baixar.py)") },
  { "apagar", "", G_CARTAO,
    N_("apaga um arquivo do cartao"),
    N_("apagar <caminho>\tum arquivo por vez, sem curinga"),
    N_("Irreversivel. So aceita caminhos dentro de /sdcard e sem '..'."),
    [](int c, char** v) { cmd_apagar(c, v); },
    N_("apagar apps/Console/console.bmp\tapaga a foto de teste") },
  { "receber", "", G_CARTAO,
    N_("recebe um arquivo pela serial e grava no cartao"),
    N_("receber <app> <arquivo> <bytes>\tgrava em /sdcard/apps/<app>/<arquivo>"),
    N_("Depois do comando, mande o arquivo cru pela mesma porta, com o monitor\n"
    "fechado. Tudo de uma vez, do Mac:\n"
    "  { printf 'receber Sistema x.bin 1234\\r'; sleep 2; cat x.bin; } > /dev/cu.usbmodem*\n"
    "Confira o md5 impresso no fim com 'md5 -q x.bin'. Ate 6 MB."),
    [](int c, char** v) { cmd_receber(c, v); },
    N_("receber Sistema tab5_c6.bin 1397904\tdepois mande o arquivo pela porta") },

  // --- REDE ---
  { "wifi", "", G_REDE,
    N_("estado, redes em volta e redes guardadas"),
    N_("wifi\testado: rede, IP, sinal\n"
    "wifi redes\tprocura as redes em volta, sem soltar a atual\n"
    "wifi gravar <ssid> [senha]\tguarda a rede (sem senha = aberta)\n"
    "wifi conectar <ssid>\ttroca para uma rede ja guardada\n"
    "wifi esquecer <ssid>\ttira a rede do wifi.txt\n"
    "wifi ligar | desligar\tliga ou desliga o Wi-Fi (como no Ajustes)"),
    N_("Use \"aspas\" se o nome ou a senha tiver espaco. A senha e gravada cifrada.\n"
    "'wifi <ssid> [senha]' (sem 'gravar') tambem grava. 'wifi redes' e a mesma procura\n"
    "de Ajustes > Wi-Fi e exige o WiFi ligado ao menos uma vez neste boot."),
    c_wifi,
    N_("wifi\testou conectado? em que rede?\n"
    "wifi redes\tquais redes estao em volta\n"
    "wifi gravar \"Casa 2G\" minhasenha\tguarda uma rede com espaco no nome\n"
    "wifi conectar \"Casa 2G\"\ttroca para ela agora") },
  { "ble", "", G_REDE,
    N_("Bluetooth: estado, enlace do Mesh e aparelhos vistos"),
    N_("ble\testado e aparelhos vistos\n"
    "ble varrer\tprocura por 6 s (rode 'ble' depois)\n"
    "ble ligar\tliga o Bluetooth (como o botao de Ajustes)\n"
    "ble desligar\tdesliga o Bluetooth (como o botao de Ajustes)"),
    N_("Aparelhos Meshtastic aparecem marcados - sao os que o Mesh pode usar\n"
    "pelo Bluetooth."),
    c_ble,
    N_("ble varrer\tprocura; depois rode ble para ver a lista") },
  { "espnow", "", G_REDE,
    N_("ESP-NOW: estado, canal e envio de teste"),
    N_("espnow\tfirmware do C6, canal real, MAC e contadores\n"
    "espnow canal <0-13>\t0 = segue o roteador; 1-13 = fixo\n"
    "espnow enviar <texto>\tdifunde uma mensagem\n"
    "espnow para <MAC> <texto>\tmanda DIRETO para um aparelho\n"
    "espnow todos [<MAC>] <texto>\tmanda em todos os canais (1-13)\n"
    "espnow msgs [n]\tas ultimas n mensagens, com os bytes em hexa"),
    N_("Dois aparelhos so se ouvem no mesmo canal. O canal mostrado e lido do C6\n"
    "agora, nao o que o servico acha. Exige o firmware proprio do C6.\n"
    "'espnow todos' solta o roteador: o Wi-Fi cai durante a varredura (alguns\n"
    "segundos) e volta sozinho."),
    c_espnow,
    N_("espnow canal 1\tfixa no canal 1 (o mesmo do outro aparelho)\n"
    "espnow enviar ola Cardputer\tmensagem de teste\n"
    "espnow para AA:BB:CC:DD:EE:01 ola\tmensagem direta\n"
    "espnow todos ola\tdifunde em todos os canais\n"
    "espnow msgs 5\tas ultimas 5 recebidas/enviadas") },
  { "c6", "", G_REDE,
    N_("coprocessador C6: firmware, MAC e gravacao pelo enlace"),
    N_("c6\tversao do firmware e MACs\n"
    "c6 gravar\tcomo gravar firmware novo no C6 pelo enlace (sem abrir)\n"
    "c6 gravar sim\tgrava apps/Sistema/tab5_c6.bin no C6\n"
    "c6 dormir\tdesmonta o enlace e deixa o C6 dormir associado (prova; Bluetooth desligado)\n"
    "c6 acordar\ttraz o enlace de volta pela linha CMD\n"
    "c6 sono\to estado e os contadores dos dois lados\n"
    "c6 sono auto ligar|desligar\to C6 dorme sozinho com a tela apagada (fica gravado)\n"
    "c6 janela <intervalo> <janela>\tquanto o ESP-NOW deixa o radio do C6 acordado (ms)\n"
    "c6 travas\tas travas de PM do C6 (quem impede o light sleep dele)\n"
    "c6 laco <N>\tteste: dorme e acorda N vezes com esperas sorteadas"),
    N_("Exige o WiFi ligado. A gravacao confere a imagem antes, e o C6 volta\n"
    "sozinho para a anterior se a nova nao for confirmada."),
    c_c6,
    N_("c6\tqual firmware esta no C6") },
  { "farejo", "", G_REDE,
    N_("modo promiscuo no C6: quem esta no ar em volta"),
    N_("farejo\testado e os MACs ouvidos (RSSI, tipo, quadros)\n"
    "farejo on [lote_ms] [mask_hex]\tliga; mask = canais a saltar (solte o roteador)\n"
    "farejo off\tdesliga\n"
    "farejo info\tcapacidades de radio do C6 gravado (CSI/FTM/802.15.4)\n"
    "farejo limpar\tzera a tabela"),
    N_("O C6 junta por MAC e manda em lotes - o P4 so recebe o resumo. Deixa o\n"
    "receptor ligado enquanto ativo; desliga sozinho se a tela que usa fechar.\n"
    "Saltar varios canais exige o WiFi solto do roteador ('espnow canal 1')."),
    c_farejo,
    N_("farejo info\teste C6 sabe CSI/FTM/802.15.4?\n"
    "farejo on\tcomeca a ouvir no canal atual\n"
    "farejo on 200 0x1452\tlote 200 ms, salta canais 1/6/11/13 (solte o roteador antes)") },

  { "ping", "", G_REDE,
    N_("o WiFi passa dados? a internet responde?"),
    N_("ping\troteador e 8.8.8.8, 4 pacotes cada\n"
    "ping <ip|nome>\tum alvo so"),
    N_("WiFi 'conectado' so diz que o C6 esta associado ao roteador. Se o roteador\n"
    "nao responde, o caminho de dados (C6/SDIO) parou; se so a internet nao\n"
    "responde, o problema esta depois do roteador."),
    c_ping,
    N_("ping\tos dois de uma vez\n"
    "ping platform.meshbrasil.com\to servidor do MQTT do Mesh") },
  { "ip", "ifconfig", G_REDE,
    N_("o endereco do aparelho na rede"),
    N_("ip\tIPv4, mascara, gateway, DNS, IPv6, MAC e nome"),
    N_("O Wi-Fi e do C6, mas o IP (lwIP) roda aqui no P4."),
    c_ip, N_("ip\tonde o Tab5 esta na rede") },
  { "dns", "nslookup host", G_REDE,
    N_("resolve um nome (IPv4 e IPv6)"),
    N_("dns <nome>\tos enderecos e quanto o DNS levou"),
    N_("Nao resolveu com o Wi-Fi conectado: o DNS da rede caiu, ou o nome nao existe."),
    c_dns, N_("dns google.com\to IP do Google") },
  { "arp", "", G_REDE,
    N_("quem esta na rede local"),
    N_("arp\ta tabela ARP (quem falou com o Tab5 ha pouco)\n"
    "arp varrer\tpergunta os 254 enderecos do /24 (~15 s)"),
    N_("A varredura acha ate aparelho que nao responde ping: todo mundo responde ARP."),
    c_arp, N_("arp varrer\tos aparelhos da sua rede, com o MAC") },
  { "traceroute", "tracert", G_REDE,
    N_("o caminho ate um servidor, salto a salto"),
    N_("traceroute <ip|nome> [saltos]\tate 20 saltos; 3 sondas de 1 s cada"),
    N_("Cada roteador do caminho responde quando o TTL zera nele. '*' = ele nao responde\n"
    "ICMP (comum), nao que o caminho caiu. Ctrl+C para."),
    c_traceroute, N_("traceroute 8.8.8.8\tate o DNS do Google") },
  { "tcp", "nc porta", G_REDE,
    N_("a porta de um servidor esta aberta?"),
    N_("tcp <host> <porta>\tconecta e fecha (5 s de prazo)"),
    N_("Separa 'o servidor caiu' (recusou / sem resposta) de 'o servico nao responde'."),
    c_tcp, N_("tcp 10.0.0.23 8989\to GitLab de casa responde?") },
  { "http", "curl wget", G_REDE,
    N_("busca uma pagina (HTTP e HTTPS)"),
    N_("http <url>\tstatus, tipo e o comeco do texto (4 KB)\n"
    "http -I <url>\tso o cabecalho\n"
    "http -n <bytes> <url>\tmostra mais (ou menos) do corpo\n"
    "http -o <arquivo> <url>\tbaixa para o disco (relativo a pasta atual)\n"
    "wget <url>\tbaixa para a pasta atual, com o nome do fim da URL"),
    N_("Segue redirecionamentos. HTTPS confere o certificado pelo pacote do sistema.\n"
    "Conteudo binario (imagem, zip) so mostra o resumo."),
    c_http, N_("http ifconfig.me/ip\to seu IP na internet") },
  { "ssh", "", G_REDE,
    N_("entra noutra maquina (cliente SSH)"),
    N_("ssh usuario@host\tno Terminal (tela ou navegador): o terminal remoto inteiro;\n"
    "\tna serial: shell linha a linha ('exit' sai)\n"
    "ssh usuario@host <comando>\troda o comando la e mostra a saida\n"
    "ssh -p <porta> usuario@host ...\toutra porta (ou usuario@host:porta)\n"
    "ssh fechar\tencerra a sessao aberta (a da tela e do navegador)\n"
    "ssh esquecer <host>[:porta]\tesquece a chave guardada do servidor"),
    N_("No Terminal a sessao e UMA, compartilhada: a tela do Tab5 e a pagina\n"
    "/terminal/ do navegador mostram a mesma (nano, top e vim funcionam); 'exit'\n"
    "volta aos comandos. A chave do servidor (SHA256) fica guardada na primeira\n"
    "vez (apps/SSH/known_hosts); se mudar, recusa. Ainda nao: chave privada."),
    c_ssh, N_("ssh pi@192.168.1.10 uname -a\to Linux do Raspberry") },
  { "web", "pc", G_REDE,
    N_("usa o Tab5 pelo navegador do PC"),
    N_("web\to endereco, a senha e os apps abertos\n"
    "web ligar [app]\tabre o app no PC (sem nome: o Terminal)\n"
    "web desligar\tfecha tudo"),
    N_("O endereco e o portal (http://<ip>/): a visao geral do aparelho e os apps\n"
    "que tem pagina no PC; os fechados abrem com um clique. So na rede local, com\n"
    "a senha da tela; cada app fecha sozinho apos 10 min sem uso."),
    c_web, N_("web ligar\to Terminal no navegador") },
  { "busca", "pesquisa wiki", G_REDE,
    N_("pesquisa na internet (Wikipedia e DuckDuckGo)"),
    N_("busca <termo>\to resumo do artigo da Wikipedia em portugues"),
    N_("Sem artigo, a resposta instantanea do DuckDuckGo. Nao precisa de conta nem chave."),
    c_busca, N_("busca ESP32\to que e o ESP32") },
  { "radio", "", G_REDE,
    N_("teste dos extras do C6: CSI, FTM, 802.15.4"),
    N_("radio csi [s]\tliga o CSI por s segundos (10), so do roteador, pingando-o a 20 Hz como\n"
    "\tfonte; conta os records por formato (11g/HT/HE) e mostra a medida de movimento\n"
    "radio 154 [canal] [s]\touve o 802.15.4 no canal (11..26, padrao 15); pinga o roteador\n"
    "\tantes, durante e depois para provar que o WiFi segue passando dados\n"
    "radio ftm\tmede a distancia ate o roteador conectado por FTM"),
    N_("Ainda nao ha app que use estes dados: o comando prova que a gravacao do\n"
    "C6 liga, recebe e desliga cada recurso sem derrubar o C6 nem o WiFi.\n"
    "Depois de cada teste, 'farejo info' deve mostrar o C6 de pe (no ar sem\n"
    "zerar). FTM so mede distancia se o roteador falar FTM (802.11mc)."),
    c_radio,
    N_("radio csi\tCSI por 10 s: perto de 100 records (1 a cada 100 ms)\n"
    "radio 154 15 20\t802.15.4 no canal 15 por 20 s: os tres pings em 4/4\n"
    "radio ftm\to caminho do FTM ate o roteador") },

  // --- MESHTASTIC ---
  { "mesh", "", G_MESH,
    N_("Meshtastic: enlace, nos, mensagens e envio"),
    N_("mesh\tficha completa: enlace, saude, nos e mensagens\n"
    "mesh nos\tlista curta: numero e nome de cada no\n"
    "mesh msgs [N]\tultimas N mensagens de todas as conversas\n"
    "mesh enviar <texto>\tdifunde no canal principal (LongFast)\n"
    "mesh enviar para <no> <texto>\tmensagem direta (numero hex ou nome curto)\n"
    "mesh casa [lat lon]\tmostra ou grava a posicao de referencia\n"
    "mesh pausar\tpara de falar com o radio (o 5V continua)\n"
    "mesh retomar\tvolta a falar com ele"),
    N_("Atalhos: 'nos' e 'enviar ...' funcionam sem o 'mesh' na frente. 'msgs'\n"
    "mostra o tamanho em bytes de cada texto, para separar mensagem cortada na\n"
    "captura de cortada no desenho. 'pausar' isola se um problema vem do\n"
    "software ou da posicao do modulo. Mensagem direta para no que veio pela\n"
    "internet (via MQTT) e recusada: por la so vai mensagem de canal."),
    c_mesh,
    N_("mesh nos\tacha o nome curto antes de mandar direta\n"
    "mesh enviar bom dia a todos\tmensagem no canal principal\n"
    "mesh enviar para RP05 oi\tdireta ao no de nome curto RP05\n"
    "mesh enviar para ACA97F24 oi\tdireta, pelo numero do no\n"
    "mesh msgs 50\tas 50 mensagens mais recentes\n"
    "mesh casa -22.88 -42.79\tgrava a posicao de casa") },

  // --- BANCADA ---
  { "grove", "", G_BANCADA,
    N_("porta Grove: o que esta plugado e quem a usa"),
    N_("grove\tmodulo detectado, energia e dono da porta\n"
    "grove trocar\tsonda de novo depois de trocar o modulo\n"
    "grove cru <baud>\t2,5 s de bytes crus (ex.: grove cru 115200)"),
    N_("'grove' so relata: sondar com o Mesh no ar embaralharia a UART. 'trocar'\n"
    "para o servico, sonda e sobe o que couber - feche antes o app que usa o Grove."),
    c_grove,
    N_("grove cru 9600\tum GPS esta falando no cabo?\n"
    "grove trocar\tdepois de plugar outro modulo") },
  { "i2c", "", G_BANCADA,
    N_("procura dispositivos I2C num par de pinos"),
    N_("i2c <sda> <scl>\tvarre os enderecos (ex.: i2c 49 50)"),
    N_("Para identificar hardware novo. Teste o par nos dois sentidos (49 50 e\n"
    "50 49). So roda na tela inicial: o barramento externo e o mesmo do GPS e\n"
    "do teclado."),
    [](int c, char** v) { cmd_i2c(c, v); },
    N_("i2c 49 50\tprocura no conector lateral\n"
    "i2c 50 49\to mesmo par, invertido") },
  { "camera", "", G_BANCADA,
    N_("diagnostico de cor da camera (AWB, CCM, preto)"),
    N_("camera\tmede um quadro: centro, sombras, estourado, magenta\n"
    "camera reg <hex> [valor]\tle ou escreve um registrador do sensor"),
    N_("Liga a camera se preciso e espera 2,5 s o AE/AWB assentar. Com a lente\n"
    "tampada, 'centro' mostra o preto do sensor. 'reg' exige o app Camera aberto."),
    cmd_camera,
    N_("camera reg 3908\tle o alvo do preto do sensor") },
  { "foto", "", G_BANCADA,
    N_("tira uma foto e grava no cartao"),
    N_("foto\tquadro inteiro em /sdcard/apps/Console/console.bmp"),
    N_("Funciona com o app Camera fechado ou aberto - prova que a camera e um\n"
    "servico, independente do app."),
    [](int, char**) { cmd_foto(); },
    N_("foto\tdepois: lista apps/Console") },

  // --- TESTES ---
  { "concorrencia", "", G_TESTES,
    N_("segundo leitor de GPS+IMU, fora de qualquer app"),
    N_("concorrencia\testa rodando? quantas leituras\n"
    "concorrencia iniciar\tliga o leitor (uma linha por segundo)\n"
    "concorrencia parar\tdesliga"),
    N_("Abra o IMU junto e compare os numeros: os dois leitores\n"
    "devem ver sempre o mesmo valor, nunca um meio atualizado."),
    cmd_concorrencia,
    N_("concorrencia iniciar\tdepois abra o IMU e compare") },
  { "simulacao", "", G_TESTES,
    N_("tres apps de mentira pedindo GPS e IMU juntos"),
    N_("simulacao\testado e leituras de cada um\n"
    "simulacao iniciar\tmapa (GPS/200ms), nivel (IMU), log (GPS+IMU/1s)\n"
    "simulacao parar\tpara os tres"),
    N_("Teste mais forte que 'concorrencia': tres cadencias diferentes, cada uma\n"
    "na sua tarefa, como apps reais abertos ao mesmo tempo."),
    cmd_simulacao,
    N_("simulacao iniciar\tdepois abra apps de verdade e compare") },

  // --- fora da lista: ajuda e atalhos antigos ---
  { "help",   "ajuda ?", G_OCULTO, "", "", "", c_ajuda },
  { "app",    "", G_OCULTO, "", "", "", c_app },
  { "redes",  "", G_OCULTO, "", "", "", c_redes },
  { "nos",    "", G_OCULTO, "", "", "", c_nos },
  { "enviar", "", G_OCULTO, "", "", "", c_enviar },
};
static const int N_COMANDOS = (int)(sizeof(COMANDOS) / sizeof(COMANDOS[0]));

static bool casa_nome(const comando_t& c, const char* s)
{
  if (eh(c.nome, s)) { return true; }
  // Apelidos: palavras separadas por espaco.
  const size_t n = strlen(s);
  for (const char* a = c.apelidos; *a; ) {
    while (*a == ' ') { ++a; }
    const char* fim = a;
    while (*fim && *fim != ' ') { ++fim; }
    if ((size_t)(fim - a) == n && n && strncasecmp(a, s, n) == 0) { return true; }
    a = fim;
  }
  return false;
}

// ---------------------------------------------------------------------------
// OS COMANDOS EM INGLES (06/11/2026) - o sistema vai para o mundo: cada comando
// e subcomando em portugues tem o nome em ingles ao lado. O QUE APARECE segue
// o idioma do sistema (pedido do usuario): em portugues, o help e as
// sugestoes mostram so os nomes em portugues; em ingles, so os em ingles. Ao
// DIGITAR os dois valem sempre ('estado' e 'status') - trocar o idioma nao
// quebra o costume nem um script.
//
// SUBCOMANDO SO SE TRADUZ ONDE ELE EXISTE: a palavra em ingles vira a em
// portugues apenas nas posicoes 1 a 4 e so se o 'uso' do comando tem aquela
// palavra ali (ver uso_tem). Assim 'mesh enviar to be or not' nao vira 'para
// be...', e um nome de arquivo ou de rede nunca e trocado.
// Um nome em ingles pode valer para dois em portugues ('sleep' = sono e
// dormir): o uso do comando decide qual.
// ---------------------------------------------------------------------------
typedef struct { const char* pt; const char* en; } par_t;

static const par_t CMD_EN[] = {
  { "estado", "status" },        { "tarefas", "tasks" },        { "energia", "power" },
  { "musica", "music" },         { "fundo", "background" },     { "travamento", "crash" },
  { "boasvindas", "welcome" },   { "arquivos", "files" },       { "tela", "display" },
  { "brilho", "brightness" },    { "piscada", "flicker" },      { "toque", "touch" },
  { "tocar", "tap" },            { "deslizar", "swipe" },       { "abrir", "open" },
  { "ordem", "order" },          { "cartao", "card" },          { "lista", "list" },
  { "ler", "read" },             { "despejar", "dump" },        { "apagar", "delete" },
  { "receber", "receive" },      { "farejo", "sniff" },         { "busca", "search" },
  { "foto", "photo" },           { "concorrencia", "concurrency" }, { "simulacao", "simulation" },
  { "redes", "networks" },       { "nos", "nodes" },            { "enviar", "send" },
  { "mem", "memory" },          { "fuso", "timezone" },
};

static const par_t SUB_EN[] = {
  { "ligar", "on" },             { "desligar", "off" },         { "iniciar", "start" },
  { "comecar", "start" },        { "parar", "stop" },           { "pausar", "pause" },
  { "retomar", "resume" },       { "acordar", "wake" },         { "dormir", "suspend" },
  { "sono", "sleep" },           { "repouso", "idle" },         { "reiniciar", "restart" },
  { "varrer", "scan" },          { "conectar", "connect" },     { "esquecer", "forget" },
  { "enviar", "send" },          { "para", "to" },              { "redes", "networks" },
  { "nos", "nodes" },            { "canal", "channel" },        { "saltos", "hops" },
  { "lista", "list" },           { "ler", "read" },             { "gravar", "save" },
  { "copiar", "copy" },          { "colar", "paste" },          { "limpar", "clear" },
  { "fechar", "close" },         { "cancelar", "cancel" },      { "ejetar", "eject" },
  { "devolver", "release" },     { "lixeira", "trash" },        { "pasta", "folder" },
  { "subpasta", "subfolder" },   { "caminho", "path" },         { "nome", "name" },
  { "ren", "rename" },           { "reler", "reload" },         { "verificar", "check" },
  { "rodar", "run" },            { "medir", "measure" },        { "zerar", "reset" },
  { "padrao", "default" },       { "tudo", "all" },             { "todos", "all" },             { "uma", "one" },
  { "sim", "yes" },              { "nao", "no" },               { "quem", "who" },
  { "travas", "locks" },         { "teto", "ceiling" },         { "tempos", "times" },
  { "perfil", "profile" },       { "prioridade", "priority" },  { "mapa", "map" },
  { "integridade", "integrity" },{ "estresse", "stress" },      { "espaco", "space" },
  { "tocar", "play" },           { "proxima", "next" },         { "anterior", "prev" },
  { "aleatorio", "shuffle" },    { "repetir", "repeat" },       { "trocar", "switch" },
  { "capa", "cover" },           { "recortar", "cut" },        { "janela", "window" },
  { "duracao", "duration" },     { "relogio", "clock" },        { "alta", "high" },
  { "cru", "raw" },              { "quadro", "frame" },         { "foto", "photo" },
  { "linhas", "lines" },         { "rolar", "scroll" },         { "marca", "mark" },
  { "aviso", "alert" },          { "casa", "home" },            { "brilho", "brightness" },
  { "extrato", "breakdown" },    { "ensaio", "trial" },         { "banco", "bench" },
  { "estado", "status" },        { "criar", "create" },         { "apagar", "delete" },
  { "por", "add" },              { "tirar", "remove" },
  { "restaurar", "restore" },    { "excluir", "remove" },       { "antigos", "older" },
  { "esvaziar", "empty" },       { "abrir", "open" },           { "ir", "seek" },
};

template <size_t N>
static const char* em_ingles(const par_t (&t)[N], const char* pt)
{
  for (const par_t& p : t) { if (eh(p.pt, pt)) { return p.en; } }
  return nullptr;
}

// O idioma do sistema nao e o portugues: o help mostra os nomes em ingles.
static bool help_em_ingles(void)
{
  const char* c = idioma_codigo(idioma_atual());
  return c && strcmp(c, "pt") != 0;
}
#define TXT(pt, en) (help_em_ingles() ? (en) : (pt))

static const comando_t* achar(const char* s)
{
  for (int i = 0; i < N_COMANDOS; ++i) { if (casa_nome(COMANDOS[i], s)) { return &COMANDOS[i]; } }
  for (const par_t& p : CMD_EN) {
    if (!eh(p.en, s)) { continue; }
    for (int i = 0; i < N_COMANDOS; ++i) { if (eh(COMANDOS[i].nome, p.pt)) { return &COMANDOS[i]; } }
  }
  return nullptr;
}

// A palavra 'pt' aparece na posicao 'pos' de alguma forma do uso de 'c', com
// as palavras antes dela batendo com argv (ou sendo um <argumento>)? As formas
// sao "cmd sub1 sub2 <arg>\t..."; alternativas como "[ligar|desligar]".
static bool palavra_casa(const char* tok, size_t n, const char* w)
{
  // tira [ ] e testa cada alternativa separada por '|'
  while (n && (*tok == '[')) { ++tok; --n; }
  while (n && (tok[n - 1] == ']')) { --n; }
  if (!n || *tok == '<') { return false; }
  const size_t wl = strlen(w);
  for (size_t i = 0; i <= n; ) {
    size_t j = i;
    while (j < n && tok[j] != '|') { ++j; }
    if (j - i == wl && strncasecmp(tok + i, w, wl) == 0) { return true; }
    i = j + 1;
  }
  return false;
}

static bool uso_tem(const comando_t& c, int pos, const char* pt, char** argv)
{
  for (const char* t = c.uso; t && *t; ) {
    const char* fim = strchr(t, '\n');
    const size_t n = fim ? (size_t)(fim - t) : strlen(t);
    const char* tab = (const char*)memchr(t, '\t', n);
    const char* lim = tab ? tab : t + n;
    // palavras da forma
    const char* w = t;
    bool ok = (*t != '\t');
    for (int k = 0; ok && k <= pos; ++k) {
      while (w < lim && *w == ' ') { ++w; }
      const char* e = w;
      while (e < lim && *e != ' ') { ++e; }
      if (e == w) { ok = false; break; }
      if (k == pos) { ok = palavra_casa(w, (size_t)(e - w), pt); }
      else if (k > 0 && *w != '<' && *w != '[') { ok = palavra_casa(w, (size_t)(e - w), argv[k]); }
      w = e;
    }
    if (ok) { return true; }
    t += n + (fim ? 1 : 0);
  }
  return false;
}

// Troca, nas posicoes 1 a 4, a palavra em ingles pela em portugues que o uso
// do comando conhece. 'buf' guarda as trocadas (argv aponta para la).
static void traduzir_subcomandos(const comando_t& c, int argc, char** argv, char (*buf)[24])
{
  for (int pos = 1; pos <= 4 && pos < argc; ++pos) {
    for (const par_t& p : SUB_EN) {
      if (!eh(p.en, argv[pos]) || eh(p.pt, argv[pos])) { continue; }
      if (!uso_tem(c, pos, p.pt, argv)) { continue; }
      snprintf(buf[pos - 1], 24, "%s", p.pt);
      argv[pos] = buf[pos - 1];
      break;
    }
  }
}

// Uma linha de uso com os nomes em ingles (o comando e os subcomandos nas
// posicoes 1 a 4; argumentos e o texto depois do TAB ficam como estao).
static void linha_em_ingles(const char* t, int n, char* out, size_t tam)
{
  const char* tab = (const char*)memchr(t, '\t', (size_t)n);
  const char* lim = tab ? tab : t + n;
  size_t o = 0;
  const char* w = t;
  for (int k = 0; w < lim && o + 1 < tam; ++k) {
    const char* e = w;
    while (e < lim && *e != ' ') { ++e; }
    const size_t wl = (size_t)(e - w);
    char palavra[32];
    snprintf(palavra, sizeof(palavra), "%.*s", (int)(wl < sizeof(palavra) ? wl : sizeof(palavra) - 1), w);
    if (k == 0) {
      const char* en = em_ingles(CMD_EN, palavra);
      o += (size_t)snprintf(out + o, tam - o, "%s", en ? en : palavra);
    } else if (k <= 4 && palavra[0] != '<') {
      // "[ligar|desligar]": cada alternativa, mantendo os colchetes e as barras
      for (const char* q = palavra; *q && o + 1 < tam; ) {
        if (*q == '[' || *q == ']' || *q == '|') { out[o++] = *q++; continue; }
        const char* f = q;
        while (*f && *f != '[' && *f != ']' && *f != '|') { ++f; }
        char alt[32];
        snprintf(alt, sizeof(alt), "%.*s", (int)(f - q), q);
        const char* en = em_ingles(SUB_EN, alt);
        o += (size_t)snprintf(out + o, tam - o, "%s", en ? en : alt);
        q = f;
      }
    } else {
      o += (size_t)snprintf(out + o, tam - o, "%s", palavra);
    }
    // os espacos que vierem
    while (e < lim && *e == ' ' && o + 1 < tam) { out[o++] = ' '; ++e; }
    w = e;
  }
  if (o >= tam) { o = tam - 1; }
  out[o] = 0;
  if (tab) { snprintf(out + o, tam - o, "%.*s", (int)(t + n - tab), tab); }
}

// Texto de varias linhas, cada uma com recuo.
static void imprimir_recuado(const char* t, const char* recuo)
{
  while (*t) {
    const char* fim = strchr(t, '\n');
    const int n = fim ? (int)(fim - t) : (int)strlen(t);
    printf("%s%.*s\n", recuo, n, t);
    t += n + (fim ? 1 : 0);
  }
}

// Linhas "esquerda\tdireita" em duas colunas. A esquerda que nao cabe na
// coluna fica sozinha, e a explicacao desce para a linha de baixo, alinhada -
// cortar um caminho de arquivo no meio estragaria o exemplo para copiar.
#define AJUDA_COLUNA 32
static void imprimir_colunas(const char* texto, const char* prefixo, bool traduzir)
{
  const int larg = AJUDA_COLUNA - (int)strlen(prefixo);
  const bool ingles = traduzir && help_em_ingles();
  for (const char* t0 = texto; t0 && *t0; ) {
    const char* fim = strchr(t0, '\n');
    const int n0 = fim ? (int)(fim - t0) : (int)strlen(t0);
    char trad[256];
    const char* t = t0;
    int n = n0;
    if (ingles && *t0 != '\t') { linha_em_ingles(t0, n0, trad, sizeof(trad)); t = trad; n = (int)strlen(trad); }
    const char* tab = (const char*)memchr(t, '\t', (size_t)n);
    if (!tab) {
      printf("  %s%.*s\n", prefixo, n, t);
    } else {
      const int a = (int)(tab - t);
      if (a <= larg) { printf("  %s%-*.*s %.*s\n", prefixo, larg, a, t, n - a - 1, tab + 1); }
      else           { printf("  %s%.*s\n  %*s %.*s\n", prefixo, a, t, AJUDA_COLUNA, "", n - a - 1, tab + 1); }
    }
    t0 += n0 + (fim ? 1 : 0);
  }
}

// As formas de chamar e, logo abaixo, exemplos prontos para copiar.
static void imprimir_uso(const comando_t& c)
{
  imprimir_colunas(tr(c.uso), "", true);
  imprimir_colunas(tr(c.exemplos), "ex: ", true);
}

void mostrar_uso(const char* nome)
{
  const comando_t* c = achar(nome);
  if (!c) { return; }
  printf("%s", TXT("uso:\n", "usage:\n"));
  imprimir_uso(*c);
}

static void ajuda_comando(const comando_t& c)
{
  // O NOME NO IDIOMA DO SISTEMA, so ele: em portugues os nomes em portugues,
  // em ingles os em ingles (o outro continua aceito ao digitar, mas nao aparece).
  const char* en = help_em_ingles() ? em_ingles(CMD_EN, c.nome) : nullptr;
  printf("%s - %s\n\n", en ? en : c.nome, tr(c.resumo));
  imprimir_uso(c);
  printf("\n");
  imprimir_recuado(tr(c.detalhe), "  ");
  if (c.apelidos[0]) { printf(TXT("  (tambem aceito como: %s)\n", "  (also accepted as: %s)\n"), c.apelidos); }
}

// Comando desconhecido: sugere os que comecam igual ou contem o que foi
// digitado. Errar uma letra nao pode virar "leia a lista inteira".
static void sugerir(const char* s)
{
  const size_t n = strlen(s);
  int achados = 0;
  for (int i = 0; i < N_COMANDOS; ++i) {
    const comando_t& c = COMANDOS[i];
    if (c.grupo == G_OCULTO) { continue; }
    const char* en = help_em_ingles() ? em_ingles(CMD_EN, c.nome) : nullptr;
    const char* nome = en ? en : c.nome;   // o nome no idioma do sistema
    const bool parecido = (n >= 2 && strncasecmp(nome, s, 2) == 0) ||
                          (n >= 3 && strcasestr(nome, s) != nullptr);
    if (parecido) { printf(achados++ ? ", %s" : TXT("  talvez: %s", "  maybe: %s"), nome); }
  }
  if (achados) { printf("\n"); }
  printf("%s", TXT("  'help' lista os comandos\n", "  'help' lists the commands\n"));
}

// O 'help' SEM NADA: os comandos que quem vem do Linux procura, com o nome do
// Linux (ps, free, cat, ifconfig, curl...). Os do M5tabOS ficam em
// 'help sistema' - antes vinham todos de uma vez, ~200 linhas, e o 'ls'
// ficava perdido entre 'piscada' e 'radio 154'.
typedef struct { const char* forma_pt; const char* forma_en; const char* cmd; const char* pt; const char* en; } linux_t;
static const linux_t LINUX[] = {
  { "ls [pasta]",          "ls [dir]",            "ls",         "lista a pasta",                        "list a folder" },
  { "cd <pasta>",          "cd <dir>",            "cd",         "muda de pasta",                        "change folder" },
  { "pwd",                 "pwd",                 "pwd",        "mostra a pasta atual",                 "print the current folder" },
  { "cat <arquivo>",       "cat <file>",          "ler",        "mostra um arquivo de texto",           "print a text file" },
  { "cp <origem> <pasta>", "cp <src> <dir>",      "cp",         "copia",                                "copy" },
  { "mv <origem> <pasta>", "mv <src> <dir>",      "mv",         "move",                                 "move" },
  { "rm <caminho>",        "rm <path>",           "rm",         "manda para a lixeira",                 "move to the trash" },
  { "mkdir <pasta>",       "mkdir <dir>",         "mkdir",      "cria uma pasta",                       "make a folder" },
  { "df",                  "df",                  "df",         "espaco dos discos",                    "disk space" },
  { "echo <texto>",        "echo <text>",         "echo",       "repete o texto",                       "print the text" },
  { "clear",               "clear",               "clear",      "limpa a tela",                         "clear the screen" },
  { "ps / top",            "ps / top",            "tarefas",    "tarefas rodando, CPU e pilha",         "running tasks, CPU and stack" },
  { "free",                "free",                "mem",        "memoria livre",                        "free memory" },
  { "ping <host>",         "ping <host>",         "ping",       "responde?",                            "is it reachable?" },
  { "ifconfig",            "ifconfig",            "ip",         "endereco, mascara, gateway",           "address, netmask, gateway" },
  { "nslookup <nome>",     "nslookup <name>",     "dns",        "resolve um nome",                      "resolve a name" },
  { "arp",                 "arp",                 "arp",        "aparelhos da rede local",              "devices on the local network" },
  { "traceroute <host>",   "traceroute <host>",   "traceroute", "o caminho ate o host",                 "the route to a host" },
  { "nc <host> <porta>",   "nc <host> <port>",    "tcp",        "a porta esta aberta?",                 "is the port open?" },
  { "curl <url>",          "curl <url>",          "http",       "busca uma pagina (HTTP e HTTPS)",      "fetch a page (HTTP and HTTPS)" },
  { "wget <url>",          "wget <url>",          "http",       "baixa para a pasta atual",             "download to the current folder" },
  { "ssh <usuario>@<host>","ssh <user>@<host>",   "ssh",        "terminal remoto",                      "remote shell" },
};

static void ajuda_linux(void)
{
  const bool en = help_em_ingles();
  printf(tr("M5tabOS %s - %s\n\n"), esp_app_get_description()->version, en ? tr("terminal") : tr("terminal"));
  printf("%s\n", en ? tr("THE USUAL COMMANDS (as in Linux)") : tr("OS COMANDOS DE SEMPRE (como no Linux)"));
  for (const linux_t& l : LINUX) {
    if (!achar(l.cmd)) { continue; }
    printf("  %-*s %s\n", AJUDA_COLUNA, en ? l.forma_en : l.forma_pt, en ? l.en : l.pt);
  }
  printf("\n%s\n", en ? tr("MORE") : tr("MAIS"));
  printf("  %-*s %s\n", AJUDA_COLUNA, en ? tr("help system") : tr("help sistema"),
         en ? tr("M5tabOS commands: display, card, network, Mesh, power...") : tr("os comandos do M5tabOS: tela, cartao, rede, Mesh, energia..."));
  printf("  %-*s %s\n", AJUDA_COLUNA, "help app", en ? tr("app commands") : tr("os comandos dos apps"));
  printf("  %-*s %s\n", AJUDA_COLUNA, en ? tr("help <command>") : tr("help <comando>"), en ? tr("what it is for, with examples") : tr("para que serve, com exemplos"));
  printf("  %-*s %s\n", AJUDA_COLUNA, en ? tr("help all") : tr("help tudo"), en ? tr("everything, explained") : tr("tudo, explicado"));
  printf("\n%s\n", en ? tr("Case does not matter.") : tr("Maiusculas tanto faz."));
}

static void c_ajuda(int argc, char** argv)
{
  if (argc < 2) { ajuda_linux(); return; }
  if (argc >= 2 && eh(argv[1], "app")) {
    const app_t* a = nullptr;
    for (int i = 0; argc >= 3 && i < SISTEMA_NUM_APPS; ++i) {
      if (nome_de_app(SISTEMA_APPS[i]->nome, argv[2])) { a = SISTEMA_APPS[i]; }
    }
    ajuda_apps(a);
    return;
  }
  const bool tudo = eh(argv[1], "tudo") || eh(argv[1], "all");
  const bool sistema = eh(argv[1], "sistema") || eh(argv[1], "system");
  if (!tudo && !sistema) {
    const comando_t* c = achar(argv[1]);
    if (!c || c->grupo == G_OCULTO) {
      printf(TXT("nao ha comando '%s'\n", "no command '%s'\n"), argv[1]);
      sugerir(argv[1]);
      return;
    }
    ajuda_comando(*c);
    return;
  }
  // Agrupado por ASSUNTO, com TODAS as formas de cada comando a vista. Uma
  // linha so por comando escondia justamente o que se procura ("como mando
  // mensagem?" ficava atras de 'help mesh').
  // 'help sistema': os do M5tabOS (o SHELL ja esta no 'help'); 'help tudo':
  // todos, cada um explicado.
  printf(tr("M5tabOS %s - %s\n"), esp_app_get_description()->version,
         TXT("comandos do sistema", "system commands"));
  for (int g = 0; g < G_N; ++g) {
    if (sistema && g == G_SHELL) { continue; }
    printf("\n%s\n", (help_em_ingles() ? GRUPO_TITULO_EN : GRUPO_TITULO)[g]);
    if (g == G_APPS) { ajuda_apps(nullptr); continue; }
    bool primeiro = true;
    for (int i = 0; i < N_COMANDOS; ++i) {
      const comando_t& c = COMANDOS[i];
      if (c.grupo != g) { continue; }
      if (!primeiro) { printf("\n"); }   // um respiro entre comandos do grupo
      primeiro = false;
      if (tudo) { ajuda_comando(c); }
      else      { imprimir_uso(c); }
      ceder(i);
    }
  }
  printf("%s", TXT("\n'help <comando>' explica para que serve cada um; 'help tudo' explica todos.\n"
                   "Caminhos do cartao podem ser relativos a /sdcard. Maiusculas tanto faz.\n",
                   "\n'help <command>' explains each one; 'help all' explains them all.\n"
                   "Card paths may be relative to /sdcard. Case does not matter.\n"));
}

// ---------------------------------------------------------------------------
// Leitor de linha proprio, SEM linenoise
// ---------------------------------------------------------------------------
//
// A primeira versao usava esp_console_new_repl_uart(), e nao dava para digitar:
// nada aparecia na tela. O motivo esta no proprio linenoise, em
// components/console/linenoise/linenoise.c:1191 - no modo "terminal burro" ele
// so imprime o prompt e NAO ecoa o que voce digita; o comentario la e explicito
// ("Only erase symbol echoed from stdin"), ou seja, ele conta com o eco local
// do terminal. O idf.py monitor nao faz eco local, encaminha as teclas cruas.
// Resultado: voce digita as cegas.
//
// Alem disso, cair ou nao no modo burro depende de uma sondagem do terminal que
// pode dar resultados diferentes em cada monitor. Preferi eliminar a variavel:
// este loop le byte a byte e ECOA ELE MESMO, entao funciona igual em qualquer
// terminal - idf.py monitor, screen, minicom, o que for.

static void executar(char* linha)
{
  // Quebra em palavras, respeitando aspas para senha com espaco.
  //
  // 24 E NAO 8: o "mesh enviar para <no> <texto>" ja gasta quatro posicoes, e
  // com 8 o texto era truncado em silencio na quinta palavra. 24 cobre a linha
  // inteira: com 192 caracteres de entrada e o limite de 160 do MESH_TEXTO, o
  // texto acaba antes das palavras. Custa 64 bytes de stack.
  char* argv[24];
  int argc = 0;
  char* p = linha;
  while (*p && argc < 24)
  {
    while (*p == ' ') { ++p; }
    if (*p == 0) { break; }
    if (*p == '"')
    {
      argv[argc++] = ++p;
      while (*p && *p != '"') { ++p; }
    }
    else
    {
      argv[argc++] = p;
      while (*p && *p != ' ') { ++p; }
    }
    if (*p) { *p++ = 0; }
  }
  if (argc == 0) { return; }

  const comando_t* c = achar(argv[0]);
  if (!c) {
    printf(TXT("comando desconhecido: '%s'\n", "unknown command: '%s'\n"), argv[0]);
    sugerir(argv[0]);
    return;
  }
  char trad[4][24];   // subcomandos em ingles trocados pelos em portugues (posicoes 1 a 4)
  traduzir_subcomandos(*c, argc, argv, trad);
  c->fn(argc, argv);
}

void console_executar(console_sessao_t* sessao, const char* linha)
{
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", linha ? linha : "");
  // A sessao desta tarefa enquanto o comando roda. Sem vaga (nao acontece:
  // sao duas sessoes), roda com a da serial.
  const TaskHandle_t eu = xTaskGetCurrentTaskHandle();
  int k = -1;
  if (sessao && sessao != &s_serial) {
    taskENTER_CRITICAL(&s_ativas_mux);
    for (int i = 0; i < SESSOES_MAX && k < 0; ++i) {
      if (!s_ativas[i].t) { s_ativas[i].t = eu; s_ativas[i].s = sessao; k = i; }
    }
    taskEXIT_CRITICAL(&s_ativas_mux);
  }
  executar(buf);
  fflush(stdout);
  if (k >= 0) {
    taskENTER_CRITICAL(&s_ativas_mux);
    s_ativas[k].t = nullptr;
    s_ativas[k].s = nullptr;
    taskEXIT_CRITICAL(&s_ativas_mux);
  }
}


static void vTaskConsole(void*)
{
  char linha[192];
  size_t n = 0;

  printf("%s", tr("\nconsole pronto - digite help\nm5tab> "));
  fflush(stdout);

  for (;;)
  {
    // BLOQUEIA DE VERDADE, e isso e economia de energia.
    //
    // Era assim:
    //
    //     int c = fgetc(stdin);
    //     if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    //
    // O fgetc sobre o VFS devolve EOF quando nao ha byte, entao a tarefa
    // acordava 50 VEZES POR SEGUNDO, para sempre, mesmo em repouso e mesmo com
    // ninguem conectado na serial. Cada volta e uma troca de contexto e uma
    // consulta ao driver para descobrir que nao ha nada.
    //
    // O usb_serial_jtag_read_bytes com portMAX_DELAY dorme no semaphore do
    // driver: zero acordadas enquanto nao chega tecla. E o mesmo caminho que o
    // comando `receber` usa, e ele passa ao largo da traducao de fim de linha
    // do VFS - que aqui nao faz falta, porque o loop abaixo ja trata CR e LF.
    uint8_t byte = 0;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) { continue; }
#else
    if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) { continue; }
#endif
    const int c = (int)byte;

    // Diagnostico enquanto o console nao estiver confiavel: mostra o codigo de
    // TODO byte que chega. Serve para distinguir dois problemas muito
    // diferentes - "o monitor nao encaminha o teclado" (nada aparece aqui) de
    // "chega, mas o filtro descarta" (aparece o codigo e nada mais).
    ESP_LOGD(TAG, "rx: 0x%02x", (unsigned)c);

    if (c == '\r' || c == '\n')
    {
      printf("\n");
      linha[n] = 0;
      if (n > 0) { console_executar(&s_serial, linha); }
      n = 0;
      printf("%s", tr("m5tab> "));
      fflush(stdout);
    }
    else if (c == 8 || c == 127)          // backspace
    {
      if (n > 0) { --n; printf("\b \b"); fflush(stdout); }
    }
    else if (c >= 32 && c < 127 && n < sizeof(linha) - 1)
    {
      linha[n++] = (char)c;
      fputc(c, stdout);                   // O ECO QUE FALTAVA
      fflush(stdout);
    }
  }
}

void console_iniciar(void)
{
  // O driver TEM que ser o do mesmo periferico que o console primario, senao o
  // stdin fica preso num canal onde ninguem digita - foi exatamente o erro que
  // deixou o console mudo (ver o comentario no sdkconfig.defaults).
  // Por isso a escolha e feita pela propria chave do console, e nao fixada.
  //
  // Em ambos os casos: sem driver instalado a leitura de stdin NAO bloqueia (o
  // fgetc devolveria EOF para sempre e o loop viraria espera ocupada), e e o
  // driver que permite ler tecla a tecla em vez de linha inteira.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  // ANEL DE RX GRANDE, e isto NAO e folga preventiva - e correcao.
  //
  // O padrao do IDF e 256 bytes (USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT), e a
  // ISR do driver faz assim (usb_serial_jtag.c:150):
  //
  //     xRingbufferSendFromISR(rx_ring_buf, buf, rx_fifo_len, &xTaskWoken);
  //
  // O RETORNO E IGNORADO. Anel cheio = bytes descartados em silencio, sem erro,
  // sem log, sem nada. Para digitar comando ninguem percebe; mandando 1,4MB de
  // firmware pelo `receber`, sumiram 2432 bytes no meio e a transferencia
  // morreu a 99,8%.
  //
  // 8KB dao ~30ms de folga a velocidade de USB - tempo de sobra para o loop de
  // leitura voltar mesmo que o escalonador atrase.
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 8192;
  cfg.tx_buffer_size = 2048;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK)
  {
    ESP_LOGE(TAG, "nao consegui instalar o driver do USB-Serial-JTAG");
    return;
  }
  usb_serial_jtag_vfs_use_driver();
  // Enter chega como CR; sem a conversao a linha nunca terminaria. A saida vai
  // como CRLF para nao sair em escadinha.
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#else
  if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK)
  {
    ESP_LOGE(TAG, "nao consegui instalar o driver da UART0");
    return;
  }
  uart_vfs_dev_use_driver(UART_NUM_0);
  uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
  uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
#endif

  // stdin sem buffer. O newlib bufferiza por linha por padrao, e ai o fgetc
  // pode ficar esperando encher o buffer em vez de devolver a tecla na hora -
  // o eco so apareceria depois do Enter, ou nao apareceria. Todo exemplo de
  // console do IDF faz isto.
  setvbuf(stdin, NULL, _IONBF, 0);

  // 12KB, e nao 4096. O 4096 ESTOUROU - "enviar" derrubou o aparelho com
  // Stack protection fault, e o retrato de tarefas ja avisava: 372 B livres.
  //
  // Esta tarefa tem o pior caso do sistema inteiro, porque de um comando ela
  // alcanca QUALQUER subsistema. So no caminho do "enviar":
  //
  //   executar()          ~1,3KB de buffers de comando (sub, cam, completo...)
  //   tab5_mesh_enviar()   656B - data[176]+pkt[224]+frame[256], todos vivos
  //   guardar_msg()        -> FATFS, que e faminto de stack
  //   printf com float     -> vfprintf do newlib, mais de 1KB sozinho
  //
  // O "cartao" e o "c6 ota" descem igualmente fundo. Dimensionar pelo caso
  // tipico aqui e dimensionar para o dia em que nao se usa o comando dificil.
  //
  // MEDIDO: dois dias de pilhas-*.csv com uso normal do console, pico de
  // ~4,0KB (minimo livre 8320 de 12288). 8KB e o dobro do pico - folga para
  // os comandos fundos que nao aparecem todo dia - e devolve 4KB a RAM
  // interna. Se um comando novo descer mais, o pilhas-*.csv mostra.
  xTaskCreatePinnedToCore(vTaskConsole, "console", 8192, nullptr, 1, nullptr, tskNO_AFFINITY);
  ESP_LOGI(TAG, "console no ar - digite 'help' no monitor");
}
