// Mesh service - the node's CHANNELS: active, with names, remembered from one boot to
// the next. Split out of tab5_mesh.cpp (the service); what the parts share lives
// in mesh/tab5_mesh_interno.hpp.

#include <string.h>
#include "esp_log.h"
#include <stdio.h>
#include "tab5_mutex.hpp"
#include "tab5_mesh_mqtt.hpp"
#include "nvs.h"
#include "mesh/tab5_mesh_interno.hpp"

static const char *TAG = "mesh";

static bool s_canais_lidos      = false;   // NVS has already been consulted

// ---------------------------------------------------------------------------
// THE NODE'S CHANNELS - active, with names, and remembered from one boot to the next
// ---------------------------------------------------------------------------
// The channel configuration only arrives in the radio's dump. Without storing it, with the node
// away (query only, or internet mode) there was no way of knowing a channel
// had been DISABLED: the canal-N.txt file stayed on the card and the
// conversation came back on the folder scan. It lives in mesh's own NVS, next
// to our number - it is information the radio told us, not a preference.
//
// Saved in a BATCH (s_canais_sujos), at the end of the dump or on the task's 15 s
// round: the dump brings the 8 channels in sequence and each would be a write.
typedef struct { uint8_t ativo; char nome[MESH_NOME_CONVERSA]; } canal_salvo_t;
#define MESH_NVS_CANAIS "canais"

void recordar_canais(void)
{
  if (s_canais_lidos) { return; }
  s_canais_lidos = true;
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READONLY, &h) != ESP_OK) { return; }
  canal_salvo_t tab[MESH_MAX_CANAIS];
  size_t n = sizeof(tab);
  if (nvs_get_blob(h, MESH_NVS_CANAIS, tab, &n) == ESP_OK && n == sizeof(tab)) {
    for (int i = 0; i < MESH_MAX_CANAIS; ++i) {
      s_canal_ativo[i] = tab[i].ativo != 0;
      tab[i].nome[sizeof(tab[i].nome) - 1] = 0;
      snprintf(s_canal_nome[i], sizeof(s_canal_nome[i]), "%s", tab[i].ativo ? tab[i].nome : "");
    }
    s_canais_conhecidos = true;
  }
  nvs_close(h);
}

// The COPY under the lock, the WRITE outside it: NVS writes flash (the cache
// stops, ms), and with the lock held the radio waited along with it to store the
// next frame. Rides along on the "cartao" task (see mesh_na_carona).
void salvar_canais(void)
{
  canal_salvo_t tab[MESH_MAX_CANAIS];
  travar();
  if (!s_canais_sujos) { destravar(); return; }
  s_canais_sujos = false;
  memset(tab, 0, sizeof(tab));
  for (int i = 0; i < MESH_MAX_CANAIS; ++i) {
    tab[i].ativo = s_canal_ativo[i] ? 1 : 0;
    snprintf(tab[i].nome, sizeof(tab[i].nome), "%s", s_canal_nome[i]);
  }
  destravar();
  nvs_handle_t h;
  if (nvs_open(MESH_NVS, NVS_READWRITE, &h) != ESP_OK) { return; }
  nvs_set_blob(h, MESH_NVS_CANAIS, tab, sizeof(tab));
  nvs_commit(h);
  nvs_close(h);
}

// A channel's display name. An empty name in Meshtastic means "the name
// of the modem preset" - LongFast in the factory default.
static void nome_do_canal(int idx, char* out, size_t tam)
{
  if (idx >= 0 && idx < MESH_MAX_CANAIS && s_canal_nome[idx][0]) { snprintf(out, tam, "%s", s_canal_nome[idx]); }
  else if (idx == 0) { snprintf(out, tam, "LongFast"); }
  else               { snprintf(out, tam, "Canal %d", idx); }
}

// Drops a channel's conversation from the column, WITHOUT marking it as deleted: what
// removed it was the node's configuration, not you. Re-enabling the channel on the node, it
// comes back by itself on the next dump, with the history from the card.
static void esconder_conversa_canal(int idx)
{
  for (int i = 0; i < MESH_MAX_CONVERSAS; ++i) {
    if (s_conv[i].usada && s_conv[i].canal && s_conv[i].idx == idx) {
      memset(&s_conv[i], 0, sizeof(s_conv[i]));
      s_conv_seq[i] = 0;
      ++s_conv_ver;
      publicar_notif();   // its unread ones leave the total
    }
  }
}

// A channel from the configuration (FromRadio field 10), WITH proto3's zeros already
// handled (see the reader). With the lock held.
void aplicar_canal(int idx, int papel, const char* nome_radio, const uint8_t* psk, size_t n_psk)
{
  recordar_canais();   // before comparing: NVS must not overwrite what has just arrived
  const bool ativo = (papel != 0);
  char nome[MESH_NOME_CONVERSA] = "";
  if (ativo) {
    if (nome_radio[0]) { snprintf(nome, sizeof(nome), "%s", nome_radio); }
    else if (idx == 0) { snprintf(nome, sizeof(nome), "LongFast"); }
    else               { snprintf(nome, sizeof(nome), "Canal %d", idx); }
  }
  mesh_mqtt_aprender_canal(idx, papel, nome, ativo ? psk : nullptr, ativo ? n_psk : 0);

  if (s_canal_ativo[idx] != ativo || strcmp(s_canal_nome[idx], nome) != 0 || !s_canais_conhecidos) {
    s_canais_sujos = true;
  }
  s_canal_ativo[idx] = ativo;
  snprintf(s_canal_nome[idx], sizeof(s_canal_nome[idx]), "%s", nome);
  s_canais_conhecidos = true;

  if (!ativo) {
    // DISABLED ON THE NODE: leaves the column. That was the case of the channel turned off in the
    // web client that kept showing up here.
    esconder_conversa_canal(idx);
    return;
  }
  ESP_LOGI(TAG, "channel %d: \"%s\"", idx, nome);

  // THE CHANNEL EXISTS, SO THE CONVERSATION EXISTS - unless you deleted it. Without this
  // a channel created on the phone stayed invisible until someone spoke in it.
  if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_criar_conversa(true, (uint8_t)idx, 0); }
  // Renames any that already exists. The file goes by INDEX (canal-0.txt), so
  // renaming does not detach the history.
  for (int k = 0; k < MESH_MAX_CONVERSAS; ++k) {
    if (s_conv[k].usada && s_conv[k].canal && s_conv[k].idx == idx) {
      snprintf(s_conv[k].nome, sizeof(s_conv[k].nome), "%s", nome);
    }
  }
}

// The conversations of the active channels (without the deleted ones), or just the primary when
// no configuration is known yet. With the lock held.
void criar_conversas_dos_canais(void)
{
  recordar_canais();
  bool algum = false;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (!s_canais_conhecidos || !s_canal_ativo[idx]) { continue; }
    algum = true;
    if (!esta_apagada(true, (uint8_t)idx)) { achar_ou_criar_conversa(true, (uint8_t)idx, 0); }
  }
  if (!algum && !esta_apagada(true, 0)) { achar_ou_criar_conversa(true, 0, 0); }
}

// Indices of the channels to OFFER: the active ones; with none known, the primary.
static int lista_canais(uint8_t out[MESH_MAX_CANAIS + 1])
{
  int n = 0;
  for (int idx = 0; idx < MESH_MAX_CANAIS; ++idx) {
    if (s_canais_conhecidos && s_canal_ativo[idx]) { out[n++] = (uint8_t)idx; }
  }
  if (n == 0) { out[n++] = 0; }
  return n;
}

int tab5_mesh_num_canais(void)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  travar();
  recordar_canais();
  const int n = lista_canais(l);
  destravar();
  return n;
}

bool tab5_mesh_canal(int i, uint8_t* idx_saida, char* nome, size_t tam, bool* visivel)
{
  uint8_t l[MESH_MAX_CANAIS + 1];
  bool ok = false;
  travar();
  recordar_canais();
  const int n = lista_canais(l);
  if (i >= 0 && i < n) {
    const uint8_t idx = l[i];
    if (idx_saida) { *idx_saida = idx; }
    if (nome)      { nome_do_canal(idx, nome, tam); }
    if (visivel) {
      *visivel = false;
      for (int k = 0; k < MESH_MAX_CONVERSAS; ++k) {
        if (s_conv[k].usada && s_conv[k].canal && s_conv[k].idx == idx) { *visivel = true; break; }
      }
    }
    ok = true;
  }
  destravar();
  return ok;
}
