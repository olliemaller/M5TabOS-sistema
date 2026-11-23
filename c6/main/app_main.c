/*
 * Firmware for the M5Stack Tab5's ESP32-C6 coprocessor.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS
 * ---------------------------------------------------------------------------
 * The ESP32-P4 has no radio. Everything Wi-Fi goes through the C6 over SDIO,
 * via esp_hosted. And esp_hosted does NOT carry ESP-NOW with it: esp_now_init,
 * _send and _register_recv_cb do not even link on the P4 side (the esp_now.h
 * there is only "injected", it makes things compile and has no
 * implementation). esp_wifi_80211_tx and promiscuous mode are not proxied either.
 *
 * In other words: to have ESP-NOW on the Tab5, it has to run HERE, where the
 * radio is. The P4 talks to this firmware over esp_hosted's peer_data channel -
 * an opaque byte pipe that already exists and spares us touching protobuf.
 *
 * ---------------------------------------------------------------------------
 * THIS REPLACES THE M5STACK FIRMWARE
 * ---------------------------------------------------------------------------
 * The Tab5's C6 ships with M5Stack's firmware (esp_hosted 2.12.6, backup in
 * c6/backup_fabrica_2.12.6.bin). The M5tabOS host is 3.0.7, and this firmware
 * is the same version - the boot checks ("host=3.0.7 coprocessor=3.0.7 (match)").
 *
 * But it is the firmware that makes Wi-Fi work. Hence the TWO safety nets
 * below.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "espnow.h"            /* ONLY the types from the espressif/esp-now component (see TWO DIALECTS) */
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include <stdlib.h>   /* free of the FTM report */
#if CONFIG_IEEE802154_ENABLED
#include "esp_ieee802154.h"
#include "esp_coexist.h"       /* esp_coex_wifi_i154_enable */
#include "esp_coex_i154.h"     /* esp_coex_ieee802154_status_disable */
#endif
#include "esp_wifi.h"
#include "esp_private/wifi.h"   /* esp_wifi_internal_tx (the counting wrapper) */
#include "eh_cp_feat_wifi.h"     /* eh_cp_feat_wifi_is_station_connected */
#include "esp_pm.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "eh_cp_feat_peer_data.h"
#include "tab5_c6.h"   /* the contract, shared with the P4 */

static const char *TAG = "tab5c6";

/* ---------------------------------------------------------------------------
 * PROTOCOL WITH THE P4
 * ---------------------------------------------------------------------------
 * The msg_ids are a contract between the two sides, and they live in a single
 * file: compartilhado/tab5_c6.h, included by both projects.
 *
 * Payload format, always raw - no protobuf here. The channel already delivers
 * (msg_id, bytes, length), and inventing serialisation on top of that would be
 * work for no return: both sides are ours and compile together.
 */
#define MSG_HOST_VIVO     C6_MSG_HOST_VIVO
#define MSG_HOST_ENVIAR   C6_MSG_HOST_ENVIAR
#define MSG_HOST_CANAL    C6_MSG_HOST_CANAL
#define MSG_HOST_ESPNOW   C6_MSG_HOST_ESPNOW
#define MSG_C6_PRONTO     C6_MSG_PRONTO
#define MSG_C6_RECEBIDO   C6_MSG_RECEBIDO
#define MSG_C6_ESTADO     C6_MSG_ESTADO

/* What THIS firmware can do: sent in PRONTO and INFO. The three radio extras
 * only light their bit if the module was COMPILED in (depends on c6/'s
 * sdkconfig) - so the P4 hides a feature this build did not include, without
 * reopening the device. */
#if CONFIG_ESP_WIFI_CSI_ENABLED
#define CAP_CSI C6_CAP_CSI
#else
#define CAP_CSI 0
#endif
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
#define CAP_FTM C6_CAP_FTM
#else
#define CAP_FTM 0
#endif
#if CONFIG_IEEE802154_ENABLED
#define CAP_154 C6_CAP_154
#else
#define CAP_154 0
#endif
#define CAPS (C6_CAP_ESPNOW | C6_CAP_FAREJO | C6_CAP_CRU | C6_CAP_INFO \
              | CAP_CSI | CAP_FTM | CAP_154)

/* What THIS chip does, beyond what was compiled. The FTM initiator does not
 * exist on C6 revisions v0.0/v0.1: esp_wifi_init itself
 * (esp_wifi/src/wifi_init.c) clears its bit in feature_caps on those revisions
 * - a silicon erratum - and initiate_session replies ESP_ERR_NOT_SUPPORTED
 * ("FTM Initiator mode not supported!"). Advertising the cap on that chip
 * would promise something it does not do. */
static uint8_t caps_reais(void)
{
    uint8_t c = CAPS;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    if (ci.model == CHIP_ESP32C6 && ci.revision <= 1) { c &= (uint8_t)~C6_CAP_FTM; }
    return c;
}

/* Chip revision for INFO: 0x80 marks "valid"; the rest is major*16+minor
 * (esp_chip_info gives major*100+minor). */
static uint8_t chip_rev_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    return (uint8_t)(0x80 | (((ci.revision / 100) & 0x7) << 4) | ((ci.revision % 100) & 0xF));
}

static const uint8_t DIFUSAO[ESP_NOW_ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* Diagnostics for the radio extras, returned in INFO: without the C6's serial
 * log (which needs the Downloader), it is the only way for the P4 to know
 * where it stopped. */
static volatile int16_t  s_csi_err = 0, s_z154_err = 0;
static volatile uint32_t s_csi_cb = 0, s_csi_env = 0, s_z154_rx = 0;
static volatile int16_t  s_csi_err2 = 0;
static volatile bool     s_z154_lig = false;          /* 802.15.4 listening right now */
static bool              s_z154_habilitado __attribute__((unused)) = false;   /* esp_ieee802154_enable done (only once) */
static volatile uint8_t  s_csi_etapa = 0, s_csi_modo = 0;
/* The radio extras are turned on by the RPC task and off by the watchdog
 * (esp_timer): this lock stops the two touching the driver at the same time. */
static SemaphoreHandle_t s_radio_mtx = NULL;

/* ---------------------------------------------------------------------------
 * SAFETY NET 2 - at the APPLICATION level
 * ---------------------------------------------------------------------------
 * Net 1 is CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in sdkconfig.defaults. It only
 * works with OUR bootloader: OTA does not replace the bootloader, and
 * M5Stack's may not have rollback. Flashing over the cable (idf.py flash)
 * writes this project's bootloader, partition table and otadata together.
 *
 * This second net does not depend on it. If the P4 does not say "I am
 * listening to you" within the deadline, WE OURSELVES point the boot back to
 * the previous partition and restart. It covers the likely OTA case - we come
 * up, but the SDIO link does not close and the Tab5 is left without Wi-Fi. It
 * only applies to an image that arrived via OTA (see NEW IMAGE GUARD); one
 * flashed over the cable has nowhere to go back to.
 *
 * What it does NOT cover: a failure before this code runs (panic loop at
 * boot). For that only the bootloader, or physical access to the C6.
 */
#define PRAZO_HOST_MS (C6_PRAZO_HOST_S * 1000)

/* NEW IMAGE GUARD - why the bootloader's rollback is not enough
 *
 * An OTA over the link restarts the C6 TWICE in a row: the 1st by esp_hosted
 * itself (2 s after activate) and the 2nd by the P4, which sees SDIO drop,
 * restarts itself and, on boot, resets the C6 again
 * (CP_RESET_STRATEGY_ALWAYS). The bootloader reads "restarted while still in
 * PENDING_VERIFY" as a broken image and goes back to the previous one - that
 * is, EVERY OTA would be undone, good or bad.
 *
 * So the decision moves elsewhere:
 *   - the bootloader only covers a panic BEFORE app_main (the image never gets
 *     here): on the new image's 1st boot, as soon as we get here, it is
 *     marked valid for the bootloader;
 *   - from then on this guard decides, with its state in NVS ("tab5c6"):
 *     "pend" = new image not yet confirmed by the P4, "tent" = its boots
 *     without confirmation. Past GUARDA_MAX_BOOTS (panic loop after app_main,
 *     a link that never closes), or the deadline ran out without the P4's
 *     VIVO: go back to the other slot.
 *   - an image flashed over the cable is never pending: nowhere to go back to. */
#define GUARDA_MAX_BOOTS 4

static esp_timer_handle_t s_carencia = NULL;
static bool s_confirmado = false;
static nvs_handle_t s_guarda = 0;
static volatile bool s_pendente = false;

static void guarda_gravar(uint8_t pend, uint8_t tent)
{
    if (!s_guarda) { return; }
    nvs_set_u8(s_guarda, "pend", pend);
    nvs_set_u8(s_guarda, "tent", tent);
    nvs_commit(s_guarda);
}

static void desistir_e_voltar(void *arg)
{
    (void)arg;
    if (s_confirmado) { return; }

    /* "THE OTHER slot" (next_update_partition), where the image that flashed
     * this one remained. NOT esp_ota_get_last_invalid_partition(): it only
     * finds an INVALID/ABORTED partition, a state that does not exist before a
     * rollback. The factory partition is the net below, if there ever is one. */
    const esp_partition_t *cand[2] = {
        esp_ota_get_next_update_partition(NULL),
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL),
    };

    ESP_LOGE(TAG, "new image not confirmed by the host - reverting");

    for (int i = 0; i < 2; ++i) {
        if (!cand[i]) { continue; }

        esp_app_desc_t d;
        if (esp_ota_get_partition_description(cand[i], &d) == ESP_OK) {
            ESP_LOGE(TAG, "trying '%s' (%s %s)", cand[i]->label,
                     d.project_name, d.version);
        } else {
            ESP_LOGE(TAG, "'%s' has no valid image - skipping", cand[i]->label);
            continue;
        }

        if (esp_ota_set_boot_partition(cand[i]) == ESP_OK) {
            /* Clear the guard FIRST: the image we go back to reads the same NVS, and
             * with "pend" set it would also think it was new and revert again. */
            guarda_gravar(0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "'%s' rejected", cand[i]->label);
    }

    /* No way back: stay up and stop counting, otherwise it would revert on every boot. */
    guarda_gravar(0, 0);
    s_pendente = false;
    ESP_LOGE(TAG, "NOWHERE TO GO BACK TO. Staying up.");
}

/* In app_main, right after NVS: decides whether this image is on trial. */
static void guarda_iniciar(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *rodando = esp_ota_get_running_partition();
    const bool chegou_por_ota = rodando
        && esp_ota_get_state_partition(rodando, &st) == ESP_OK
        && st == ESP_OTA_IMG_PENDING_VERIFY;

    uint8_t pend = 0, tent = 0;
    if (nvs_open("tab5c6", NVS_READWRITE, &s_guarda) != ESP_OK) {
        /* Without NVS there is no guard of our own: the bootloader's rollback and
         * the classic grace period remain (VIVO marks it valid). */
        s_guarda = 0;
        pend = chegou_por_ota ? 1 : 0;
    } else {
        if (chegou_por_ota) {
            pend = 1; tent = 0;
        } else {
            nvs_get_u8(s_guarda, "pend", &pend);
            nvs_get_u8(s_guarda, "tent", &tent);
        }
        if (pend) { ++tent; }
        guarda_gravar(pend, tent);
        /* With the guard written, the bootloader has already done its part. */
        if (chegou_por_ota) { esp_ota_mark_app_valid_cancel_rollback(); }
    }

    s_pendente = pend != 0;
    if (!s_pendente) {
        ESP_LOGI(TAG, "image confirmed: no grace period");
        return;
    }
    if (tent > GUARDA_MAX_BOOTS) {
        ESP_LOGE(TAG, "new image: %u boots without confirmation", (unsigned)tent);
        desistir_e_voltar(NULL);
        return;
    }
    const esp_timer_create_args_t ta = { .callback = desistir_e_voltar, .name = "carencia" };
    if (esp_timer_create(&ta, &s_carencia) == ESP_OK) {
        esp_timer_start_once(s_carencia, (uint64_t)PRAZO_HOST_MS * 1000);
        ESP_LOGW(TAG, "NEW image on trial (boot %u of %u): without the host's VIVO within %d s, "
                      "I go back to the previous one", (unsigned)tent, GUARDA_MAX_BOOTS + 1,
                 PRAZO_HOST_MS / 1000);
    }
}

static void host_falou(void)
{
    if (s_confirmado) { return; }
    s_confirmado = true;
    if (s_carencia) { esp_timer_stop(s_carencia); }
    if (s_pendente) {
        guarda_gravar(0, 0);
        s_pendente = false;
        ESP_LOGI(TAG, "host confirmed the link - new image becomes permanent");
    }
    /* No-NVS case: this is where the bootloader's rollback is cancelled. */
    esp_ota_mark_app_valid_cancel_rollback();
}

/* ---------------------------------------------------------------------------
 * ESP-NOW
 * --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * OUTPUT TO THE HOST - and it is NOT a luxury
 * ---------------------------------------------------------------------------
 * The radio callbacks (ESP-NOW, CSI, promiscuous) run on the Wi-Fi task and
 * must return quickly - it is the same rule as "no blocking I/O in ao_tocar"
 * on the P4 side. eh_cp_feat_peer_data_send talks to the host over RPC and can
 * BLOCK; called from inside the callback, it hangs the whole Wi-Fi stack, and
 * there is no worse symptom: the radio simply stops.
 *
 * So the callback only writes into the RING BUFFER and leaves; the c6saida
 * task is what talks to the host. A ring and not a fixed-item queue: messages
 * range from 7 B (an ESTADO) to ~1 KB (an HE CSI). With a queue, each item
 * would cost the largest size in RAM and the producer would build a copy that
 * size on ITS OWN stack - the Wi-Fi task's stack, which is not ours. In the
 * ring, the producer reserves the exact space (SendAcquire) and writes
 * straight into it: zero intermediate copies.
 *
 * Item = msg_id (4 B) + payload. Full = drop (the host is not consuming, and
 * blocking here would only make things worse; losing a record is better than
 * stopping the radio). */
#define SAIDA_RING_BYTES  (12 * 1024)

static RingbufHandle_t s_saida = NULL;
static volatile uint32_t s_saida_perdas = 0;

/* In TWO parts (header + payload), so whoever builds the message does not need
 * to join the two in a buffer of their own. Any task; NEVER from an ISR. */
static bool enfileirar2(uint32_t msg_id, const uint8_t *cab, size_t ncab,
                        const uint8_t *corpo, size_t ncorpo)
{
    if (!s_saida) { return false; }
    uint8_t *it = NULL;
    if (xRingbufferSendAcquire(s_saida, (void **)&it, 4 + ncab + ncorpo, 0) != pdTRUE || !it) {
        s_saida_perdas++;
        return false;
    }
    memcpy(it, &msg_id, 4);
    memcpy(it + 4, cab, ncab);
    if (ncorpo) { memcpy(it + 4 + ncab, corpo, ncorpo); }
    xRingbufferSendComplete(s_saida, it);
    return true;
}

static void enfileirar(uint32_t msg_id, const uint8_t *dados, size_t n)
{
    enfileirar2(msg_id, dados, n, NULL, 0);
}

static void vTarefaSaida(void *arg)
{
    (void)arg;
    for (;;) {
        size_t n = 0;
        uint8_t *it = (uint8_t *)xRingbufferReceive(s_saida, &n, portMAX_DELAY);
        if (!it) { continue; }
        if (n > 4) {
            uint32_t msg_id;
            memcpy(&msg_id, it, 4);
            eh_cp_feat_peer_data_send(msg_id, it + 4, n - 4);
        }
        vRingbufferReturnItem(s_saida, it);
    }
}

/* ---------------------------------------------------------------------------
 * TWO ESP-NOW DIALECTS
 * ---------------------------------------------------------------------------
 * ESP-NOW only carries bytes; what goes inside is each firmware's decision:
 *   - ESPRESSIF FORMAT: the espressif/esp-now component adds a header
 *     (version, type, length, anti-replay "magic", flags, addresses) and
 *     retransmits. It is the one in the official Cardputer chat
 *     (M5Cardputer-UserDemo).
 *   - PLAIN TEXT: esp_now_send() directly with the text. Arduino, UIFlow,
 *     simple examples - the StickC and the Basic, probably.
 * The Tab5 speaks BOTH. We do not use the library at run time (it drops
 * everything that is not its own - plain text would never arrive - and its
 * espnow_init aborts the chip if ESP-NOW fails underneath, the failure mode of
 * 09/2026). It only lends the TYPES (espnow.h), so the header is identical by
 * construction; the acceptance rules below are those of its espnow_recv_cb
 * (version 2.5.3, on-air format identical to the UserDemo's 2.5.1).
 *
 * RECEIVE: passes the Espressif format rules -> strip the header; otherwise
 * -> plain text. SEND: in the dialects heard in the last 10 min; with nobody
 * heard yet, in both (so we can be found). */

typedef struct {                       /* = espnow_data_t from espnow.c */
    uint8_t type    : 4;
    uint8_t version : 2;
    uint8_t         : 2;
    uint8_t size;
    espnow_frame_head_t frame_head;
    uint8_t dest_addr[6];
    uint8_t src_addr[6];
    uint8_t payload[0];
} __attribute__((packed)) pacote_espressif_t;

#define ESPRESSIF_VERSAO   2           /* the component's ESPNOW_VERSION */
#define ESPRESSIF_REPETE   3           /* copies with the same magic (the receiver discards the repeats) */
#define DIALETO_VALIDADE_US (10LL * 60 * 1000000)

static uint8_t  s_meu_mac[ESP_NOW_ETH_ALEN];
static uint16_t s_magicos[32];         /* last 'magic' values seen: discard retransmissions */
static int      s_magico_prox = 0, s_magicos_n = 0;
/* Two tasks touch the ring (the Wi-Fi one receiving, c6envio registering
 * ours): the lock is short, 32 comparisons. */
static portMUX_TYPE s_magico_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_ouviu_espressif_us = 0, s_ouviu_cru_us = 0;

static bool magico_visto(uint16_t m)
{
    bool visto = false;
    portENTER_CRITICAL(&s_magico_mux);
    /* Only FILLED entries: the ring starts zeroed, and a legitimate magic 0
     * (the library picks from 0 to 65535) would be discarded as a repeat. */
    for (int i = 0; i < s_magicos_n && !visto; ++i) { visto = (s_magicos[i] == m); }
    if (!visto) {
        s_magicos[s_magico_prox] = m;
        s_magico_prox = (s_magico_prox + 1) % 32;
        if (s_magicos_n < 32) { ++s_magicos_n; }
    }
    portEXIT_CRITICAL(&s_magico_mux);
    return visto;
}

/* A peer must be REGISTERED before any send, including broadcast. */
static esp_err_t garantir_par(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (esp_now_is_peer_exist(mac)) { return ESP_OK; }
    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.ifidx   = WIFI_IF_STA;
    p.channel = 0;
    p.encrypt = false;
    esp_err_t r = esp_now_add_peer(&p);
    if (r == ESP_ERR_ESPNOW_FULL) {
        /* TABLE FULL (ESP_NOW_MAX_TOTAL_PEER_NUM = 20). Each new unicast
         * destination takes a slot forever; on the 21st, every send to a new MAC
         * would fail until the C6 restarted. Drop the unicast peers (broadcast
         * stays) and try again. Collect before deleting: deleting mid-fetch
         * derails the driver's iteration. */
        uint8_t apagar[ESP_NOW_MAX_TOTAL_PEER_NUM][ESP_NOW_ETH_ALEN];
        int n = 0;
        esp_now_peer_info_t q;
        for (bool inicio = true; n < ESP_NOW_MAX_TOTAL_PEER_NUM
                                 && esp_now_fetch_peer(inicio, &q) == ESP_OK; inicio = false) {
            if (memcmp(q.peer_addr, DIFUSAO, ESP_NOW_ETH_ALEN) != 0) {
                memcpy(apagar[n++], q.peer_addr, ESP_NOW_ETH_ALEN);
            }
        }
        for (int i = 0; i < n; ++i) { esp_now_del_peer(apagar[i]); }
        r = esp_now_add_peer(&p);
    }
    return r;
}

/* In the Wi-Fi task's context: decides the dialect, only copies and leaves. */
static volatile bool s_repassar_cru = false;   /* C6_AJ_CRU */

static void ao_receber(const esp_now_recv_info_t *info, const uint8_t *dados, int n)
{
    if (!info || !dados || n <= 0) { return; }
    if (s_repassar_cru) {
        /* TRANSPARENT PIPE: the whole frame, uninterpreted, with the RSSI.
         * A new format from another device becomes the P4's problem only -
         * without reflashing the C6. */
        uint8_t cab[9];
        memcpy(cab, info->src_addr, ESP_NOW_ETH_ALEN);
        cab[6] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->rssi : 0;
        cab[7] = info->rx_ctrl ? (uint8_t)info->rx_ctrl->channel : 0;
        cab[8] = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;
        /* Up to 1470 B: ESP-NOW v2.0 (IDF 5.4+, new C6/S3) goes beyond 250. */
        enfileirar2(C6_MSG_RECEBIDO_CRU, cab, sizeof(cab), dados,
                    (size_t)(n > ESP_NOW_MAX_DATA_LEN_V2 ? ESP_NOW_MAX_DATA_LEN_V2 : n));
    }
    const uint8_t *texto = dados;
    int tam = n;
    const uint8_t *de = info->src_addr;
    uint8_t marca = (memcmp(info->des_addr, DIFUSAO, ESP_NOW_ETH_ALEN) == 0) ? 1 : 0;

    const pacote_espressif_t *p = (const pacote_espressif_t *)dados;
    if (n >= (int)sizeof(pacote_espressif_t) && p->version == ESPRESSIF_VERSAO
            && p->type < ESPNOW_DATA_TYPE_MAX
            && n == (int)(sizeof(pacote_espressif_t) + p->size)) {
        /* ESPRESSIF FORMAT. Only the DATA type is a message; the rest (ACK, control)
         * is their protocol. Encrypted cannot be read. A retransmission of the
         * same 'magic' already delivered is discarded. */
        if (p->type != ESPNOW_DATA_TYPE_DATA || p->frame_head.security) { return; }
        if (memcmp(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN) == 0) { return; }
        if (magico_visto(p->frame_head.magic)) { return; }
        texto = p->payload;
        tam = p->size;
        de = p->src_addr;   /* the author (may have been relayed by someone else) */
        marca |= 2;         /* bit 1: came in the Espressif format */
        s_ouviu_espressif_us = esp_timer_get_time();
    } else {
        s_ouviu_cru_us = esp_timer_get_time();
    }
    if (tam <= 0) { return; }

    uint8_t cab[7];
    memcpy(cab, de, ESP_NOW_ETH_ALEN);
    cab[6] = marca;
    const int c = tam > ESP_NOW_MAX_DATA_LEN ? ESP_NOW_MAX_DATA_LEN : tam;
    enfileirar2(MSG_C6_RECEBIDO, cab, sizeof(cab), texto, (size_t)c);
}

static void ao_enviar(const esp_now_send_info_t *info, esp_now_send_status_t st)
{
    (void)info; (void)st;   /* the status goes via esp_now_send's return value (see vTarefaEnvio) */
}

/* SENDING IN ITS OWN TASK: the Espressif format copies have a wait between
 * them, and the request arrives in the peer_data callback, which cannot wait. */
typedef struct {
    uint8_t  mac[ESP_NOW_ETH_ALEN];
    uint16_t n;
    bool     cru;              /* C6_MSG_HOST_CRU: once, no header, no ESTADO */
    /* Sized for v2.0 (1470): CRU can send a large frame to those that speak
     * v2.0; text (two dialects) is still cut at 250. */
    uint8_t  texto[ESP_NOW_MAX_DATA_LEN_V2];
} envio_t;
static QueueHandle_t s_envio = NULL;

static void vTarefaEnvio(void *arg)
{
    (void)arg;
    static envio_t e;
    static uint8_t quadro[ESP_NOW_MAX_DATA_LEN];
    for (;;) {
        if (xQueueReceive(s_envio, &e, portMAX_DELAY) != pdTRUE) { continue; }
        bool ok = (garantir_par(e.mac) == ESP_OK);
        if (e.cru) {
            if (ok) { esp_now_send(e.mac, e.texto, e.n); }
            continue;
        }

        const int64_t agora = esp_timer_get_time();
        bool espressif = s_ouviu_espressif_us && (agora - s_ouviu_espressif_us) < DIALETO_VALIDADE_US;
        bool cru       = s_ouviu_cru_us       && (agora - s_ouviu_cru_us)       < DIALETO_VALIDADE_US;
        if (!espressif && !cru) { espressif = cru = true; }   /* nobody heard: both */

        bool algum = false;
        if (ok && cru) {
            algum |= (esp_now_send(e.mac, e.texto, e.n) == ESP_OK);
        }
        if (ok && espressif) {
            /* Header + text within the SAME 250-byte limit (ESP-NOW v1.0, what the
             * ESP32/S3 speak). */
            const size_t cab = sizeof(pacote_espressif_t);
            size_t t = e.n;
            if (cab + t > ESP_NOW_MAX_DATA_LEN) { t = ESP_NOW_MAX_DATA_LEN - cab; }
            memset(quadro, 0, cab);
            pacote_espressif_t *p = (pacote_espressif_t *)quadro;
            p->version = ESPRESSIF_VERSAO;
            p->type = ESPNOW_DATA_TYPE_DATA;
            p->size = (uint8_t)t;
            uint16_t m = (uint16_t)esp_random();
            if (m == 0) { m = 1; }
            p->frame_head.magic = m;
            uint8_t canal = 0; wifi_second_chan_t sec;
            esp_wifi_get_channel(&canal, &sec);
            p->frame_head.channel = canal;
            p->frame_head.broadcast = (memcmp(e.mac, DIFUSAO, ESP_NOW_ETH_ALEN) == 0);
            p->frame_head.retransmit_count = ESPRESSIF_REPETE;
            memcpy(p->dest_addr, e.mac, ESP_NOW_ETH_ALEN);
            memcpy(p->src_addr, s_meu_mac, ESP_NOW_ETH_ALEN);
            memcpy(p->payload, e.texto, t);
            magico_visto(m);   /* our own relayed echo does not come back as a message */
            for (int k = 0; k < ESPRESSIF_REPETE; ++k) {
                algum |= (esp_now_send(e.mac, quadro, cab + t) == ESP_OK);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        uint8_t st[ESP_NOW_ETH_ALEN + 1];
        memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
        st[6] = algum ? 1 : 0;
        enfileirar(MSG_C6_ESTADO, st, sizeof(st));
    }
}

/* STARTING ESP-NOW IS THE HOST'S DECISION (it asks once Wi-Fi is up), and
 * NOTHING here aborts: if it fails, give up quietly - the host asks again in 5 s. */
static bool s_espnow_no_ar = false;
/* The window requested by the P4 (C6_AJ_ESPNOW_JANELA); reapplied if ESP-NOW comes up again. */
static uint16_t s_en_intervalo = 0, s_en_janela = 0xFFFF;
static void espnow_aplicar_janela(void)
{
    if (!s_espnow_no_ar) { return; }
    if (s_en_intervalo) { esp_wifi_connectionless_module_set_wake_interval(s_en_intervalo); }
    esp_now_set_wake_window(s_en_janela);
}

static void ligar_espnow(void)
{
    /* ESP-NOW survives esp_wifi_stop/start, but NOT an esp_wifi_deinit:
     * esp_hosted's handler does deinit+init when the host sends a different
     * init config, and the P4 may turn Wi-Fi off at level 2 (deinit). Without
     * this check, s_espnow_no_ar would stay true with ESP-NOW dead underneath,
     * and every send/receive would vanish silently until the C6 restarted. */
    uint32_t ver;
    if (s_espnow_no_ar && esp_now_get_version(&ver) != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW dropped underneath (Wi-Fi restarted) - starting it again");
        s_espnow_no_ar = false;
    }
    if (!s_espnow_no_ar) {
        const esp_err_t r = esp_now_init();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_init: %s - is Wi-Fi really up?", esp_err_to_name(r));
            return;
        }
        esp_now_register_recv_cb(ao_receber);
        esp_now_register_send_cb(ao_enviar);
        garantir_par(DIFUSAO);
        esp_wifi_get_mac(WIFI_IF_STA, s_meu_mac);
        if (!s_envio) {
            s_envio = xQueueCreate(4, sizeof(envio_t));
            if (s_envio) { xTaskCreate(vTarefaEnvio, "c6envio", 4096, NULL, 4, NULL); }
        }
        s_espnow_no_ar = true;
        espnow_aplicar_janela();
    }
    /* PRONTO always (the host repeats the request until it sees this message). */
    uint8_t canal = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    uint8_t msg[ESP_NOW_ETH_ALEN + 3];
    memcpy(msg, s_meu_mac, ESP_NOW_ETH_ALEN);
    msg[6] = canal;
    msg[7] = C6_PROTO_VERSAO;
    msg[8] = caps_reais();
    enfileirar(MSG_C6_PRONTO, msg, sizeof(msg));
    ESP_LOGI(TAG, "ESP-NOW up (Espressif format + plain text): channel %u", (unsigned)canal);
}

/* ---------------------------------------------------------------------------
 * SNIFFER - promiscuous mode, summarised on the C6
 * ---------------------------------------------------------------------------
 * The contract (format, validity, channels) is in tab5_c6.h. Here, the how:
 *
 * WHO TOUCHES WHAT. The promiscuous callback runs on the Wi-Fi task, on every
 * frame - a thousand or more per second on a busy channel. It only finds the
 * MAC in the table and adds up; no allocating, logging or talking to the host.
 * Starting, stopping, hopping channel and sending the batch all belong to the
 * c6farejo task, the ONLY one that calls esp_wifi_set_promiscuous*. The host's
 * request (which arrives on the RPC task) only leaves the new configuration
 * and wakes c6farejo.
 *
 * TWO TABLES. The callback writes into the active one; when the batch closes,
 * c6farejo swaps the active one (one line inside the lock) and reads the other
 * with no lock at all, at leisure, while frames keep landing in the new one.
 *
 * Table hashed by MAC with linear probing: 128 slots for at most 96 MACs per
 * period (3/4 full, the search stops after a few comparisons). Beyond 96, it
 * counts as lost - the P4 sees it in the batch and can shorten the period.
 */
#define FJ_VAGAS   128
#define FJ_MAX      96

typedef struct {
    uint8_t mac[6];
    int8_t  ult, max;
    int16_t soma;       /* 255 x -128 fits */
    uint8_t n, tipos, canal, usado;
} fj_ent_t;             /* 16 B */

typedef struct {
    fj_ent_t e[FJ_VAGAS];
    uint16_t usados, perdidos;
    uint32_t quadros;
} fj_tab_t;

static fj_tab_t s_fj[2];
static volatile uint8_t s_fj_ativa = 0;
static portMUX_TYPE s_fj_mux = portMUX_INITIALIZER_UNLOCKED;

/* Read by the callback on every frame: plain copies of the s_cfg in force. */
static volatile int8_t s_fj_rssi_min = -128;
static volatile bool   s_fj_tem_alvo = false;
static uint8_t         s_fj_alvo[6];
static volatile uint32_t s_fj_quadros_total = 0;

/* Host request -> c6farejo (under s_fj_mux). */
static c6_farejo_cfg_t s_fj_pedido;
static bool            s_fj_tem_pedido = false;
static int64_t         s_fj_validade_us = 0;

static volatile bool s_farejando = false;
static TaskHandle_t  s_fj_tarefa = NULL;
/* Where the radio returns when sniffing stops: the channel from when it
 * started, or the one the host asked for AFTERWARDS (C6_MSG_HOST_CANAL during
 * sniffing). Without this, stopping the sniffer undid the ESP-NOW "fixed
 * channel" requested midway. */
static volatile uint8_t s_canal_volta = 0;

static void radio_guarda(int64_t agora);   /* radio extras, further down */

static void ao_farejar(void *buf, wifi_promiscuous_pkt_type_t tipo)
{
    const wifi_promiscuous_pkt_t *pk = (const wifi_promiscuous_pkt_t *)buf;
    if (!pk || pk->rx_ctrl.sig_len < 16) { return; }   /* no addr2 */
    const int8_t rssi = (int8_t)pk->rx_ctrl.rssi;
    if (rssi < s_fj_rssi_min) { return; }

    const uint8_t *q = pk->payload;
    const uint8_t sub = q[0] >> 4;
    uint8_t t;
    if (tipo == WIFI_PKT_MGMT) {
        t = (sub == 8 || sub == 5) ? C6_FT_BEACON
          : (sub == 4)             ? C6_FT_PROCURA
          :                          C6_FT_GERENCIA;
    } else if (tipo == WIFI_PKT_DATA) {
        const uint8_t ds = q[1] & 0x03;
        t = C6_FT_DADOS | (ds == 1 ? C6_FT_PARA_AP : ds == 2 ? C6_FT_DO_AP : 0);
    } else {
        return;
    }

    const uint8_t *ta = q + 10;   /* addr2 = transmitter, in management and data */
    if (s_fj_tem_alvo && memcmp(ta, s_fj_alvo, 6) != 0) { return; }
    const uint8_t canal = (uint8_t)pk->rx_ctrl.channel;

    /* The last 3 bytes vary between devices; the first 3 are the manufacturer's. */
    unsigned h = ((unsigned)ta[5] * 31u + ta[4] * 7u + ta[3]) & (FJ_VAGAS - 1);

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    tb->quadros++;
    for (int k = 0; k < FJ_VAGAS; ++k, h = (h + 1) & (FJ_VAGAS - 1)) {
        fj_ent_t *e = &tb->e[h];
        if (!e->usado) {
            if (tb->usados >= FJ_MAX) { tb->perdidos++; break; }
            memcpy(e->mac, ta, 6);
            e->usado = 1;
            e->ult = e->max = rssi;
            e->soma = rssi;
            e->n = 1;
            e->tipos = t;
            e->canal = canal;
            tb->usados++;
            break;
        }
        if (memcmp(e->mac, ta, 6) == 0) {
            e->ult = rssi;
            if (rssi > e->max) { e->max = rssi; }
            if (e->n < 255) { e->soma += rssi; e->n++; }
            e->tipos |= t;
            e->canal = canal;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fj_mux);
    s_fj_quadros_total++;
}

/* Swaps the active table and sends what the old one gathered, in chunks of LOTE_MAX. */
static void fechar_lote(void)
{
    static uint8_t msg[sizeof(c6_farejo_lote_t) + C6_FAREJO_LOTE_MAX * sizeof(c6_farejo_reg_t)];

    portENTER_CRITICAL(&s_fj_mux);
    fj_tab_t *tb = &s_fj[s_fj_ativa];
    s_fj_ativa ^= 1;
    portEXIT_CRITICAL(&s_fj_mux);

    if (tb->usados) {
        uint8_t canal = 0; wifi_second_chan_t sec;
        esp_wifi_get_channel(&canal, &sec);
        c6_farejo_lote_t *cab = (c6_farejo_lote_t *)msg;
        c6_farejo_reg_t *r = (c6_farejo_reg_t *)(msg + sizeof(*cab));
        int n = 0;
        for (int i = 0; i < FJ_VAGAS; ++i) {
            const fj_ent_t *e = &tb->e[i];
            if (!e->usado) { continue; }
            memcpy(r[n].mac, e->mac, 6);
            r[n].rssi_ult = e->ult;
            r[n].rssi_max = e->max;
            r[n].rssi_med = (int8_t)(e->soma / (int)e->n);
            r[n].n = e->n;
            r[n].tipos = e->tipos;
            r[n].canal = e->canal;
            if (++n == C6_FAREJO_LOTE_MAX) {
                cab->canal = canal; cab->n = (uint8_t)n;
                cab->perdidos = tb->perdidos;
                cab->quadros = tb->quadros > 0xFFFF ? 0xFFFF : (uint16_t)tb->quadros;
                eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
                n = 0;
            }
        }
        if (n) {
            cab->canal = canal; cab->n = (uint8_t)n;
            cab->perdidos = tb->perdidos;
            cab->quadros = tb->quadros > 0xFFFF ? 0xFFFF : (uint16_t)tb->quadros;
            eh_cp_feat_peer_data_send(C6_MSG_FAREJO, msg, sizeof(*cab) + n * sizeof(*r));
        }
    }
    /* Nothing heard: nothing sent. Silence on SDIO means the P4 sleeping. */
    memset(tb, 0, sizeof(*tb));
}

static bool associado(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static void vTarefaFarejo(void *arg)
{
    (void)arg;
    c6_farejo_cfg_t cfg = { 0 };   /* the RAW request in force: only for comparison */
    uint16_t lote_ms = 250, perm_ms = 200, mascara = 0;
    bool saltou = false;
    int64_t prox_lote = 0, prox_salto = 0;
    int canal_idx = 0;

    for (;;) {
        /* 1. New request from the host? */
        c6_farejo_cfg_t novo;
        bool tem = false;
        int64_t validade;
        portENTER_CRITICAL(&s_fj_mux);
        if (s_fj_tem_pedido) { novo = s_fj_pedido; s_fj_tem_pedido = false; tem = true; }
        validade = s_fj_validade_us;
        portEXIT_CRITICAL(&s_fj_mux);

        const int64_t agora = esp_timer_get_time();
        /* This task is also the one that turns off CSI/802.15.4 abandoned by the host:
         * the watchdog's esp_timer only wakes it (see radio_guarda_cb). */
        radio_guarda(agora);
        bool parar = s_farejando && agora > validade;
        if (parar) { ESP_LOGW(TAG, "sniffer: the host did not renew - turning off"); }

        if (tem && !novo.liga) { parar = s_farejando; tem = false; }

        if (parar) {
            esp_wifi_set_promiscuous(false);
            s_farejando = false;
            if (saltou && s_canal_volta && !associado()) {
                esp_wifi_set_channel(s_canal_volta, WIFI_SECOND_CHAN_NONE);
            }
            saltou = false;
            portENTER_CRITICAL(&s_fj_mux);
            memset(s_fj, 0, sizeof(s_fj));
            portEXIT_CRITICAL(&s_fj_mux);
            ESP_LOGI(TAG, "sniffer off");
        }

        if (tem) {
            const bool mudou = !s_farejando || memcmp(&novo, &cfg, sizeof(cfg)) != 0;
            if (mudou) {
                cfg = novo;
                lote_ms = cfg.lote_ms ? cfg.lote_ms : 250;
                if (lote_ms < 50) { lote_ms = 50; }
                perm_ms = cfg.permanencia_ms ? cfg.permanencia_ms : 200;
                if (perm_ms < 50) { perm_ms = 50; }
                mascara = cfg.canais & 0x3FFE;   /* channels 1..13 */
                s_fj_rssi_min = cfg.rssi_min ? cfg.rssi_min : -128;
                static const uint8_t zero[6] = { 0 };
                memcpy(s_fj_alvo, cfg.alvo, 6);
                s_fj_tem_alvo = memcmp(cfg.alvo, zero, 6) != 0;

                const uint8_t f = cfg.filtro ? cfg.filtro : (C6_FJ_MGMT | C6_FJ_DADOS);
                wifi_promiscuous_filter_t filtro = { .filter_mask =
                    ((f & C6_FJ_MGMT)  ? WIFI_PROMIS_FILTER_MASK_MGMT : 0) |
                    ((f & C6_FJ_DADOS) ? WIFI_PROMIS_FILTER_MASK_DATA : 0) };
                esp_wifi_set_promiscuous_filter(&filtro);
            }
            if (!s_farejando) {
                wifi_second_chan_t sec;
                uint8_t canal_antes = 0;
                esp_wifi_get_channel(&canal_antes, &sec);
                s_canal_volta = canal_antes;
                esp_wifi_set_promiscuous_rx_cb(ao_farejar);
                const esp_err_t r = esp_wifi_set_promiscuous(true);
                if (r != ESP_OK) {
                    /* Wi-Fi stopped, for example. The host renews within seconds
                     * and we try again - no aborting. */
                    ESP_LOGW(TAG, "sniffer: promiscuous mode refused (%s)", esp_err_to_name(r));
                } else {
                    s_farejando = true;
                    prox_lote = agora + (int64_t)lote_ms * 1000;
                    prox_salto = agora;
                    canal_idx = 0;
                    ESP_LOGI(TAG, "sniffer on: channel %u, mask 0x%04x, batch %u ms",
                             (unsigned)s_canal_volta, (unsigned)mascara, (unsigned)lote_ms);
                }
            } else {
                /* Renewal: Wi-Fi may have been stopped and restarted in between,
                 * and esp_wifi_stop turns promiscuous mode off underneath. */
                bool ligado = false;
                if (esp_wifi_get_promiscuous(&ligado) == ESP_OK && !ligado) {
                    esp_wifi_set_promiscuous_rx_cb(ao_farejar);
                    esp_wifi_set_promiscuous(true);
                }
            }
        }

        if (!s_farejando) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* stopped: zero CPU */
            continue;
        }

        /* 2. Channel hopping - only when detached from the router. */
        const bool um_so = mascara && !(mascara & (mascara - 1));
        if (mascara && agora >= prox_salto) {
            if (!associado()) {
                int c = 0;
                for (int k = 0; k < 14 && !c; ++k) {
                    canal_idx = canal_idx % 13 + 1;
                    if (mascara & (1u << canal_idx)) { c = canal_idx; }
                }
                uint8_t atual = 0; wifi_second_chan_t sec;
                esp_wifi_get_channel(&atual, &sec);
                if (c && c != atual) {
                    esp_wifi_set_channel((uint8_t)c, WIFI_SECOND_CHAN_NONE);
                    saltou = true;
                }
            }
            /* A single channel: check every second (someone may have changed
             * it); several: the pace is the dwell time. */
            prox_salto = agora + (int64_t)(um_so ? 1000 : perm_ms) * 1000;
        }

        /* 3. Batch. */
        if (agora >= prox_lote) {
            fechar_lote();
            prox_lote += (int64_t)lote_ms * 1000;
            if (prox_lote <= agora) { prox_lote = agora + (int64_t)lote_ms * 1000; }
        }

        /* 4. Sleep until the next event - or until a request from the host. */
        int64_t prox = prox_lote;
        if (mascara && prox_salto < prox) { prox = prox_salto; }
        if (validade < prox) { prox = validade + 1000; }
        int64_t esp = (prox - esp_timer_get_time()) / 1000;
        if (esp < 1) { esp = 1; }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(esp));
    }
}

/* Called on the RPC task: only records it and wakes c6farejo. */
static void pedir_farejo(const uint8_t *dados, size_t n)
{
    c6_farejo_cfg_t c = { 0 };
    memcpy(&c, dados, n < sizeof(c) ? n : sizeof(c));
    portENTER_CRITICAL(&s_fj_mux);
    s_fj_pedido = c;
    s_fj_tem_pedido = true;
    s_fj_validade_us = esp_timer_get_time() + (int64_t)C6_FAREJO_VALIDADE_MS * 1000;
    portEXIT_CRITICAL(&s_fj_mux);
    if (s_fj_tarefa) { xTaskNotifyGive(s_fj_tarefa); }
}

/* ===========================================================================
 * THE HOST SLEEPS, SO DOES THE C6 (firmware c6-5, contract v3)
 * ===========================================================================
 * With the Tab5's screen off, connected Wi-Fi cost ~30 mA of battery: SDIO is
 * the P4's slave, and with the link up this chip cannot enter light sleep (the
 * host would be talking to itself). esp_hosted already has this side's half of
 * "host power save": the P4 sends POWER_SAVE_ON via the SDIO interrupt,
 * esp_hosted tears down the slave and, when traffic for the host arrives,
 * rebuilds it and pulses IO2 (wire to the P4's G14) until the host comes back.
 * Espressif measures ~1 mA this way, associated, at MAX_MODEM listen 10.
 *
 * What is OURS here:
 *   - THE LOCK. PM enables light sleep (app_main), but a NO_LIGHT_SLEEP lock
 *     stays held while the link is up. It only drops once SDIO has been torn
 *     down (on_ready) and comes back before rebuilding (off_prepare).
 *   - WAKING BY THE P4. There is no wire from the P4 to here. With the slave
 *     torn down, the SDIO CMD line (GPIO18) sits high on the pull-up; the P4
 *     pulls it low for 2 ms when it wants the link back (screen on, MQTT
 *     keepalive), and the low level wakes this chip. The line is only armed
 *     SONO_ARMAR_MS after tearing down: the P4 itself still sends commands over
 *     it while tearing down its side, and they would wake this chip at once.
 *   - THE COUNTERS, which go in INFO: the P4 cannot see the serial here.
 *
 * Unless the P4 asks (POWER_SAVE_ON), none of this happens: the lock stays
 * held and the behaviour is as before. */
#if CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS
#include "eh_cp_feat_host_ps.h"
#include "eh_cp.h"   /* eh_cp_rx_get / eh_cp_rx_register: the STA filter */
#include "eh_cp_feat_host_ps_apis.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_sleep.h"
#include "soc/sdio_slave_pins.h"

#define SONO_CMD_GPIO   SDIO_SLAVE_SLOT0_IOMUX_PIN_NUM_CMD   /* 18 */
#define SONO_ARMAR_MS   500
#define SONO_ACORDAR_MS 3000   /* how long the IO2 pulse insists until the P4 responds */

static esp_pm_lock_handle_t s_sono_trava = NULL;
static volatile bool        s_sono_trava_solta = false;
static volatile uint8_t     s_sono_estado = C6_SONO_SEM;
static volatile uint32_t    s_sono_n = 0;
static volatile uint64_t    s_sono_us = 0;
static volatile uint16_t    s_sono_entradas = 0, s_sono_despertares = 0;
static TaskHandle_t         s_sono_tarefa = NULL;
static esp_timer_handle_t   s_sono_armar = NULL;
static volatile bool        s_sono_cmd_armado = false;
static volatile bool        s_sono_cmd_disparou = false;   /* the CMD ISR, not an ARP */

static esp_err_t IRAM_ATTR sono_saiu(int64_t dormiu_us, void *arg)
{
    (void)arg;
    s_sono_n = s_sono_n + 1;
    s_sono_us = s_sono_us + (uint64_t)dormiu_us;
    return ESP_OK;
}

/* Low level on CMD: the P4 wants the link. Only once (the level would repeat). */
static void IRAM_ATTR sono_cmd_isr(void *arg)
{
    (void)arg;
    gpio_ll_intr_disable(&GPIO, SONO_CMD_GPIO);
    s_sono_cmd_armado = false;
    s_sono_cmd_disparou = true;
    BaseType_t acordou = pdFALSE;
    if (s_sono_tarefa) { vTaskNotifyGiveFromISR(s_sono_tarefa, &acordou); }
    portYIELD_FROM_ISR(acordou);
}

static void sono_cmd_desarmar(void)
{
    s_sono_cmd_armado = false;
    gpio_wakeup_disable(SONO_CMD_GPIO);
    gpio_isr_handler_remove(SONO_CMD_GPIO);
    gpio_set_intr_type(SONO_CMD_GPIO, GPIO_INTR_DISABLE);
    /* The pin function (SDIO slave IO_MUX) comes back in the slave's init. */
}

/* On the esp_timer, SONO_ARMAR_MS after the link drops. */
static void sono_cmd_armar(void *arg)
{
    (void)arg;
    if (s_sono_estado != C6_SONO_DORMINDO || s_sono_cmd_armado) { return; }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << SONO_CMD_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    if (gpio_config(&io) != ESP_OK) { return; }
    gpio_isr_handler_add(SONO_CMD_GPIO, sono_cmd_isr, NULL);
    gpio_wakeup_enable(SONO_CMD_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    s_sono_cmd_armado = true;
}

/* LOCK SNAPSHOT WITH THE HOST ASLEEP: the P4 does not talk to the C6 at that
 * time, so the snapshot is kept and goes along with the next reply to C6_AJ_PM. */
static char               s_pm_dormindo[1024] = "";
static esp_timer_handle_t s_pm_retrato = NULL;
static void pm_retratar(void *arg)
{
    (void)arg;
    FILE *f = fmemopen(s_pm_dormindo, sizeof(s_pm_dormindo) - 1, "w");
    if (f) { esp_pm_dump_locks(f); fclose(f); s_pm_dormindo[sizeof(s_pm_dormindo) - 1] = 0; }
}

/* The host has told us and esp_hosted has already torn down the slave: we can sleep from here. */
static void sono_host_dormiu(void)
{
    s_sono_estado = C6_SONO_DORMINDO;
    s_sono_entradas = s_sono_entradas + 1;
    if (s_sono_trava && !s_sono_trava_solta) { s_sono_trava_solta = true; esp_pm_lock_release(s_sono_trava); }
    if (s_sono_armar) { esp_timer_stop(s_sono_armar); esp_timer_start_once(s_sono_armar, SONO_ARMAR_MS * 1000ULL); }
    if (s_pm_retrato) { esp_timer_stop(s_pm_retrato); esp_timer_start_once(s_pm_retrato, 5000000ULL); }
}

/* The link is about to come back (via the P4 or via traffic for it): wake
 * before rebuilding the slave. It may come twice per wake-up (esp_hosted calls
 * it on the pulse path and again when the host confirms). */
static void sono_host_vai_acordar(void)
{
    if (s_sono_armar) { esp_timer_stop(s_sono_armar); }
    sono_cmd_desarmar();
    if (s_sono_trava && s_sono_trava_solta) { esp_pm_lock_acquire(s_sono_trava); s_sono_trava_solta = false; }
    if (s_sono_estado == C6_SONO_DORMINDO) { s_sono_despertares = s_sono_despertares + 1; }
    s_sono_estado = C6_SONO_ACORDADO;
}

static void responder_arps(void);   /* the filter, just below */

static void vTarefaSono(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        responder_arps();
        if (s_sono_estado != C6_SONO_DORMINDO || s_sono_cmd_armado || !s_sono_cmd_disparou) { continue; }
        s_sono_cmd_disparou = false;
        ESP_LOGI(TAG, "sleep: the P4 pulled CMD low - rebuilding the link");
        eh_cp_feat_host_ps_wakeup_host(SONO_ACORDAR_MS);
    }
}

/* THE FILTER WITH THE HOST ASLEEP. Without it, sleep did not last: the router
 * sends ~2 ARPs per second (measured: 131 in 59 s) and each frame for the P4
 * woke it and rebuilt the link. With the host asleep, this callback (in place
 * of esp_hosted's STA RX) handles here what does not need the host:
 *   - ARP asking for the P4's IP (C6_AJ_IP): the C6 replies;
 *   - any other ARP, and all broadcast/multicast: drop;
 *   - the rest (unicast for the P4: MQTT, TCP) goes through - and wakes the host.
 * With the host awake, everything goes straight through, as before. */
static eh_cp_rx_cb_t        s_rx_padrao = NULL;
static volatile uint32_t    s_ip_host = 0;           /* network order */
static uint8_t              s_mac_sta[6];
static volatile uint32_t    s_filt_arp = 0, s_filt_desc = 0, s_filt_acordou = 0;
static QueueHandle_t        s_arp_fila = NULL;
typedef struct { uint8_t mac[6]; uint8_t ip[4]; } arp_ped_t;

static void rx_soltar(void *buffer, void *eb)
{
    if (eb) { esp_wifi_internal_free_rx_buffer(eb); } else { free(buffer); }
}

static esp_err_t rx_filtro(void *buffer, uint16_t len, void *eb)
{
    if (s_sono_estado == C6_SONO_DORMINDO && buffer && len >= 14) {
        const uint8_t *b = (const uint8_t *)buffer;
        const uint16_t tipo = (uint16_t)((b[12] << 8) | b[13]);
        if (tipo == 0x0806) {
            const uint32_t alvo = len >= 42 ? ((uint32_t)b[38] | (uint32_t)b[39] << 8 | (uint32_t)b[40] << 16 | (uint32_t)b[41] << 24) : 0;
            if (len >= 42 && b[20] == 0 && b[21] == 1 && s_ip_host && alvo == s_ip_host && s_arp_fila) {
                arp_ped_t p;
                memcpy(p.mac, b + 22, 6);
                memcpy(p.ip, b + 28, 4);
                if (xQueueSend(s_arp_fila, &p, 0) == pdTRUE && s_sono_tarefa) { xTaskNotifyGive(s_sono_tarefa); }
                s_filt_arp = s_filt_arp + 1;
            } else {
                s_filt_desc = s_filt_desc + 1;
            }
            rx_soltar(buffer, eb);
            return ESP_OK;
        }
        if (b[0] & 1) {   /* group destination: broadcast or multicast */
            s_filt_desc = s_filt_desc + 1;
            rx_soltar(buffer, eb);
            return ESP_OK;
        }
        s_filt_acordou = s_filt_acordou + 1;
    }
    return s_rx_padrao ? s_rx_padrao(buffer, len, eb) : (rx_soltar(buffer, eb), ESP_OK);
}

/* On c6sono (not in the Wi-Fi callback): the ARP reply for the P4's IP. */
static void responder_arps(void)
{
    arp_ped_t p;
    while (s_arp_fila && xQueueReceive(s_arp_fila, &p, 0) == pdTRUE) {
        uint8_t q[42];
        memcpy(q, p.mac, 6); memcpy(q + 6, s_mac_sta, 6);
        q[12] = 0x08; q[13] = 0x06;
        q[14] = 0x00; q[15] = 0x01; q[16] = 0x08; q[17] = 0x00; q[18] = 6; q[19] = 4;
        q[20] = 0x00; q[21] = 0x02;                       /* reply */
        memcpy(q + 22, s_mac_sta, 6);
        const uint32_t ip = s_ip_host;
        memcpy(q + 28, &ip, 4);
        memcpy(q + 32, p.mac, 6); memcpy(q + 38, p.ip, 4);
        esp_wifi_internal_tx(WIFI_IF_STA, q, sizeof(q));
    }
}

/* Before esp_pm_configure: the lock already held when light sleep is enabled. */
static void sono_trava_criar(void)
{
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "enlace_p4", &s_sono_trava) == ESP_OK) {
        esp_pm_lock_acquire(s_sono_trava);
    } else {
        s_sono_trava = NULL;
    }
}

/* After the features (host_ps comes up before peer_data). */
static void sono_iniciar(void)
{
    gpio_install_isr_service(0);   /* already installed: ESP_ERR_INVALID_STATE, that is fine */
    const esp_timer_create_args_t ta = { .callback = sono_cmd_armar, .name = "sono_cmd" };
    esp_timer_create(&ta, &s_sono_armar);
    const esp_timer_create_args_t tr = { .callback = pm_retratar, .name = "sono_pm" };
    esp_timer_create(&tr, &s_pm_retrato);
    xTaskCreate(vTarefaSono, "c6sono", 3072, NULL, 5, &s_sono_tarefa);
    esp_pm_sleep_cbs_register_config_t cb = { .exit_cb = sono_saiu };
    esp_pm_light_sleep_register_cbs(&cb);
    static const host_power_save_callbacks_t cbs = {
        .host_power_save_on_ready_cb    = sono_host_dormiu,
        .host_power_save_off_prepare_cb = sono_host_vai_acordar,
    };
    eh_cp_feat_host_ps_cp__enable(&cbs);
    s_arp_fila = xQueueCreate(4, sizeof(arp_ped_t));
    esp_wifi_get_mac(WIFI_IF_STA, s_mac_sta);
    s_rx_padrao = eh_cp_rx_get(ESP_STA_IF);
    if (s_rx_padrao) { eh_cp_rx_register(ESP_STA_IF, rx_filtro); }
    s_sono_estado = C6_SONO_ACORDADO;
    ESP_LOGI(TAG, "sleep: ready (CMD = GPIO%d, host wake-up = IO%d)", SONO_CMD_GPIO,
             CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_HOST_WAKEUP_GPIO);
}
#else
#define s_sono_estado      C6_SONO_SEM
#define s_sono_n           0
#define s_sono_us          0
#define s_sono_entradas    0
#define s_sono_despertares 0
#define s_filt_arp         0
#define s_filt_desc        0
#define s_filt_acordou     0
static void sono_trava_criar(void) {}
static void sono_iniciar(void) {}
#endif

/* WHAT THE P4 SENDS TO THE NETWORK: all the host's data output goes through
 * esp_wifi_internal_tx (esp_hosted calls it, eh_cp_core.c). The link-time
 * wrapper (c6/main/CMakeLists.txt) only counts - so INFO can show whether the
 * frame reached Wi-Fi from here and with what error. */
static volatile uint32_t s_tx_wifi_n = 0, s_tx_wifi_erros = 0;
static volatile int16_t  s_tx_wifi_ultimo = 0;
esp_err_t __real_esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
esp_err_t __wrap_esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len)
{
    const esp_err_t r = __real_esp_wifi_internal_tx(ifx, buffer, len);
    s_tx_wifi_n = s_tx_wifi_n + 1;
    if (r != ESP_OK) { s_tx_wifi_erros = s_tx_wifi_erros + 1; s_tx_wifi_ultimo = (int16_t)r; }
    return r;
}

static void responder_info(void)
{
    c6_info_t i = { 0 };
    uint8_t canal = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    i.proto = C6_PROTO_VERSAO;
    i.caps = caps_reais();
    i.chip_rev = chip_rev_info();
    i.canal = canal;
    i.farejando = s_farejando ? 1 : 0;
    i.heap_livre = esp_get_free_heap_size();
    i.heap_min = esp_get_minimum_free_heap_size();
    i.no_ar_s = (uint32_t)(esp_timer_get_time() / 1000000);
    i.quadros = s_fj_quadros_total;
    i.csi_err = s_csi_err;
    i.z154_err = s_z154_err;
    i.csi_cb = s_csi_cb;
    i.csi_env = s_csi_env;
    i.z154_rx = s_z154_rx;
    i.csi_err2 = s_csi_err2;
    i.csi_etapa = s_csi_etapa;
    i.csi_modo = s_csi_modo;
    i.saida_perdas = s_saida_perdas;
    i.flags = (esp_core_dump_image_check() == ESP_OK ? C6_INFO_F_COREDUMP : 0)
            | (s_pendente ? C6_INFO_F_EM_PROVA : 0);
#if CONFIG_IEEE802154_ENABLED
    i.z154_estado = s_z154_lig ? 1 : (s_z154_habilitado ? 2 : 0);
#endif
    i.sono_estado = s_sono_estado;
    i.sono_n = s_sono_n;
    i.sono_ms = (uint32_t)(s_sono_us / 1000);
    i.sono_entradas = s_sono_entradas;
    i.sono_despertares = s_sono_despertares;
    i.tx_wifi_n = s_tx_wifi_n;
    i.tx_wifi_erros = s_tx_wifi_erros;
    i.tx_wifi_ultimo_err = s_tx_wifi_ultimo;
    i.sta_conectada = eh_cp_feat_wifi_is_station_connected() ? 1 : 0;
    i.filtro_arp = s_filt_arp;
    i.filtro_descartes = s_filt_desc;
    i.filtro_acordou = s_filt_acordou;
    enfileirar(C6_MSG_INFO, (const uint8_t *)&i, sizeof(i));
}

/* ===========================================================================
 * RADIO EXTRAS - CSI, FTM, 802.15.4
 * ===========================================================================
 * Each one sleeps until the P4 asks (C6_MSG_HOST_RADIO), and each CAPS bit only
 * lights if the module was compiled in - firmware flashed without the flag in
 * sdkconfig lets the P4 hide the feature, without reopening the device.
 *
 * Same discipline as the sniffer: the raw forwarding leaves the callback
 * (radio task) only through the output queue, never blocks; CSI and 802.15.4
 * turn themselves off if the P4 does not renew within C6_RADIO_VALIDADE_MS;
 * FTM is one-shot.
 */

/* ---- CSI ---- */
#if CONFIG_ESP_WIFI_CSI_ENABLED
static volatile bool     s_csi_lig = false;
static volatile int64_t  s_csi_validade = 0, s_csi_prox_us = 0;
static volatile uint16_t s_csi_intervalo_ms = 100;
static volatile int8_t   s_csi_rssi_min = -128;
static volatile bool     s_csi_so_alvo = false;
static uint8_t           s_csi_alvo[6];

static void ao_csi(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    s_csi_cb++;
    if (!info || !info->buf || info->len <= 0) { return; }
    const int8_t rssi = (int8_t)info->rx_ctrl.rssi;
    if (rssi < s_csi_rssi_min) { return; }
    if (s_csi_so_alvo && memcmp(info->mac, s_csi_alvo, 6) != 0) { return; }
    const int64_t now = esp_timer_get_time();
    /* PACING BY CADENCE, not "interval since the last one". With the source at
     * the same pace as the limit (10 Hz ping, 100 ms limit), the variable air
     * delay made half the frames arrive 95 ms after the previous one and get
     * discarded: the test gave 64 records out of 110. The cadence moves in
     * fixed steps and accepts the first frame of each step; if it falls too far
     * behind, it restarts from now. */
    const int64_t passo = (int64_t)s_csi_intervalo_ms * 1000;
    if (now < s_csi_prox_us) { return; }
    s_csi_prox_us = (now - s_csi_prox_us > passo) ? now + passo : s_csi_prox_us + passo;

    c6_csi_cab_t cab;
    memcpy(cab.mac, info->mac, 6);
    cab.rssi = rssi;
    cab.canal = (uint8_t)info->rx_ctrl.channel;
    cab.sec_canal = (uint8_t)info->rx_ctrl.second;
    int len = info->len;
    /* What the P4 needs to interpret the bytes: the frame format (each one
     * carries a different set of subcarriers), whether there is 40 MHz, and
     * whether the first word is usable - on the C6 it may be invalid due to a
     * hardware limit, and anyone unaware of that reads 2 garbage subcarriers. */
    cab.flags = (uint8_t)((info->rx_ctrl.cur_bb_format & 0x0F) << 4)
              | (info->rx_ctrl.second ? C6_CSI_F_HT40 : 0)
              | (info->first_word_invalid ? C6_CSI_F_W0_RUIM : 0)
              | (len > C6_CSI_MAX ? C6_CSI_F_CORTADO : 0);
    if (len > C6_CSI_MAX) { len = C6_CSI_MAX; }
    cab.len = (uint16_t)len;
    cab.t_us = (uint32_t)now;
    if (enfileirar2(C6_MSG_CSI, (const uint8_t *)&cab, sizeof(cab),
                    (const uint8_t *)info->buf, (size_t)len)) {
        s_csi_env++;
    }
}

/* WHY CSI GAVE ESP_FAIL (fixed on the P4 side)
 *
 * The driver's esp_wifi_set_csi* checks a field stored in esp_wifi_init: the
 * csi_enable of wifi_init_config_t. If zero, it logs "CSI not enabled in
 * menuconfig!" and returns ESP_FAIL - before looking at anything else. With
 * esp_hosted, what calls esp_wifi_init here is the RPC handler, with the fields
 * the P4 SENDS (get_merged_init_config: "always use value from host"), and the
 * P4 was sending 0. This sdkconfig's CONFIG_ESP_WIFI_CSI_ENABLED only changes
 * WIFI_INIT_CONFIG_DEFAULT, which the handler overwrites.
 *
 * It was not the call order nor promiscuous mode: after the check, the driver
 * only writes the filter to the hardware and does not fail any more. A single
 * path, the one from Espressif's example; INFO reports which step it stopped
 * at, if it stops. */
static esp_err_t csi_aplicar(uint8_t *etapa)
{
    /* Starts from what the driver has; the fields we do not touch stay as it wants them. */
    wifi_csi_config_t cfg;
    if (esp_wifi_get_csi_config(&cfg) != ESP_OK) { memset(&cfg, 0, sizeof(cfg)); }
    cfg.enable = 1;
    cfg.acquire_csi_legacy = 1;    /* L-LTF: beacons and 11b/g frames - every device */
    cfg.acquire_csi_ht20 = 1;      /* HT-LTF: 802.11n */
    cfg.acquire_csi_ht40 = 1;
    cfg.acquire_csi_su = 1;        /* HE-LTF: 802.11ax SU, what a Wi-Fi 6 router sends to the C6 */
    cfg.acquire_csi_he_stbc = 2;   /* like Espressif's esp-csi for the C6 */
    esp_err_t r;
    *etapa = 1; r = esp_wifi_set_csi_config(&cfg);        if (r != ESP_OK) { return r; }
    *etapa = 2; r = esp_wifi_set_csi_rx_cb(ao_csi, NULL); if (r != ESP_OK) { return r; }
    *etapa = 3; r = esp_wifi_set_csi(true);               if (r != ESP_OK) { return r; }
    *etapa = 0; return ESP_OK;
}

static void csi_ligar(const c6_csi_cfg_t *c)
{
    s_csi_intervalo_ms = c->min_intervalo_ms ? c->min_intervalo_ms : 100;
    s_csi_rssi_min = c->rssi_min ? c->rssi_min : -128;
    static const uint8_t zero[6] = { 0 };
    s_csi_so_alvo = c->so_alvo && memcmp(c->alvo, zero, 6) != 0;
    memcpy(s_csi_alvo, c->alvo, 6);
    s_csi_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    /* Reapplied on EVERY renewal, not just the first: the P4 may stop and
     * restart Wi-Fi in between (Config), and esp_wifi_stop undoes CSI
     * underneath. With "only the first", the P4 would keep renewing a CSI that
     * was already dead - the same trap the sniffer handles in its renewal. */
    uint8_t etapa = 0;
    const esp_err_t r = csi_aplicar(&etapa);
    s_csi_err = s_csi_err2 = (int16_t)r;   /* the last time; the P4 reads it in INFO */
    s_csi_etapa = etapa;
    if (r == ESP_OK) {
        if (!s_csi_lig) {
            ESP_LOGI(TAG, "CSI on (interval %u ms)", (unsigned)s_csi_intervalo_ms);
        }
        s_csi_lig = true;
    } else {
        if (!s_csi_lig) {
            ESP_LOGW(TAG, "CSI refused: %s (step %u)%s", esp_err_to_name(r), (unsigned)etapa,
                     r == ESP_FAIL ? " - the host initialised Wi-Fi with csi_enable 0" : "");
        }
        s_csi_lig = false;
    }
    s_csi_modo = s_csi_lig ? 1 : 0;
}
static void csi_desligar(void)
{
    if (s_csi_lig) { esp_wifi_set_csi(false); s_csi_lig = false; ESP_LOGI(TAG, "CSI off"); }
    s_csi_modo = 0;
}
static void csi_guarda(int64_t agora)
{
    if (s_csi_lig && agora > s_csi_validade) { ESP_LOGW(TAG, "CSI: host did not renew"); csi_desligar(); }
}
#else
static void csi_ligar(const c6_csi_cfg_t *c) { (void)c; }
static void csi_desligar(void) {}
static void csi_guarda(int64_t a) { (void)a; }
#endif

/* ---- FTM (one-shot) ---- */
#if CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT
static bool s_ftm_evt = false;

static void ftm_evento(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != WIFI_EVENT_FTM_REPORT || !data) { return; }
    const wifi_event_ftm_report_t *r = (const wifi_event_ftm_report_t *)data;
    c6_ftm_rep_t rep;
    memcpy(rep.peer, r->peer_mac, 6);
    rep.status = (uint8_t)r->status;
    rep.num = r->ftm_report_num_entries;
    rep.rtt_est_ns = r->rtt_est;
    rep.dist_est_cm = r->dist_est;
    rep.rtt_raw_ns = r->rtt_raw;
    enfileirar(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    /* The detailed report does not go to the P4 (only the summary); free it so it does not leak. */
    if (r->ftm_report_data) { free(r->ftm_report_data); }
}

static void ftm_disparar(const c6_ftm_cfg_t *c)
{
    if (!s_ftm_evt) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftm_evento, NULL);
        s_ftm_evt = true;
    }
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = c->canal, .frm_count = c->frm_count, .burst_period = c->burst_period,
    };
    memcpy(cfg.resp_mac, c->peer, 6);
    const esp_err_t r = esp_wifi_ftm_initiate_session(&cfg);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "FTM refused (%s)", esp_err_to_name(r));
        c6_ftm_rep_t rep;
        memset(&rep, 0, sizeof(rep));
        memcpy(rep.peer, c->peer, 6);
        rep.status = C6_FTM_NAO_INICIOU;
        rep.rtt_raw_ns = (uint32_t)(int32_t)r;   /* the reason: without it the P4 would only know "failed" */
        enfileirar(C6_MSG_FTM, (const uint8_t *)&rep, sizeof(rep));
    }
}
#else
static void ftm_disparar(const c6_ftm_cfg_t *c) { (void)c; }
#endif

/* ---- 802.15.4 (the other radio: raw Thread/Zigbee) ---- */
#if CONFIG_IEEE802154_ENABLED
static volatile int64_t  s_z154_validade = 0, s_z154_ult_us = 0;
static volatile uint16_t s_z154_intervalo_ms = 0;

/* WEAK symbol in the driver: ours wins and is called for every frame received.
 *
 * WARNING: the driver calls this DIRECTLY from the 802.15.4 radio ISR (there is
 * no task in between). So only the FromISR variants are allowed here: a plain
 * xQueueSend (enfileirar2) trips the FreeRTOS assert on the first frame and
 * brings the C6 down. The buffer is static: the ISR does not re-enter, and the
 * ISR stack is short. */
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    /* msg_id + header + frame, assembled here and copied in one go into the
     * ring (the FromISR variant has no Acquire). Static: the ISR does not
     * re-enter, and the ISR stack is short. */
    static uint8_t it[4 + sizeof(c6_154_cab_t) + C6_154_MAX];
    s_z154_rx++;
    if (frame && info && s_saida) {
        const int64_t now = esp_timer_get_time();
        if (!s_z154_intervalo_ms || now - s_z154_ult_us >= (int64_t)s_z154_intervalo_ms * 1000) {
            s_z154_ult_us = now;
            uint8_t len = frame[0];            /* frame[0] = length; the frame follows (FCS replaced by RSSI/LQI) */
            if (len > C6_154_MAX) { len = C6_154_MAX; }
            const uint32_t id = C6_MSG_154;
            c6_154_cab_t cab;
            cab.canal = info->channel;
            cab.rssi = info->rssi;
            cab.lqi = info->lqi;
            cab.len = len;
            cab.t_us = (uint32_t)now;
            memcpy(it, &id, 4);
            memcpy(it + 4, &cab, sizeof(cab));
            memcpy(it + 4 + sizeof(cab), frame + 1, len);
            BaseType_t acordou = pdFALSE;
            if (xRingbufferSendFromISR(s_saida, it, 4 + sizeof(cab) + len, &acordou) != pdTRUE) {
                s_saida_perdas++;
            }
            if (acordou) { portYIELD_FROM_ISR(); }
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

/* LISTEN ONLY, NEVER REPLY. The driver starts with automatic ACK on, and in
 * promiscuous mode (no address filter) it would ACK frames from ANY device in
 * the mesh that asked for confirmation: the sender would think the
 * destination received it when perhaps it had not, and the neighbouring
 * Zigbee/Thread network would start losing messages because of us. There is no
 * public API for this; the two below are the driver's (esp_ieee802154_pib.h,
 * private) and take effect on the next esp_ieee802154_receive(), which re-reads
 * the pending configuration. */
extern void ieee802154_pib_set_auto_ack_tx(bool enable);
extern void ieee802154_pib_set_enhance_ack_tx(bool enable);

/* TURNING ON AND OFF - the two points where Wi-Fi went down.
 *
 * 1. COEXISTENCE (on enable). Wi-Fi, BLE and 802.15.4 share ONE RF path on the
 *    C6, and the coexistence arbiter divides up the time. It only takes
 *    802.15.4 into account after esp_coex_wifi_i154_enable() (coex_enable +
 *    esp_coex_ieee802154_status_enable); the 802.15.4 driver never does this by
 *    itself - the official ot_br and esp_zigbee_gateway examples, both "Wi-Fi +
 *    802.15.4 on a C6", call the function in the application. With it, while
 *    listening, 802.15.4 requests the radio with the driver's "txrx" priority
 *    (low); Wi-Fi goes first when it has traffic and 802.15.4 listens in the gaps.
 *
 * 2. TURN OFF VIA SLEEP, NEVER VIA DISABLE. Measured on the device: with the
 *    right coexistence, Wi-Fi passed data WITH 802.15.4 on and died right
 *    AFTER turning it off. esp_ieee802154_disable() cuts the MAC clock
 *    (modem_clock_module_disable) and changes state without
 *    stop_current_operation(): with the receiver on (rx_when_idle), the MAC is
 *    frozen mid-reception, with the radio request made to the arbiter.
 *    esp_ieee802154_sleep() is the clean path - it stops the operation in
 *    progress (CMD_STOP), enters SLEEP and releases RF (esp_phy_disable) - and
 *    it is what OpenThread uses all the time with Wi-Fi on. The driver stays
 *    enabled and asleep: the stopped MAC costs only its clock, and turning it
 *    back on is a receive().
 *
 * Different tasks call this (the host RPC and the 1 s watchdog): the
 * s_radio_mtx lock serialises them. */

static void z154_ligar(const c6_154_cfg_t *c)
{
    s_z154_intervalo_ms = c->min_intervalo_ms;
    s_z154_validade = esp_timer_get_time() + (int64_t)C6_RADIO_VALIDADE_MS * 1000;
    uint8_t canal = (c->canal >= 11 && c->canal <= 26) ? c->canal : 15;
    const bool novo = !s_z154_lig;
    if (!s_z154_habilitado) {
        esp_coex_wifi_i154_enable();
        const esp_err_t r = esp_ieee802154_enable();
        s_z154_err = (int16_t)r;
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "802.15.4 enable refused: %s", esp_err_to_name(r));
            esp_coex_ieee802154_status_disable();
            return;
        }
        s_z154_habilitado = true;
    } else if (novo) {
        esp_coex_ieee802154_status_enable();
    }
    s_z154_lig = true;
    /* On every receive() the driver re-reads the pending configuration (pib). */
    ieee802154_pib_set_auto_ack_tx(false);
    ieee802154_pib_set_enhance_ack_tx(false);
    esp_ieee802154_set_channel(canal);
    esp_ieee802154_set_promiscuous(c->promiscuo != 0);
    esp_ieee802154_set_rx_when_idle(true);
    const esp_err_t rr = esp_ieee802154_receive();
    s_z154_err = (int16_t)rr;
    /* Renewals arrive every few seconds: only log what changes. */
    if (novo) { ESP_LOGI(TAG, "802.15.4 listening: channel %u", (unsigned)canal); }
}
static void z154_desligar(void)
{
    if (!s_z154_lig) { return; }
    esp_ieee802154_set_rx_when_idle(false);   /* otherwise the driver goes back to listening by itself */
    esp_ieee802154_sleep();
    esp_coex_ieee802154_status_disable();     /* out of the arbiter: Wi-Fi has the radio all to itself */
    s_z154_lig = false;
    ESP_LOGI(TAG, "802.15.4 sleeping");
}
static void z154_guarda(int64_t agora)
{
    if (s_z154_lig && agora > s_z154_validade) { ESP_LOGW(TAG, "802.15.4: host did not renew"); z154_desligar(); }
}
#else
static void z154_ligar(const c6_154_cfg_t *c) { (void)c; }
static void z154_desligar(void) {}
static void z154_guarda(int64_t a) { (void)a; }
#endif

/* Single watchdog: turns off CSI/802.15.4 that the host stopped renewing.
 *
 * THE WORK DOES NOT RUN ON THE esp_timer. The esp_timer task is shared - the
 * BLE controller uses it (CONFIG_BT_LE_USE_ESP_TIMER), as does esp_hosted
 * itself. Waiting on a lock there (up to 50 ms) and calling the Wi-Fi or
 * 802.15.4 driver (which wait for the radio task) delayed every other timer.
 * The callback only wakes c6farejo, which is already sleeping idle and does
 * the work at leisure. */
static void radio_guarda(int64_t agora)
{
    if (!s_radio_mtx) { return; }
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    csi_guarda(agora);
    z154_guarda(agora);
    xSemaphoreGive(s_radio_mtx);
}

static void radio_guarda_cb(void *arg)
{
    (void)arg;
#if CONFIG_ESP_WIFI_CSI_ENABLED
    const bool csi = s_csi_lig;
#else
    const bool csi = false;
#endif
    /* Nothing on: do not even wake anyone (an idle C6 stays idle). */
    if ((csi || s_z154_lig) && s_fj_tarefa) { xTaskNotifyGive(s_fj_tarefa); }
}

/* ---------------------------------------------------------------------------
 * Coming from the P4
 * --------------------------------------------------------------------------- */

static void do_host(uint32_t msg_id, const uint8_t *dados, size_t n, void *ctx)
{
    (void)ctx;

    switch (msg_id) {
    case MSG_HOST_VIVO:
        host_falou();
        break;

    case MSG_HOST_ENVIAR: {
        if (n <= ESP_NOW_ETH_ALEN) { return; }
        /* STATIC: envio_t is 1.5 KB and this is the stack of esp_hosted's
         * pserial_task (4 KB, not ours). Only it calls do_host; xQueueSend
         * copies before returning. */
        static envio_t e;
        memcpy(e.mac, dados, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN) { tam = ESP_NOW_MAX_DATA_LEN; }
        memcpy(e.texto, dados + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = false;
        /* With ESP-NOW not up, or the queue full: IMMEDIATE failure - otherwise the
         * Tab5's screen would wait for the confirmation forever. */
        if (!s_espnow_no_ar || !s_envio || xQueueSend(s_envio, &e, 0) != pdTRUE) {
            uint8_t st[ESP_NOW_ETH_ALEN + 1];
            memcpy(st, e.mac, ESP_NOW_ETH_ALEN);
            st[6] = 0;
            enfileirar(MSG_C6_ESTADO, st, sizeof(st));
        }
        break;
    }

    case MSG_HOST_ESPNOW:
        ligar_espnow();
        break;

    case C6_MSG_HOST_FAREJO:
        pedir_farejo(dados, n);
        break;

    case C6_MSG_HOST_CRU: {
        if (n <= ESP_NOW_ETH_ALEN || !s_espnow_no_ar || !s_envio) { return; }
        static envio_t e;   /* static: see MSG_HOST_ENVIAR */
        memcpy(e.mac, dados, ESP_NOW_ETH_ALEN);
        size_t tam = n - ESP_NOW_ETH_ALEN;
        if (tam > ESP_NOW_MAX_DATA_LEN_V2) { tam = ESP_NOW_MAX_DATA_LEN_V2; }
        memcpy(e.texto, dados + ESP_NOW_ETH_ALEN, tam);
        e.n = (uint16_t)tam;
        e.cru = true;
        xQueueSend(s_envio, &e, 0);
        break;
    }

    case C6_MSG_HOST_AJUSTE:
        if (n < 1) { return; }
        switch (dados[0]) {
        case C6_AJ_INFO: responder_info(); break;
        case C6_AJ_PM: {
            /* What keeps the C6 awake: the PM lock table, as text. */
            static char txt[1400];   /* fits in one link message */
            FILE *f = fmemopen(txt, sizeof(txt) - 1, "w");
            if (f) {
                fprintf(f, "--- agora ---\n");
                esp_pm_dump_locks(f);
#if CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS
                fprintf(f, "--- 5 s depois do ultimo 'host dormiu' ---\n%s", s_pm_dormindo[0] ? s_pm_dormindo : "(ainda nao)\n");
#endif
                fclose(f);
                txt[sizeof(txt) - 1] = 0;
                enfileirar(C6_MSG_PM, (const uint8_t *)txt, strlen(txt));
            }
            break;
        }
        case C6_AJ_CRU:  s_repassar_cru = (n >= 2 && dados[1]); break;
#if CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS
        case C6_AJ_ESPNOW_JANELA:
            if (n >= 5) {
                s_en_intervalo = (uint16_t)(dados[1] | (dados[2] << 8));
                s_en_janela    = (uint16_t)(dados[3] | (dados[4] << 8));
                espnow_aplicar_janela();
                ESP_LOGI(TAG, "ESP-NOW: %u ms window every %u ms", (unsigned)s_en_janela, (unsigned)s_en_intervalo);
            }
            break;
        case C6_AJ_IP:   if (n >= 5) { uint32_t ip; memcpy(&ip, dados + 1, 4); s_ip_host = ip; } break;
#endif
        default: break;   /* new key from a newer P4: ignore */
        }
        break;

    case C6_MSG_HOST_RADIO: {
        if (n < 1) { return; }
        const uint8_t  sub = dados[0];
        const uint8_t *p = dados + 1;
        const size_t   m = n - 1;
        if (!s_radio_mtx) { return; }
        xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
        switch (sub) {
        case C6_RX_CSI: { c6_csi_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          if (c.liga) { csi_ligar(&c); } else { csi_desligar(); } break; }
        case C6_RX_FTM: { c6_ftm_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          ftm_disparar(&c); break; }
        case C6_RX_154: { c6_154_cfg_t c = { 0 }; memcpy(&c, p, m < sizeof(c) ? m : sizeof(c));
                          if (c.liga) { z154_ligar(&c); } else { z154_desligar(); } break; }
        default: break;   /* new sub-command from a newer P4: ignore */
        }
        xSemaphoreGive(s_radio_mtx);
        break;
    }

    case MSG_HOST_CANAL:
        /* EVERYONE HAS TO BE ON THE SAME CHANNEL, and this is the number one reason
         * for "it does not work". If the C6 is associated with an AP, the AP
         * controls the channel - changing it here would drop the connection. So
         * we only touch it when the P4 explicitly asks. */
        if (n >= 1) {
            esp_err_t r = esp_wifi_set_channel(dados[0], WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "channel -> %u: %s", (unsigned)dados[0], esp_err_to_name(r));
            if (r == ESP_OK) { s_canal_volta = dados[0]; }   /* the sniffer returns to this one */
        }
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Tab5 C6 firmware (esp_hosted + ESP-NOW + sniffer), contract v%d", C6_PROTO_VERSAO);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    /* No ESP_ERROR_CHECK, for the same reason as the registration below: nothing
     * here may bring down the coprocessor. NVS failing is bad; aborting is FAR worse. */
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(ret)); }
    esp_event_loop_create_default();

    /* New image guard BEFORE anything that could go wrong. */
    guarda_iniciar();

    /* VARIABLE CLOCK (DFS) AND LIGHT SLEEP ONLY WITH THE HOST ASLEEP. Measured on
     * the Tab5 with 'energia ensaio' (2026-10-16): connected Wi-Fi cost 28 mA of
     * the 74 at idle - without PM the CPU here stayed at 160 MHz all the time.
     * With DFS the Wi-Fi driver releases the lock between beacons and the CPU
     * drops to 40 MHz (XTAL). Light sleep stays ENABLED in PM, but held off by
     * the link lock (see "THE HOST SLEEPS, SO DOES THE C6"): SDIO is the P4's
     * slave, and sleeping with it up would leave the host talking to itself.
     * A failure only logs. */
    sono_trava_criar();
    {
        const esp_pm_config_t pm = { .max_freq_mhz = 160, .min_freq_mhz = 40,
                                     .light_sleep_enable = CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS ? true : false };
        const esp_err_t r = esp_pm_configure(&pm);
        if (r == ESP_OK) { ESP_LOGI(TAG, "PM: clock 40-160 MHz, light sleep %s", pm.light_sleep_enable ? "with the host asleep" : "off"); }
        else             { ESP_LOGW(TAG, "PM: esp_pm_configure %s", esp_err_to_name(r)); }
    }

    s_saida = xRingbufferCreate(SAIDA_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (s_saida) {
        xTaskCreate(vTarefaSaida, "c6saida", 4096, NULL, 5, NULL);
    }
    /* The lock before c6farejo: it is the one that runs the radio extras watchdog. */
    s_radio_mtx = xSemaphoreCreateMutex();
    /* Sleeps in ulTaskNotifyTake until the host asks for sniffing (or the
     * watchdog wakes it): idle, it costs only its stack. 4096 like c6saida: both
     * call eh_cp_feat_peer_data_send, which encodes the RPC event on the
     * caller's stack. */
    xTaskCreate(vTarefaFarejo, "c6farejo", 4096, NULL, 4, &s_fj_tarefa);

    /* Radio extras watchdog (CSI/802.15.4): 1x/s wakes c6farejo if any of them
     * is on. */
    esp_timer_handle_t guarda = NULL;
    const esp_timer_create_args_t ga = { .callback = radio_guarda_cb, .name = "radio_guarda" };
    if (esp_timer_create(&ga, &guarda) == ESP_OK) {
        esp_timer_start_periodic(guarda, 1000000);
    }

    /* NO ESP_ERROR_CHECK HERE. That was the bug.
     *
     * The coprocessor's features (peer_data included) come up in a TASK OF
     * THEIR OWN - eh_cp_core's auto_feat_init_task -, not before app_main.
     * Until it has run, register_callback returns ESP_ERR_INVALID_STATE
     * ("Not initialized"), and ESP_ERROR_CHECK on that ABORTS the C6.
     *
     * Aborting here is the worst possible outcome: the C6 goes into a reboot
     * loop, the SDIO link negotiates and dies on every round, EVERY host RPC
     * times out - and since the device never stays up long enough, you cannot
     * even flash new firmware via OTA to fix it. That was exactly the symptom.
     *
     * Rule: NOTHING in a coprocessor's app_main may abort. It is the only
     * recovery path for itself. */
    for (int tent = 0; ; ++tent) {
        const esp_err_t r = eh_cp_feat_peer_data_register_callback(MSG_HOST_VIVO, do_host, NULL);
        if (r == ESP_OK) { break; }
        if (tent == 0) {
            ESP_LOGW(TAG, "peer_data not up yet (%s) - waiting",
                     esp_err_to_name(r));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ENVIAR, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_CANAL,  do_host, NULL);
    eh_cp_feat_peer_data_register_callback(MSG_HOST_ESPNOW, do_host, NULL);
    /* peer_data table: 8 of 8 slots, VIVO already counted (see tab5_c6.h). */
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_FAREJO, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_CRU,    do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_AJUSTE, do_host, NULL);
    eh_cp_feat_peer_data_register_callback(C6_MSG_HOST_RADIO,  do_host, NULL);  /* 8th: table full */

    sono_iniciar();

    ESP_LOGI(TAG, "ready - waiting for the host to ask to start ESP-NOW");
}
