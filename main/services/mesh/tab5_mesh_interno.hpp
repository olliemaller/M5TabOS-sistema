// What the Mesh service files share: the constants and types used by more
// than one of them, the state (declared here, defined in tab5_mesh.cpp or in
// the file indicated) and the internal functions that cross files. For them
// only: no app includes this header - apps talk to the service through
// tab5_mesh.hpp.
//
// The service was split by subject (it only moved between files, no logic changed):
//   tab5_mesh.cpp             state, in-flight sends and outcome, task, sending, send queue, service API
//   mesh/tab5_mesh_hora.cpp       corrected timestamp, what time it is, live time
//   mesh/tab5_mesh_nos.cpp        node table, favourites, search, name cache, NodeDB on the SD card
//   mesh/tab5_mesh_conversas.cpp  conversations and history on the SD card, deleted ones, writing, column order
//   mesh/tab5_mesh_canais.cpp     the node's channels
//   mesh/tab5_mesh_receber.cpp    interpreting messages from the radio
//   mesh/tab5_mesh_gps.cpp        GPS position for the mesh
#pragma once

#include "tab5_mesh.hpp"

// ---------------------------------------------------------------------------
// Constants and types used by more than one file
// ---------------------------------------------------------------------------
// The service's own NVS: what the radio told us (our number, channels,
// favourites), kept apart from the person's preferences (settings).
#define MESH_NVS "mesh"
#define MESH_PASTA "Meshtastic"
#define MESH_ARQ_ENVIOS "envios.txt"
// The 2020 floor is so we don't accept a timestamp from a radio that hasn't synced yet
// (it sends 0, or a small uptime number) and anchor the reference in the year
// 1970 - which would make every node appear as seen 56 years ago.
#define EPOCH_MINIMO 1577836800u          // 2020-01-01
#define MESH_MAX_FAVS  32
// Data (dentro do MeshPacket): 1=portnum 2=payload 6=request_id 9=bitfield
#define PORT_TEXTO      1
#define PORT_POSICAO    3
#define PORT_NODEINFO   4
#define PORT_ROTEAMENTO 5
#define PORT_TELEMETRIA 67

// ---------------------------------------------------------------------------
// State used by more than one file (each defined in the file indicated)
// ---------------------------------------------------------------------------
// tab5_mesh.cpp:
extern bool s_servico_ativo;
extern uint8_t s_via;
extern mesh_no_t s_nos[MESH_MAX_NOS];
extern uint32_t s_despejos;
extern mesh_msg_t s_msgs[MESH_MAX_MSGS];
extern int s_n_msgs;
extern uint32_t s_total_msgs;
extern int s_nao_lidas;
extern int s_nao_lidas_dir;
extern mesh_saude_t s_saude;
extern uint32_t s_meu_num;
extern char s_firmware[24];
extern uint32_t s_ultimo_quadro_ms;
extern int s_notif;
extern char s_canal_nome[MESH_MAX_CANAIS][MESH_NOME_CONVERSA];
extern bool s_canal_ativo[MESH_MAX_CANAIS];
extern bool s_canais_conhecidos;
extern bool s_canais_sujos;
extern volatile bool s_radio_reiniciou;
extern mesh_conversa_t s_conv[MESH_MAX_CONVERSAS];
extern uint32_t s_conv_seq[MESH_MAX_CONVERSAS];
extern uint32_t s_seq;
extern uint32_t s_conv_ver;
extern uint32_t s_c_quadros, s_c_pacotes, s_c_telemetria;
extern uint32_t s_c_localstats, s_c_texto, s_c_nodeinfo;
extern int s_ultima_variante;

// tab5_mesh_hora.cpp:
extern uint32_t s_config_ok_ms;

// tab5_mesh_nos.cpp:
extern uint32_t s_nos_ver;
extern uint32_t s_favs[MESH_MAX_FAVS];
extern int s_n_favs;
extern bool s_nomes_lidos;
extern uint32_t s_nodedb_ver_salva;

// ---------------------------------------------------------------------------
// Internal functions that cross files (each defined in the file indicated)
// ---------------------------------------------------------------------------
// tab5_mesh.cpp:
void anotar_envio(uint32_t id, uint32_t para);
void marcar_envio(uint32_t id, int erro, uint32_t de, bool so_mqtt = false);
void travar(void);
void destravar(void);
void lembrar_meu_numero(uint32_t num);
uint32_t recordar_meu_numero(void);
bool enlace_enviar(const uint8_t* corpo, size_t n);

// tab5_mesh_hora.cpp:
void lembrar_carimbo(uint32_t id, uint32_t quando);
void ao_saltar_relogio(int32_t delta);
void marcar_epoch(uint32_t carimbo);
bool trafego_ao_vivo(void);
void marcar_hora_viva(uint32_t carimbo);

// tab5_mesh_nos.cpp:
void fixar_se_pelo_ar(mesh_no_t* no);
void recordar_favs(void);
bool radio_aceita_admin(void);
void enviar_favs_pendentes(void);
void fav_do_radio(uint32_t num, bool fav);
const char* longo_conhecido(uint32_t num);
void nome_legivel(const char* in, char* out, size_t tam);
bool curto_utilizavel(const char* s);
void nome_exibicao(uint32_t num, char* out, size_t out_tam);
void lembrar_nome(uint32_t num, const char* curto, const char* longo);
void descarregar_nomes(void);
void carregar_nomes(void);
mesh_no_t* achar_no(uint32_t num);
void salvar_nodedb(void);
void carregar_nodedb(void);

// tab5_mesh_conversas.cpp:
void guardar_nao_lidas(void);
bool ha_nao_lidas_guardadas(void);
bool esta_apagada(bool canal, uint32_t chave);
void desmarcar_apagada(bool canal, uint32_t chave);
int achar_ou_criar_conversa(bool canal, uint8_t idx, uint32_t no);
void gravar_msg(int ci, const mesh_msg_t* m);
void publicar_notif(void);

// tab5_mesh_canais.cpp:
void recordar_canais(void);
void salvar_canais(void);
void aplicar_canal(int idx, int papel, const char* nome_radio, const uint8_t* psk, size_t n_psk);
void criar_conversas_dos_canais(void);

// tab5_mesh_receber.cpp:
void guardar_msg(uint32_t de, uint32_t para, uint8_t canal, const char* txt, uint32_t quando,
                 float snr, int32_t rssi, uint32_t id, bool via_mqtt, uint8_t saltos);
void ler_from_radio(const uint8_t* buf, size_t n);

// tab5_mesh_gps.cpp:
bool enviar_posicao(void);
