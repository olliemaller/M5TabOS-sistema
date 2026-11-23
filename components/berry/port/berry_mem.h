 /* M5tabOS: accounting of Berry's memory (port/be_port_m5.c). */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ceiling for ALL Berry VMs together. A measured app uses tens of KB
 * (the largest example, ~60 KB at peak): 4 MB leaves room for many live apps
 * at once and still leaves PSRAM for the system's own users. Layers
 * (tela.camada) do NOT count here - they belong to M5GFX and have their own limit. */
#define BERRY_ORCAMENTO (4u * 1024u * 1024u)

/* A new VM loads its modules OUTSIDE any pcall: out of memory there, Berry
 * would abort the device. Whoever creates one checks beforehand that it fits. */
#define BERRY_FOLGA_VM  (64u * 1024u)

size_t berry_mem_usada(void);   /* bytes in use right now, all VMs */
size_t berry_mem_pico(void);    /* the highest seen since boot */
int    berry_mem_cabe(size_t n);

#ifdef __cplusplus
}
#endif
