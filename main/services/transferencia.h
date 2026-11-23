// CLIPBOARD - a copied text, to paste somewhere else.
//
// As on phones: ONE item only, text, which the next copy replaces.
// There is no history. Holds up to TRANSF_MAX bytes (a whole note); anything
// beyond is cut at a UTF-8 character boundary. The buffer lives in PSRAM
// and only exists after the first copy - no copy, zero cost.
//
// Whoever copies or pastes shows the message in the status bar (sistema_recado, app.h);
// the service only stores. Safe from any task (Berry included).
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define TRANSF_MAX 4096   // including the '\0'

// Copies 'texto' (empty or null changes nothing). false = out of memory.
bool   transf_copiar(const char* texto);
// The copied text into 'out' (with '\0', cut at 'cap'). Returns the bytes
// written without the '\0'; 0 = the clipboard is empty.
size_t transf_colar(char* out, size_t cap);
bool   transf_vazia(void);
