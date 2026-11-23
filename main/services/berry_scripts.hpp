// THE BERRY SCRIPTS ON THE CARD - list, read and write the .be files in apps/Berry.
//
// Whoever edits a script (the on-device Editor, the web editor in berry_web) goes
// through here: the same folder, the same size limit, the same valid names.
// Everything through the tab5_sd doors (mount/open to read, replace to write).
//
// THEY BLOCK (card): never from the UI task - from the worker
// (sistema_trabalho) or from a service task (the web server).
#pragma once

#include <stddef.h>
#include "tab5_arquivos.hpp"

// The maximum size of an EDITABLE script, including the '\0'. The on-device editor
// stores line starts as int16 (up to 32767) - the web one has the same ceiling.
// The app itself runs at any size: the limit only applies to editing.
#define BERRY_SCRIPT_MAX 32767

// The .be files in the folder (files only, without the apps' data folders), by name.
// Returns how many (up to 'max'); -1 = no card.
int berry_scripts_listar(tab5_arq_item_t* out, int max);

// Reads the WHOLE of 'arq' into 'buf' (with '\0'). NEVER TRUNCATES: a file bigger than
// tam - 1 returns BERRY_LER_GRANDE and an empty buf - whoever edits and writes back
// a truncated script breaks the app (that is what happened with Mostruario).
typedef enum { BERRY_LER_OK = 0, BERRY_LER_FALHOU, BERRY_LER_GRANDE } berry_ler_t;
berry_ler_t berry_script_ler(const char* arq, char* buf, size_t tam);

// Writes the whole of 'arq' (tab5_sd_substituir: never half a file; with the card
// lent to the computer, it goes to the journal). false = not written.
bool berry_script_gravar(const char* arq, const char* txt, size_t n);

// Acceptable file name for a script: ends in .be, no slash, does not
// start with a dot, only letters, numbers, space, '_', '-' and '.'. For whoever
// receives the name from outside (the browser).
bool berry_script_nome_valido(const char* arq);

// A script's app name: the header's '# nome:', or the file name without the
// .be. Does not block (only reads the text).
void berry_script_nome_app(const char* txt, const char* arq, char* out, size_t n);
