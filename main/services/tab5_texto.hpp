// tab5_texto: open a large text/CSV file (megabytes) and read a range of lines.
//
// Serves any app that shows paged text (today, Files). The engine lives
// here; the app only asks and draws.
//
//   tab5_texto_t* t = tab5_texto_abrir(caminho, sistema_trabalho_cancelado);
//   int total = tab5_texto_linhas(t);
//   int n = tab5_texto_ler(t, 1000, 48, &buf[0][0], 601);   // lines 1000..1047
//   tab5_texto_fechar(t);
//
// HOW IT WORKS: opening walks the file ONCE, counts the lines and notes where
// each run of TAB5_TEXTO_BLOCO lines starts (the index that makes jumping to
// the end or to a scroll-bar position fast). Reading jumps via the index to
// near the requested line. Both read through tab5_leitor (fast POSIX read). The
// disk is only safe DURING each call, not between them.
//
// BLOCKS (reads the disk): call outside the interface task, on the worker.
// A tab5_texto_t belongs to ONE task at a time.
#pragma once
#include <stddef.h>

// Every how many lines the index keeps a jump point.
#define TAB5_TEXTO_BLOCO 28

typedef struct tab5_texto tab5_texto_t;

// Opens and builds the index. 'cancelado' (may be nullptr) is checked every so
// often: if it returns true, gives up. nullptr = did not open, was cancelled or out of memory.
tab5_texto_t* tab5_texto_abrir(const char* caminho, bool (*cancelado)(void));

// How many lines the file has (the last one without '\n' counts).
int tab5_texto_linhas(const tab5_texto_t* t);

// Reads up to 'max' lines starting at line 'inicio' (0 = the first) into
// 'saida', which has 'max' lines of 'larg' bytes each (one after another, each
// terminated by '\0'). The trailing '\r' is dropped; a line longer than 'larg' - 1
// is cut and ends in " ..."; a .csv file gets a space after each ',' or
// ';'. Returns how many lines were read (0 = past the end) or -1 if the
// file did not open.
int tab5_texto_ler(tab5_texto_t* t, int inicio, int max, char* saida, size_t larg);

void tab5_texto_fechar(tab5_texto_t* t);   // accepts nullptr
