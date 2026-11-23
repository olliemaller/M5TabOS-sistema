// Console commands - CARTAO (SD CARD): cartao, lista, ler, despejar, apagar, receber, arquivos.
// (Split from console.cpp; the COMANDOS table and the help remain there.)

#include "mbedtls/base64.h"
#include "tab5_ui.hpp"   // tab5_bytes_txt
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  #include "driver/usb_serial_jtag.h"
  #include "driver/usb_serial_jtag_vfs.h"
#else
  #include "driver/uart.h"
  #include "driver/uart_vfs.h"
#endif
#include "tab5_arquivos.hpp"
#include "mbedtls/md5.h"
#include "ff.h"
#include "console/console_interno.hpp"

// Human-readable size, in its own static buffer. Static and not the caller's
// because it appears twice in the SAME printf line in some places - and
// two simultaneous uses of the same buffer would trample each other. That is why it alternates
// between two.
static const char* tamanho_curto(uint64_t bytes)
{
  static char buf[2][16];
  static int  alt = 0;
  char* b = buf[alt = (alt + 1) & 1];
  tab5_bytes_txt(b, 16, bytes);
  return b;
}

// Overview of the SD card: space and one line per app folder.
//
// `lista` shows ONE folder and is for looking for a file. This one answers another
// question, the one asked first: "what is here and how much is used?".
void cmd_cartao(void)
{
  {
    // The system writer (hal/tab5_sd.hpp): what is still waiting, what has already gone.
    uint32_t pend = 0, grav = 0, perd = 0;
    tab5_sd_anexos_estado(&pend, &grav, &perd);
    printf(tr("gravador: %lu B esperando, %lu linha(s) gravadas, %lu perdida(s) desde o boot\n"),
           (unsigned long)pend, (unsigned long)grav, (unsigned long)perd);
  }
  if (!tab5_sd_montar()) {
    printf(tr("cartao nao montou (%s)\n"), tab5_sd_descricao());
    return;
  }

  printf(tr("--- cartao ---\n  %s\n"), tab5_sd_descricao());

  // Free space via FATFS. The VFS statvfs is not implemented for FAT in the
  // IDF, so we go to the driver: free clusters x sectors per cluster.
  FATFS* fs = nullptr;
  DWORD livres = 0;
  if (f_getfree("0:", &livres, &fs) == FR_OK && fs) {
    // REAL SECTOR, not FF_MAX_SS. FF_MAX_SS is the MAXIMUM sector FATFS was
    // compiled to accept (4096 in the IDF); the card's is 512. Multiplying by the
    // maximum gave 8x too much - a 14.5GB card showed up as 118GB.
    //
    // When FF_MAX_SS == FF_MIN_SS the ssize field does not even exist (FATFS removes it,
    // because there is only one possible size), hence the #if.
#if FF_MAX_SS != FF_MIN_SS
    const uint64_t setor = fs->ssize;
#else
    const uint64_t setor = FF_MAX_SS;
#endif
    const uint64_t tot = (uint64_t)(fs->n_fatent - 2) * fs->csize * setor;
    const uint64_t liv = (uint64_t)livres * fs->csize * setor;
    printf(tr("  %llu MB livres de %llu MB (%llu%% em uso)\n"),
           liv / (1024 * 1024), tot / (1024 * 1024),
           tot ? ((tot - liv) * 100 / tot) : 0);
  }

  DIR* d = opendir(TAB5_SD_APPS);
  if (!d) {
    printf(tr("  (nao ha %s ainda)\n"), TAB5_SD_APPS);
    tab5_sd_desmontar();
    return;
  }

  printf("\n  %-16s %6s  %s\n", tr("pasta"), tr("arqs"), tr("tamanho"));
  struct dirent* e;
  int k = 0;
  while ((e = readdir(d)) != nullptr) {
    ceder(k++);
    if (e->d_name[0] == '.') { continue; }

    char sub[160];
    snprintf(sub, sizeof(sub), "%s/%s", TAB5_SD_APPS, e->d_name);

    // Counts and sums without recursion: app folders are flat by convention
    // (see tab5_sd.hpp), and walking a tree here would only open a way to
    // overflow the stack on a card someone filled with subfolders.
    DIR* sd = opendir(sub);
    if (!sd) { continue; }

    // Stores the names to print AFTER the folder header - the total has
    // to appear on top, and the total can only be known after walking it.
    #define CARTAO_MOSTRA 10
    char nomes[CARTAO_MOSTRA][40];
    uint64_t tams[CARTAO_MOSTRA];
    int n = 0; uint64_t bytes = 0;

    struct dirent* f2;
    while ((f2 = readdir(sd)) != nullptr) {
      if (f2->d_name[0] == '.') { continue; }
      char cam[320];
      snprintf(cam, sizeof(cam), "%s/%s", sub, f2->d_name);
      struct stat st;
      if (stat(cam, &st) != 0 || !S_ISREG(st.st_mode)) { continue; }
      if (n < CARTAO_MOSTRA) {
        snprintf(nomes[n], sizeof(nomes[0]), "%s", f2->d_name);
        tams[n] = (uint64_t)st.st_size;
      }
      ++n;
      bytes += (uint64_t)st.st_size;
    }
    closedir(sd);

    printf("  %-16s %6d  %s\n", e->d_name, n, tamanho_curto(bytes));

    // The files, indented. This was exactly what was missing: you could see that the
    // Sistema folder had 3 files and 1.5MB, and you could NOT see whether the C6
    // firmware was one of them.
    const int mostrar = (n < CARTAO_MOSTRA) ? n : CARTAO_MOSTRA;
    for (int i = 0; i < mostrar; ++i) {
      printf("      %-28.28s %10s\n", nomes[i], tamanho_curto(tams[i]));
    }
    if (n > CARTAO_MOSTRA) {
      printf(tr("      ... e mais %d\n"), n - CARTAO_MOSTRA);
    }
  }
  closedir(d);
  tab5_sd_desmontar();
  printf(tr("\n  'lista %s/<pasta>' mostra os arquivos de uma delas\n"), TAB5_SD_APPS);
}

// ---------------------------------------------------------------------------
// RECEIVING A FILE OVER THE SERIAL PORT
// ---------------------------------------------------------------------------
//   receber <app> <file> <bytes>
//
// It was born of a concrete need: sending 1.4MB of C6 firmware to the
// SD card without taking the card out of the device.
//
// THREE DECISIONS THAT ARE NOT OBVIOUS:
//
// 1. READS STRAIGHT FROM THE DRIVER, not from stdin. The console runs with
//    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR), which translates
//    line endings on input - harmless for text, DESTRUCTIVE for binary:
//    every 0x0D would become something else. usb_serial_jtag_read_bytes() bypasses
//    the VFS and delivers the byte as it came.
//
// 2. KEEPS EVERYTHING IN PSRAM BEFORE WRITING TO THE CARD. The serial port has no
//    flow control: if we stop to write to the SD - and FATFS does stop,
//    sometimes for hundreds of milliseconds, when it closes a cluster - the receive
//    buffer fills and the bytes are silently lost. With 32MB of PSRAM,
//    receiving it all and writing afterwards decouples the two completely.
//
// 3. PRINTS NOTHING DURING RECEPTION. The natural way to send the file is
//    "cat file > /dev/cu.usbmodem*", and in that set-up NOBODY is reading the
//    output. If we write, the TX buffer fills and the write blocks -
//    stopping precisely the loop that needs to consume the input. Silence until the
//    end is what keeps the stream alive.
#define RECEBER_MAX (6u * 1024u * 1024u)


void cmd_receber(int argc, char** argv)
{
  if (argc < 4) {
    printf("%s", tr("uso: receber <app> <arquivo> <bytes>\n"
           "  ex: receber Sistema tab5_c6.bin 1397904\n"
           "\n"
           "Depois de dar o comando, mande o arquivo CRU pela mesma porta.\n"
           "Do Mac, com o monitor FECHADO:\n"
           "  cat arquivo.bin > /dev/cu.usbmodem*\n"
           "\n"
           "Ou tudo de uma vez, sem precisar do monitor:\n"
           "  { printf 'receber Sistema tab5_c6.bin 1397904\\r'; sleep 2; \\\n"
           "    cat arquivo.bin; } > /dev/cu.usbmodem*\n"));
    return;
  }

  const long tam = atol(argv[3]);
  if (tam <= 0 || (unsigned long)tam > RECEBER_MAX) {
    printf(tr("tamanho invalido (1 a %u bytes)\n"), (unsigned)RECEBER_MAX);
    return;
  }

  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)tam, MALLOC_CAP_SPIRAM);
  if (!buf) { printf(tr("sem PSRAM para %ld bytes\n"), tam); return; }

  printf(tr("mande %ld bytes agora (10 s de paciencia entre blocos)\n"), tam);
  fflush(stdout);

  size_t lidos = 0;
  int vazios = 0;
  while (lidos < (size_t)tam) {
    const int n = usb_serial_jtag_read_bytes(buf + lidos, (size_t)tam - lidos,
                                             pdMS_TO_TICKS(1000));
    if (n > 0) { lidos += (size_t)n; vazios = 0; }
    else if (++vazios >= 10) { break; }   // gave up waiting
  }

  // WRITES WHAT ARRIVED, even if incomplete. Throwing away 1.4MB because of the
  // last 2KB helps nobody: with the file on the card and the md5 on screen,
  // you can see WHETHER something is missing and HOW MUCH. You decide whether it is usable - and
  // `c6 gravar` checks the header before using it, in any case.
  const bool completo = (lidos == (size_t)tam);
  if (lidos == 0) {
    printf("%s", tr("\nnao chegou byte nenhum.\n"));
    heap_caps_free(buf);
    return;
  }
  if (!completo) {
    printf(tr("\nINCOMPLETO: %u de %ld bytes (faltaram %ld). Gravando assim mesmo\n"
           "para voce conferir - compare o md5 abaixo.\n"),
           (unsigned)lidos, tam, tam - (long)lidos);
  }

  // MD5 to check against the Mac's `md5 -q`. An equal size proves
  // nothing: a byte swapped in the middle of the serial transfer gives the same size.
  unsigned char md5[16];
  mbedtls_md5(buf, lidos, md5);

  bool ok = false;
  if (tab5_sd_montar()) {
    FILE* f = tab5_sd_abrir(argv[1], argv[2], "wb");
    if (f) {
      ok = (fwrite(buf, 1, lidos, f) == lidos);
      fclose(f);
    }
    tab5_sd_desmontar();
  }
  heap_caps_free(buf);

  if (!ok) { printf("%s", tr("\nrecebi tudo, mas a gravacao no cartao falhou\n")); return; }

  printf(tr("\ngravado: /sdcard/apps/%s/%s  (%u bytes)%s\nmd5: "),
         argv[1], argv[2], (unsigned)lidos, completo ? "" : tr("  <- INCOMPLETO"));
  for (int i = 0; i < 16; ++i) { printf("%02x", md5[i]); }
  printf("\n");
}

// ---------------------------------------------------------------------------
// ler <path> [lines]
// ---------------------------------------------------------------------------
int cmd_ler(int argc, char** argv)
{
  if (argc < 2) { printf("%s", tr("uso: ler <caminho> [linhas]\n")); return 1; }
  char buf[160];
  const char* caminho = caminho_sd(argv[1], buf, sizeof(buf));
  // Line ceiling: a satelites-*.csv for a whole day has thousands of them,
  // and dumping everything at 115200 baud would hold the console for minutes. Whoever wants
  // more asks for the number; whoever wants the whole file uses 'despejar'.
  int teto = (argc >= 3) ? atoi(argv[2]) : 200;
  if (teto < 1) { teto = 1; }

  return com_cartao([&]() -> int {
    FILE* f = fopen(caminho, "r");
    if (!f) { printf(tr("erro: %s nao abriu\n"), caminho); return 1; }

    printf("--- %s ---\n", caminho);
    char linha[160];
    int n = 0;
    while (n < teto && fgets(linha, sizeof(linha), f)) {
      fputs(linha, stdout);
      if ((++n % 64) == 0) { fflush(stdout); vTaskDelay(1); }
    }
    if (n >= teto && !feof(f)) {
      printf(tr("... (cortado em %d linhas - 'ler %s <N>' mostra mais, 'despejar' mostra tudo)\n"),
             teto, argv[1]);
    }
    printf("%s", tr("--- fim ---\n"));
    fclose(f);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// despejar <path>  - the WHOLE file, to be captured on the other side
// ---------------------------------------------------------------------------
// Sibling of "ler", with three differences that are the reason it exists separately:
//
//   1. NO LINE CEILING. "ler" cuts at 200 because it is for LOOKING - and
//      looking at more than that on the terminal screen is no use. This one is for
//      COPYING: an energia.csv of a whole discharge has about 500 lines, and a
//      file cut in the middle is no use for fitting any curve.
//
//   2. SILENCES THE LOG WHILE DUMPING. This console shares the serial port with the
//      ESP_LOG of every task. Without silencing, a "power" or
//      "mesh" line lands IN THE MIDDLE of the CSV, and whoever is capturing on the other side gets
//      a contaminated file. Restored at the end, always - including when
//      fopen fails.
//
//   3. MARKERS THE SCRIPT RECOGNISES. ">>>INICIO" and ">>>FIM" with the
//      line count tell the capturer where it starts, where it ends and whether
//      everything arrived. Without them, the script on the other side would have to guess
//      when to stop listening - and guessing always gets it wrong at the wrong moment.
int cmd_despejar(int argc, char** argv)
{
  if (argc < 2) {
    printf("%s", tr("uso: despejar <caminho>\n"
           "  ex.: despejar /sdcard/apps/Sistema/energia/energia-AAAA-MM-DD.csv\n"
           "  no Mac, para salvar:  ./ferramentas/baixar.py <caminho>\n"));
    return 1;
  }
  // -b: BINARY (photo, firmware), in base64 - fgets/fputs break on the
  // first null byte. baixar.py decodes it.
  const bool binario = strcmp(argv[1], "-b") == 0;
  if (binario && argc < 3) { printf("%s", tr("uso: despejar -b <caminho>\n")); return 1; }
  char buf[160];
  const char* caminho = caminho_sd(argv[binario ? 2 : 1], buf, sizeof(buf));

  if (binario) {
    return com_cartao([&]() -> int {
      FILE* f = fopen(caminho, "rb");
      if (!f) { printf(tr("erro: %s nao abriu\n"), caminho); return 1; }
      long tam = 0;
      tam = tab5_sd_tamanho(f); if (tam < 0) { tam = 0; }
      esp_log_level_set("*", ESP_LOG_NONE);
      printf(tr(">>>INICIO %s %ld base64\n"), caminho, tam);
      unsigned char cru[57 * 16];
      unsigned char b64[80];
      long n = 0;
      size_t lidos;
      while ((lidos = fread(cru, 1, sizeof(cru), f)) > 0) {
        for (size_t o = 0; o < lidos; o += 57) {
          size_t olen = 0;
          const size_t pedaco = lidos - o < 57 ? lidos - o : 57;
          mbedtls_base64_encode(b64, sizeof(b64), &olen, cru + o, pedaco);
          b64[olen] = 0;
          puts((const char*)b64);
          // More often than for text: with the USB serial buffer full, whole lines
          // were discarded (measured: 6% of a photo was lost).
          if ((++n % 2) == 0) { fflush(stdout); vTaskDelay(1); }
        }
      }
      fflush(stdout);
      printf(tr(">>>FIM %ld linhas\n"), n);
      esp_log_level_set("*", ESP_LOG_INFO);
      fclose(f);
      return 0;
    });
  }

  return com_cartao([&]() -> int {
    FILE* f = fopen(caminho, "r");
    if (!f) { printf(tr("erro: %s nao abriu\n"), caminho); return 1; }

    // Size FIRST, so the other side can check what it received.
    long tam = 0;
    tam = tab5_sd_tamanho(f); if (tam < 0) { tam = 0; }

    // From here on, silence - see reason 2 in the header.
    esp_log_level_set("*", ESP_LOG_NONE);

    printf(tr(">>>INICIO %s %ld\n"), caminho, tam);
    char linha[256];
    long n = 0;
    while (fgets(linha, sizeof(linha), f)) {
      fputs(linha, stdout);
      // A pause every 64 lines. It is not politeness: dumping a large
      // file is a tight loop on this task, and without yielding the CPU the idle
      // task watchdog complains and the UI stutters.
      if ((++n % 64) == 0) { fflush(stdout); vTaskDelay(1); }
    }
    fflush(stdout);
    printf(tr(">>>FIM %ld linhas\n"), n);

    esp_log_level_set("*", ESP_LOG_INFO);
    fclose(f);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// lista [path]
// ---------------------------------------------------------------------------
int cmd_lista(int argc, char** argv)
{
  char buf[160];
  const char* caminho = caminho_sd(argc >= 2 ? argv[1] : ".", buf, sizeof(buf));

  return com_cartao([&]() -> int {
    DIR* d = opendir(caminho);
    if (!d) { printf(tr("erro: %s nao abriu\n"), caminho); return 1; }

    printf("--- %s ---\n", caminho);
    struct dirent* e;
    int i = 0;
    while ((e = readdir(d)) != nullptr)
    {
      ceder(i++);
      char completo[300];
      snprintf(completo, sizeof(completo), "%s/%s", caminho, e->d_name);
      struct stat st;
      if (e->d_type == DT_DIR)            { printf(tr("  [dir]  %s\n"), e->d_name); }
      else if (stat(completo, &st) == 0)  { printf("  %6ld  %s\n", (long)st.st_size, e->d_name); }
      else                                { printf("     ?   %s\n", e->d_name); }
    }
    closedir(d);
    printf("%s", tr("--- fim ---\n"));
    return 0;
  });
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// apagar <path>  - removes ONE file from the SD card
// ---------------------------------------------------------------------------
//
// It exists so a recording can be started from scratch without taking the card out. Deleting is
// irreversible, so:
//   - it only accepts paths inside /sdcard/, so a slip of the finger cannot reach
//     anything else;
//   - one file at a time, no wildcard: no "apagar *".
int cmd_apagar(int argc, char** argv)
{
  if (argc < 2) { printf("%s", tr("uso: apagar apps/<App>/<arquivo>\n")); return 1; }
  char buf[160];
  const char* caminho = caminho_sd(argv[1], buf, sizeof(buf));

  if (strncmp(caminho, "/sdcard/", 8) != 0 || strstr(caminho, "..") != nullptr)
  {
    printf("%s", tr("recusado: so apago dentro de /sdcard/ (e sem '..')\n"));
    return 1;
  }

  return com_cartao([&]() -> int {
    struct stat st;
    if (stat(caminho, &st) != 0) { printf(tr("nao existe: %s\n"), caminho); return 1; }

    if (remove(caminho) != 0) { printf(tr("nao consegui apagar %s\n"), caminho); return 1; }
    printf(tr("apagado: %s (%ld bytes)\n"), caminho, (long)st.st_size);
    return 0;
  });
}

void listar_pasta(const char* pasta, const char* filtro)
{
  const int MAX = 200;
  tab5_arq_item_t* it = (tab5_arq_item_t*)heap_caps_malloc(sizeof(tab5_arq_item_t) * MAX, MALLOC_CAP_SPIRAM);
  if (!it) { printf("%s", tr("sem memoria\n")); return; }
  int total = 0;
  const int n = tab5_arq_listar(pasta, it, MAX, &total, TAB5_ARQ_ORDEM_NOME, filtro);
  if (n < 0) { printf(tr("nao abriu %s (disco ausente, escondida ou nao existe)\n"), pasta); }
  static const char* const TIPO[] = { "", "/", " (musica)", " (imagem)", " (texto)", " (berry)" };
  for (int i = 0; i < n && !console_interrompido(); ++i) {
    ceder(i);
    if (it[i].tipo == TAB5_ARQ_PASTA) { printf("  %s/\n", it[i].nome); }
    else { printf("  %-40s %10llu%s\n", it[i].nome, (unsigned long long)it[i].bytes, TIPO[it[i].tipo]); }
  }
  if (n >= 0) {
    printf(tr("%d de %d item(s); %s\n"), n, total, tab5_arq_gravavel(pasta) ? tr("gravavel") : tr("SO LEITURA"));
  }
  heap_caps_free(it);
}

// 'arquivos lixeira <root> [restaurar <name> | excluir <name> | esvaziar | antigos <days>]'
// With no action, lists. The name may have spaces ("x (2).txt"): joins the rest of the line.
static void lixeira_arquivos(int argc, char** argv, const char* raiz, void (*erro)(tab5_arq_erro_t))
{
  const char* acao = argc >= 4 ? argv[3] : "";
  char nome[TAB5_ARQ_NOME_MAX] = "";
  for (int i = 4; i < argc; ++i) {
    snprintf(nome + strlen(nome), sizeof(nome) - strlen(nome), "%s%s", i > 4 ? " " : "", argv[i]);
  }
  if (eh(acao, "esvaziar")) { erro(tab5_arq_esvaziar_lixeira(raiz)); return; }
  if (eh(acao, "restaurar") || eh(acao, "excluir")) {
    if (!nome[0]) { printf("%s", tr("uso: arquivos lixeira <raiz> restaurar|excluir <nome>\n")); return; }
    erro(eh(acao, "restaurar") ? tab5_arq_lixeira_restaurar(raiz, nome) : tab5_arq_lixeira_excluir(raiz, nome));
    return;
  }
  if (eh(acao, "antigos")) {
    if (argc < 5) { printf("%s", tr("uso: arquivos lixeira <raiz> antigos <dias>\n")); return; }
    printf(tr("%d item(s) excluido(s)\n"), tab5_arq_lixeira_limpar_antigos(raiz, atoi(argv[4])));
    return;
  }
  if (acao[0]) { printf("%s", tr("acao desconhecida: use restaurar, excluir, esvaziar ou antigos\n")); return; }

  // List
  const int MAX = 200;
  tab5_arq_lixo_t* it = (tab5_arq_lixo_t*)heap_caps_malloc(sizeof(tab5_arq_lixo_t) * MAX, MALLOC_CAP_SPIRAM);
  if (!it) { printf("%s", tr("sem memoria\n")); return; }
  uint64_t total = 0;
  const int n = tab5_arq_lixeira_listar(raiz, it, MAX, &total);
  for (int i = 0; i < n && !console_interrompido(); ++i) {
    ceder(i);
    char data[16] = "?";
    if (it[i].quando) {
      const time_t t = (time_t)it[i].quando;
      struct tm tm;
      localtime_r(&t, &tm);
      strftime(data, sizeof(data), "%Y-%m-%d", &tm);
    }
    printf("%3d  %s%s  %s  %s  <- %s\n", i + 1, it[i].nome, it[i].pasta ? "/" : "", data,
           tamanho_curto(it[i].bytes), it[i].origem[0] ? it[i].origem : "?");
  }
  printf(tr("%d item(s), %s no total\n"), n, tamanho_curto(total));
  heap_caps_free(it);
}

// The file engine (services/tab5_arquivos): the same one the Arquivos (Files) app
// uses, for testing without a screen. Absolute paths (/sdcard/..., /usb/...) or
// relative to the session's current folder ('cd').
void c_arquivos(int argc, char** argv)
{
  static void (*const erro)(tab5_arq_erro_t) = [](tab5_arq_erro_t r) {
    printf(r == TAB5_ARQ_OK ? tr("ok\n") : tr("recusado: %s\n"), tab5_arq_erro_texto(r));
  };
  char c2[TAB5_ARQ_CAMINHO_MAX] = "", c3[TAB5_ARQ_CAMINHO_MAX] = "";
  if (argc >= 3) { resolver(argv[2], c2, sizeof(c2)); }
  if (argc >= 4) { resolver(argv[3], c3, sizeof(c3)); }
  if (argc >= 2 && eh(argv[1], "ls")) {
    listar_pasta(argc >= 3 ? c2 : sessao()->cwd, argc >= 4 ? argv[3] : nullptr);
    return;
  }
  if (argc >= 4 && (eh(argv[1], "cp") || eh(argv[1], "mv"))) {
    const char* o = c2;
    erro(tab5_arq_copiar(&o, 1, c3, eh(argv[1], "mv")));
    printf("%s", tr("acompanhe com 'arquivos estado'\n"));
    return;
  }
  if (argc >= 3 && eh(argv[1], "rm"))     { erro(tab5_arq_apagar(c2)); return; }
  if (argc >= 4 && eh(argv[1], "mkdir"))  { erro(tab5_arq_criar_pasta(c2, argv[3])); return; }
  if (argc == 3 && eh(argv[1], "mkdir")) {
    // A single path (the usual mkdir): the parent folder and the new name.
    char* b = strrchr(c2, '/');
    if (!b || !b[1]) { printf("%s", tr("uso: mkdir <pasta nova>\n")); return; }
    *b = 0;
    erro(tab5_arq_criar_pasta(c2[0] ? c2 : "/", b + 1));
    return;
  }
  if (argc >= 4 && eh(argv[1], "ren"))    { erro(tab5_arq_renomear(c2, argv[3])); return; }
  if (argc >= 3 && eh(argv[1], "lixeira")) { lixeira_arquivos(argc, argv, c2, erro); return; }
  if (argc >= 2 && eh(argv[1], "cancelar")) { tab5_arq_cancelar(); printf("%s", tr("pedido\n")); return; }
  if (argc >= 3 && (eh(argv[1], "copiar") || eh(argv[1], "recortar"))) {
    const char* c = c2;
    printf(tab5_arq_area_por(&c, 1, eh(argv[1], "recortar")) ? tr("na area\n") : tr("recusado\n"));
    return;
  }
  if (argc >= 3 && eh(argv[1], "colar")) { erro(tab5_arq_colar(c2)); return; }
  // status
  tab5_arq_trabalho_t t;
  tab5_arq_trabalho(&t);
  static const char* const FASE[] = { "parado", "contando", "copiando", "apagando", "feito", "FALHOU", "cancelado" };
  printf(tr("trabalho: %s%s, %d/%d arquivo(s), %.1f/%.1f MB, %lu KB/s\n"),
         FASE[t.fase <= TAB5_ARQ_CANCELADO ? t.fase : 0], t.mover ? tr(" (mover)") : "",
         t.itens_feitos, t.itens, t.bytes_feitos / 1048576.0, t.bytes / 1048576.0, (unsigned long)t.kbps);
  if (t.atual[0])   { printf(tr("arquivo: %s\n"), t.atual); }
  if (t.destino[0]) { printf(tr("destino: %s\n"), t.destino); }
  if (t.erro[0])    { printf(tr("erro: %s\n"), t.erro); }
  bool rec = false;
  const int na = tab5_arq_area(&rec);
  printf(tr("area de transferencia: %d item(s)%s\n"), na, na && rec ? tr(" (recortados)") : "");
  for (int i = 0; i < na && i < 8; ++i) {
    char c[TAB5_ARQ_CAMINHO_MAX];
    if (tab5_arq_area_item(i, c, sizeof(c))) { printf("  %s\n", c); }
  }
}
