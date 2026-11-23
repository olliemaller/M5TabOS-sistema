# The Tab5's radio: the ESP32-C6

The Tab5's ESP32-P4 has no radio. Wi-Fi, Bluetooth LE and the 802.15.4 radio
live in the **ESP32-C6**, connected to the P4 over SDIO and talking to it through
**esp_hosted** (3.0.10 on both sides). This document describes how M5tabOS uses
the C6, who is in charge of what, and the traps that cost us test runs on the
device — with the proof of each one in the ESP-IDF and esp_hosted sources, or in
the driver binary.

Versions checked: ESP-IDF 5.5.5, esp_hosted 3.0.10, C6 firmware
`m5tabos-c6-5` (contract `C6_PROTO_VERSAO 3`).

## Who runs what

| Feature | Where it runs | How the P4 gets there |
|---|---|---|
| Wi-Fi STA (associate, IP, sockets) | driver on the C6 | esp_hosted RPC (the P4-side `esp_wifi_*` become remote calls) |
| Bluetooth LE | controller on the C6, NimBLE stack on the P4 | HCI over VHCI |
| ESP-NOW, sniffing (promiscuous), CSI, FTM, 802.15.4 | our own C6 firmware (`c6/`) | esp_hosted `peer_data` channel, contract in `compartilhado/tab5_c6.h` |

esp_hosted does not carry ESP-NOW, promiscuous mode or CSI over RPC: the IDs
exist in `rpc_v2.proto`, but there is no implementation on either side. That is
why these features run **on the C6**, and the P4 only switches them on and off
and receives the results.

## Golden rule: the C6's Wi-Fi init config comes from the P4

What calls `esp_wifi_init()` on the C6 is not our `app_main`: it is esp_hosted's
RPC handler (`req_wifi_init`, in
`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c`),
with the `wifi_init_config_t` that **the P4 sent**. The function
`get_merged_init_config` states the policy in its own comment — *"always
use value from host"* — for almost every field:

`static_rx_buf_num`, `dynamic_rx_buf_num`, `tx_buf_type`, `static_tx_buf_num`,
`dynamic_tx_buf_num`, `rx_mgmt_buf_*`, **`csi_enable`**, `ampdu_*`, `amsdu_tx`,
`nvs_enable`, `rx_ba_win`, `beacon_max_len`, `mgmt_sbuf_num`,
`sta_disconnected_pm`, `espnow_max_encrypt_num`...

Consequence: **the C6's `sdkconfig` does not decide these values.**
`CONFIG_ESP_WIFI_CSI_ENABLED=y` on the C6 only changes the
`WIFI_INIT_CONFIG_DEFAULT` there, which the handler overwrites with what arrives
from the P4. The values that count are the `CONFIG_WIFI_RMT_*` in the P4's
`sdkconfig`, or whatever the P4's code puts in the struct before `esp_wifi_init`
(`main/hal/tab5_wifi.cpp`).

The exception is `feature_caps` (FTM, WPA3, GCMP...): the C6 only accepts the
P4's when it equals its own or when there is a TX cache (only with PSRAM, which
the C6 does not have); otherwise it uses **its own**. Checked in the flashed
binary (`req_wifi_init`): the C6's value is 301 (0x12D = WPA3-SAE, FTM initiator
and responder, GMAC, BSS max idle) and that is what goes on to `esp_wifi_init`.
The driver's FTM support lives in that field (`ftm_is_initiator_supported` reads
bit 2 of `feature_caps`), so it is what decides whether the C6 agrees to start
an FTM session — see [FTM](#ftm-distance-measurement--a-chip-limitation).

When the P4 restarts, it resets the C6, so a change to that config takes effect
on the next boot. If the C6 already has Wi-Fi started and the config arrives
different, esp_hosted stops, deinitialises and restarts Wi-Fi by itself
(`__wrap_esp_wifi_init`).

## Trap 1: CSI refused with `ESP_FAIL`

**Symptom.** `radio csi` on the console: the C6 refuses to turn CSI on with
`ESP_FAIL` and the callback is never called. The configuration was the same as
the official example's (esp-csi) for the C6; changing the order of the calls or
turning promiscuous mode on first changed nothing.

**Cause.** The P4 started Wi-Fi with `csi_enable = 0`
(`CONFIG_WIFI_RMT_CSI_ENABLED` off), and the C6 obeyed.

**Proof, in the driver binary** (`components/esp_wifi/lib/esp32c6/libnet80211.a`,
disassembled with `riscv32-esp-elf-objdump`):

1. `wifi_menuconfig_init` (in `ieee80211_ioctl.o`) copies fields from
   `wifi_init_config_t` into the global `g_wifi_menuconfig`. The field at offset
   80 of the input struct goes to offset 24 of the global. Offset 80 is
   `csi_enable`: `osi_funcs` (4) + `wpa_crypto_funcs` (44) put
   `static_rx_buf_num` at 48, and eight more `int`s take it to 80.
2. `esp_wifi_set_csi` and `esp_wifi_set_csi_config` (in `ieee80211_api.o`)
   check, in this order: Wi-Fi initialised (`ESP_ERR_WIFI_NOT_INIT`), Wi-Fi
   started (`ESP_ERR_WIFI_NOT_STARTED`), and **`g_wifi_menuconfig + 24`**. If it
   is zero, they call `wifi_log` with the string *"CSI not enabled in
   menuconfig!"* and return **-1 (`ESP_FAIL`)**.
3. After that check there is no other error path: `wifi_set_csi` →
   `ic_set_csi` → `hal_mac_set_csi`, and `wifi_csi_set_config` →
   `ic_csi_set_config` only write registers. Neither call order nor promiscuous
   mode has any influence.

**Fix.** On the P4, `cfg.csi_enable = 1` before `esp_wifi_init`
(`main/hal/tab5_wifi.cpp`). It does not require reflashing the C6. Cost: the C6
reserves about 1 KB per static RX buffer for CSI (with 10 buffers, ~10 KB of its
RAM), even with CSI stopped. Turning it on only on request would mean
restarting Wi-Fi every time.

On the C6, CSI now has a single path (the one in Espressif's example):
`esp_wifi_set_csi_config` → `esp_wifi_set_csi_rx_cb` → `esp_wifi_set_csi(true)`,
starting from the config the driver already has. Frame types enabled: L-LTF
(legacy, beacons), HT20, HT40 and HE SU (802.11ax, what a Wi-Fi 6 router
sends to the C6), with `acquire_csi_he_stbc = 2`. On the C6,
`CONFIG_SOC_WIFI_MAC_VERSION_NUM` is 2, so the struct is the variant without
`acquire_csi_force_lltf`/`vht` (`esp_wifi_he_types.h`).

`sniff info` (`farejo info`) shows the diagnosis: the error, the step where it
stopped (1 config, 2 callback, 3 turn on) and whether it is on. `ESP_FAIL` there
always means "Wi-Fi started with `csi_enable` 0".

### Where CSI comes from (and why only 6 arrived in 10 s)

With CSI on, the first test gave **6 callbacks in 10 s**, nowhere near
100. The driver only called the callback 6 times. With ping as the source, there
were 110 callbacks in 10 s, all HT.

CSI is measured on the **OFDM** training fields of the preamble (L-LTF, HT-LTF,
HE-LTF). At 2.4 GHz, the router's beacon usually goes out at 1 Mbps (11b, DSSS),
which has none of them and so generates no CSI. Without promiscuous mode, the C6
only receives frames addressed to it, and with Wi-Fi idle there are hardly any:
the 6 records were the MQTT messages that arrived during the test.

To get a steady stream, something has to **provoke OFDM traffic towards the
C6**. The official example (esp-csi, `csi_recv_router`) pings the router
non-stop, and `radio csi` does the same: a ping to the gateway at 10 Hz during
the window. Each reply is an HT/HE frame from the router to the C6. For CSI from
**other** devices, promiscuous mode (sniffing) has to be on.

### What comes in each record

`c6_csi_cab_t` (16 B) + the CSI bytes. The `flags` byte carries what changes how
the bytes are read:

- bits 4..7: the frame format (the driver's `cur_bb_format`: 0 11b, 1 11g,
  2 HT, 4 HE SU...), because each format carries a different set of
  subcarriers;
- `C6_CSI_F_HT40`: there is a secondary channel;
- `C6_CSI_F_W0_RUIM`: the first 4 bytes are invalid. It is the driver's
  `first_word_invalid`, a limitation of the C6 hardware; whoever ignores
  this flag reads two subcarriers of garbage;
- `C6_CSI_F_CORTADO`: the driver delivered more than `C6_CSI_MAX`.

`C6_CSI_MAX` is 1024 B, because `rx_channel_estimate_len` has 10 bits (up to
1023). The first firmware truncated at 256 B, and every record in the test came
in at exactly 256: it was losing data.

Practical limits:
- In modem sleep the C6 switches its receiver off between beacons; `radio csi`
  keeps Wi-Fi awake during the test (`tab5_wifi_manter_acordado`).
- `min_intervalo_ms` limits the rate on the C6 so as not to flood the SDIO. The
  limit moves by **cadence** (fixed steps, accepting the first frame of each
  step), and not by "interval since the last one": with the source at the same
  pace as the limit, the variable over-the-air delay discarded almost half
  (64 records out of 110). The `radio csi` source runs at twice the limit
  (20 Hz) for the same reason.

## Trap 2: 802.15.4 was taking Wi-Fi down

**Symptom.** `ping` to the router gave 4/4 before `radio 154` and 0/4 after. The
C6 stayed associated, but data did not get through, and the 802.15.4 radio did
not receive a single frame.

There were **two** causes, found one at a time: the first fix (coexistence)
let Wi-Fi pass data **with** 802.15.4 on (ping 4/4 in the middle of the test),
but it still died **after** switching off. Hence the `radio 154` test measures
before, during and after — they are failures at different moments.

### Cause 1 (on): coexistence

Wi-Fi, BLE and 802.15.4 share **one** RF path on the C6, and what divides the
time between them is the coexistence arbiter. It only takes 802.15.4 into
account after `esp_coex_wifi_i154_enable()`, and the firmware did not call that
function.

**Proof, in the IDF sources:**
- `components/esp_coex/src/coexist.c`: `esp_coex_wifi_i154_enable()` does
  `coex_enable()` + `esp_coex_ieee802154_status_enable()`.
- The 802.15.4 driver (`components/ieee802154/`) **never** calls
  `esp_coex_ieee802154_status_enable`: it only adjusts the priorities (PTI) per
  event. Turning on the status notification is the application's job.
- Both official "Wi-Fi + 802.15.4 on a single C6" examples call the function in
  the application: `examples/openthread/ot_br/main/esp_ot_br.c` and
  `examples/zigbee/esp_zigbee_gateway/main/esp_zigbee_gateway.c`.

**Fix:** `esp_coex_wifi_i154_enable()` before the first
`esp_ieee802154_enable()`, and `esp_coex_ieee802154_status_enable()` every time
it starts listening again.

With coexistence on, `esp_ieee802154_receive()` puts the radio in the `RX`
scene, which requests the radio with the `txrx` priority from `s_coex_config`
(`driver/esp_ieee802154_util.c`; `IDLE` is only for a stopped MAC). Wi-Fi goes
first when it has traffic, and 802.15.4 listens in the gaps. In the test, the
ping latency to the router rose from 5 ms to 48 ms with 802.15.4 listening,
but no packet was lost.

### Cause 2 (on switching off): `disable` in the middle of a reception

```c
esp_err_t esp_ieee802154_disable(void)   // components/ieee802154/esp_ieee802154.c
{
    esp_btbb_disable();
    ieee802154_rf_disable();   // esp_phy_disable(PHY_MODEM_IEEE802154)
    ieee802154_disable();      // modem_clock_module_disable + DISABLE state
    return ieee802154_mac_deinit();
}
```

`ieee802154_disable()` cuts the MAC clock and changes the state **without
`stop_current_operation()`**. With the receiver always on (`rx_when_idle`), the
MAC is in `RX` with a radio request lodged with the arbiter, and it is frozen
like that. The shared clocks are not the problem: `modem_clock` counts
references, and Wi-Fi and BLE hold their own.

The driver has the clean path: `esp_ieee802154_sleep()` does
`stop_current_operation()` (sends `CMD_STOP` to the MAC), enters `SLEEP` and
releases the RF via `esp_phy_disable`. It is what OpenThread uses all the time
with Wi-Fi on (`esp_openthread_radio.c`); its `disable` only runs when tearing
down the whole stack.

**Fix:** switching off = `esp_ieee802154_set_rx_when_idle(false)` (otherwise the
driver starts listening again by itself) + `esp_ieee802154_sleep()` +
`esp_coex_ieee802154_status_disable()`. **`esp_ieee802154_disable()` is never
called.** The driver stays enabled and sleeping; when stopped, the MAC costs only
its clock, and listening again is one `esp_ieee802154_receive()`. `sniff info`
shows the state: never on, listening or sleeping.

Switching on and off happen in different tasks (the host RPC switches on, the
1 s watchdog switches off what the host abandoned), and a lock (`s_radio_mtx`)
stops the two from touching the driver at the same time.

**What to expect** — the IDF coexistence table for the ESP32-C6
([api-guides/coexist](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/api-guides/coexist.html)):

| 802.15.4 | Wi-Fi STA scanning | Wi-Fi STA connecting | Wi-Fi STA connected |
|---|---|---|---|
| Scan / Router (receiver always on) | C1 (unstable) | C1 (unstable) | **Y (stable)** |
| End Device | Y | Y | Y |

M5tabOS's mode (listen only, receiver always on) is equivalent to "Router": with
Wi-Fi connected it is supported and stable, but 802.15.4 loses frames when
Wi-Fi is busy. Espressif itself recommends two chips with separate antennas for
a real Thread/Zigbee gateway; on the Tab5, 802.15.4 is for listening and
measuring, not for being a network node.

802.15.4 stays **listen-only**: automatic ACK is switched off
(`ieee802154_pib_set_auto_ack_tx(false)`, a private driver API), otherwise in
promiscuous mode the C6 would acknowledge other devices' frames and mislead the
neighbouring network.

**Test.** `radio 154 [channel] [s]` pings the router before, in the middle of
the window (with 802.15.4 on) and after. `DURANTE` (during) at 4/4 confirms
coexistence, and `DEPOIS` (after) at 4/4 confirms the switch-off. Zero frames
with no Zigbee/Thread nearby is normal. Measured with both fixes: 4/4 in all
three, latency of 12 ms before, 19 ms during and 7 ms after.

## FTM: distance measurement — a chip limitation

**Symptom.** `radio ftm` returned `status 255`. That value does not come from the
driver (its statuses go from 0 to 6): it is the firmware's code for
"`esp_wifi_ftm_initiate_session` refused". With the reason exposed, the error is
`ESP_ERR_NOT_SUPPORTED`.

**Cause — silicon errata, not configuration.** In the binary,
`esp_wifi_ftm_initiate_session` only returns `ESP_ERR_NOT_SUPPORTED` in two
cases: Wi-Fi in AP mode ("FTM initiator needs Station mode!") or
`ftm_is_initiator_supported()` false ("FTM Initiator mode not supported!"),
which reads bit 2 of `feature_caps`. The C6 starts Wi-Fi with that bit set (301,
above), but the IDF's `esp_wifi_init` clears it before it reaches the driver:

```c
/* components/esp_wifi/src/wifi_init.c */
#ifdef CONFIG_ESP_WIFI_FTM_ENABLE
    esp_chip_info_t info = {0};
    esp_chip_info(&info);
    if (info.model == CHIP_ESP32C6 && info.revision <= 1) {
        ((wifi_init_config_t *)config)->feature_caps &= ~(CONFIG_FEATURE_FTM_INITIATOR_BIT);
    }
#endif
```

**ESP32-C6 revisions v0.0 and v0.1 cannot act as FTM initiator**; Espressif
switches it off in the IDF itself. The C6 in this Tab5 is one of those
revisions. There is no honest workaround: forcing the bit back on would give
wrong time-of-flight measurements. The responder side (the C6 acting as the
reference for another device to measure against) is not affected.

**In the firmware:** the C6 only advertises `C6_CAP_FTM` if the revision allows
it (`caps_reais()`), INFO carries the chip revision (`chip_rev`), and
`radio ftm` explains the reason when the driver refuses. The synchronous refusal
has other possible outcomes in the binary — `ESP_ERR_INVALID_ARG` (`frm_count`
outside 0/8/16/24/32/64, `burst_period` 1 or above 255, channel outside the
regulatory domain), `ESP_FAIL` (STA interface not created), `ESP_ERR_NO_MEM` —
and the error name always shows. A router that does not speak FTM does **not**
cause a synchronous refusal: the session starts and ends with status 1 in
`WIFI_EVENT_FTM_REPORT`.

## The C6 sleeping with Wi-Fi associated (firmware `m5tabos-c6-5`)

With the screen off, connected Wi-Fi cost ~30 mA from the battery. Two causes,
found by measuring on the C6 itself (`c6 locks` / `c6 travas`, the table of its
PM locks with the time spent in each mode):

1. **ESP-NOW held the radio awake.** With ESP-NOW started, the IDF's default
   wake window is "the maximum" (`esp_now_set_wake_window`, see
   `esp_now.h`), and the radio never sleeps between beacons. The `wifi` lock
   (APB_FREQ_MAX) stayed held 99% of the time and was taken twice since boot:
   the maximum modem sleep the P4 asked for had no effect at all. The key
   `C6_AJ_ESPNOW_JANELA` sets the window (`c6 window <interval> <window>`,
   `c6 janela`). Closed (1000 ms / 0), the lock starts being released between
   beacons.
2. **An active SDIO link forbids light sleep.** esp_hosted has the C6 half of
   "host power save": the P4 notifies (`POWER_SAVE_ON` via the SDIO
   interrupt), the slave is torn down, and traffic for the host brings the
   slave back up and pulses IO2 (a wire to the P4's G14). Light sleep is enabled
   in the C6's PM, but held off by the `enlace_p4` lock while the link is up.

**Measured (2026-10-18, screen off, Wi-Fi associated):** with the link torn down
the C6 spent 71 s of ~80 s in light sleep (3349 entries). Coming back takes 0.4
to 0.6 s, without resetting the C6: the association, the IP and the connections
carry on.

### How it comes back

- **The C6 calls:** a frame for the P4 (unicast: MQTT, TCP) → esp_hosted brings
  the slave back up and pulses IO2.
- **The P4 calls:** there is no wire from the P4 to the C6. With the slave torn
  down, the SDIO CMD line (P4 G13 = C6 GPIO18) sits idle high; the P4 pulls it
  low for 2 ms and the low level wakes the C6 (armed 500 ms after teardown: the
  P4 itself still sends commands down the line while tearing down its side).
  What triggers it: any transmission from the IP stack (`esp_wifi_internal_tx`,
  the frame is lost and TCP resends it) and any RPC (`eh_host_feat_rpc_*`,
  which waits for the link to come back up instead of failing - without this
  every RPC waited 5 s and failed).

### The filter on the C6

Without a filter the sleep did not last: the router sends ~2 ARPs a second, and
each frame woke the P4. With the host asleep, the C6 handles it by itself: it
answers ARP for the P4's IP (`C6_AJ_IP`, sent before sleeping) and drops all
broadcast/multicast and other devices' ARP. `c6 sleep` (`c6 sono`) shows the
counters.

### The three traps of the P4 coming back up

- **The reset.** When the SDIO comes up, esp_hosted resets the C6 (which would
  lose the association), except when it thinks the host has come back from deep
  sleep (`eh_host_port_wakeup_reason_get`, which looks at
  `esp_reset_reason()`). A link-time redirect (`main/CMakeLists.txt`, `--wrap`)
  answers "I came back via GPIO" during the re-setup.
  `CONFIG_ESP_HOSTED_HOST_FEAT_POWER_SAVE=y` enables that branch; **without**
  the auto-initialisation, which would arm an interrupt on G14 that restarts
  the P4.
- **Frames silently dropped.** The Wi-Fi feature's deinit closes RX
  "admission" (`eh_host_wifi.c`); what reopens it is the StaConnected event,
  which the C6 resends with the "host woke up" notice - before the feature has
  registered its handlers, so it was lost. The ping went out over the air and
  the reply never reached the stack. The re-setup reopens admission and marks
  the netif as started.
- **The stalled RPC.** See "The P4 calls", above.

### Automatic mode and what it costs

`c6 sleep auto on` (`c6 sono auto ligar`; setting `c6_sono`, **off by default**):
with the screen off, the P4 3 s without transmitting and Bluetooth off, the
ESP-NOW window closes and the link is torn down; turning the screen on brings
everything back. The price: **with the screen off ESP-NOW does not listen** (a
message from another device during that time is lost), and Bluetooth has to be
off (the P4's NimBLE stack talks to the C6 over the same link). A failed re-setup
turns the mode off and logs it.

Commands: `c6 suspend`/`c6 wake` (`c6 dormir`/`c6 acordar`, by hand), `c6 sleep`
(state and counters on both sides), `c6 locks` (the locks and the time in each
mode of the C6's PM, now and 5 s after the last "host went to sleep"),
`c6 window`.

## Who uses the radio on the P4

| Service | Message | Used by |
|---|---|---|
| `tab5_farejo` | `C6_MSG_FAREJO` (batches) | Radar (WiFi targets), console `sniff` (`farejo`) |
| `tab5_csi` | `C6_MSG_CSI` | Radar (motion), console `radio csi` |
| console | `C6_MSG_FTM`, `C6_MSG_154` | tests `radio ftm` / `radio 154` |

**One listener per message.** The host's peer_data dispatcher
(`eh_host_feat_peer_data_dispatch.c`) keeps one entry per `msg_id`, and
`eh_host_peer_data_register` with an already registered `msg_id` **replaces** the
listener. That is why each message has a single owner (a service), and anyone
else who wants the data asks the service (`tab5_farejo_ouvinte`,
`tab5_csi_ouvinte`).

**Motion from CSI** (`main/services/tab5_csi.cpp`). It uses the 52 valid
subcarriers of the L-LTF (present in 11g and HT). For each frame: amplitude per
subcarrier, normalised by the frame's average (the C6's AGC changes the gain
from frame to frame); mean deviation against the channel's own moving average
(~1 s). Each place's "still" baseline is learnt in the first ~3 s and follows
slowly; the level comes from the ratio between the current deviation and the
still baseline (2× light, 4× strong, with hysteresis). The Tab5 itself moving
changes the CSI as much as a person does: the user notifies it
(`tab5_csi_aparelho_mexendo`), and the reference starts over when it stops.

**WiFi targets on the Radar.** Each record in a sniffer batch (100 ms) goes into
the Radar's queue with the time of the middle of the batch and the average RSSI
for that MAC, and is matched against the gyroscope heading like a BLE packet.
The sniffer does not hop channels: while associated, changing channel drops the
WiFi, and in radio-only mode the channel is ESP-NOW's.

## P4 ↔ C6 contract

It lives in `compartilhado/tab5_c6.h`, included by both projects.

- **Ceiling of 8 handlers** for `peer_data` on the C6
  (`EH_CP_FEAT_PEER_DATA_MAX_HANDLERS`), and the table is full (msg_id 1..8).
  A ninth registration would fail silently. A new command from the P4 to the C6
  goes in as a key of `C6_MSG_HOST_AJUSTE` or a sub-command of
  `C6_MSG_HOST_RADIO`, never as a new msg_id. The C6 → P4 direction has no such
  ceiling.
- **Capabilities.** PRONTO (ready) and INFO carry `caps`; each bit (`C6_CAP_CSI`,
  `C6_CAP_FTM`, `C6_CAP_154`...) is only set if the module was compiled in. The
  P4 hides whatever the flashed build did not include.
- **Everything sleeps until the P4 asks**, and CSI, the sniffer and 802.15.4
  switch themselves off if the P4 stops renewing (`C6_RADIO_VALIDADE_MS`,
  `C6_FAREJO_VALIDADE_MS`). FTM is one-shot.
- **Radio callbacks do not talk to the host.** `eh_cp_feat_peer_data_send`
  can block on the RPC; called from inside a Wi-Fi callback, it locks up the
  whole stack. The callbacks only write into a 12 KB **ring buffer**
  (`s_saida`), and the `c6saida` task sends. A ring and not a queue of fixed
  items: messages range from 7 B to ~1 KB, and with a queue each item would cost
  the maximum size and be assembled on the Wi-Fi task's stack. In the ring the
  producer reserves the exact size (`xRingbufferSendAcquire`) and writes
  straight into it. The 802.15.4 callback runs in the radio ISR and uses
  `xRingbufferSendFromISR`. A full ring drops, and `sniff info` shows how many
  messages were lost.

## Flashing the C6

The C6 has no USB of its own: it is flashed through the board's programming
connector, with the Tab5 **switched off** (the programmer powers the C6). The
step-by-step guide, the backup and going back to the factory firmware are in
the [README](../README.md#esp32-c6-firmware-optional).

```bash
c6/gravar.sh   # finds the port by itself; PORTA=/dev/xxx c6/gravar.sh picks another
```

The firmware has two safety nets: the bootloader's rollback and a 120 s grace
period in the app itself — if the P4 does not confirm the link within that time,
the C6 points the boot back at the previous image.

**Changing only the P4 side does not require reopening the device.** The CSI fix
is of that kind. Only changes in `c6/` need the programmer.

### When touching the C6's `sdkconfig.defaults`

An existing `sdkconfig` takes precedence over `sdkconfig.defaults`: changing
a key in the defaults does not change an already generated `sdkconfig`. Delete
`c6/sdkconfig` before building and check with `grep` that the key appeared.
An unknown key in the defaults is discarded without warning.

## Checklist after flashing

1. `sniff info` (`farejo info`) — `contrato v3` (contract v3), capabilities
   `CSI s | FTM s | 802.15.4 s`.
2. `ping` — router and internet responding.
3. `radio csi 10` — close to 100 records (1 every 100 ms), count per
   format (HT/HE with a Wi-Fi 4/6 router), none truncated; in `sniff info`,
   CSI `OK` and 0 messages dropped. With an 802.11n router, each HT20 record
   is 256 B (L-LTF 128 + HT-LTF 128).
4. `radio ftm` — session completed (status 0 to 6, from the driver), or the name
   of the error that prevented it starting. On a C6 v0.0/v0.1,
   `ESP_ERR_NOT_SUPPORTED` is expected (see
   [FTM](#ftm-distance-measurement--a-chip-limitation)).
5. `radio 154 15 20` — `WiFi ANTES` (before), `DURANTE` (during) and `DEPOIS`
   (after) at 4/4; `sniff info` afterwards says 802.15.4 "dormindo" (sleeping).
6. `ping` again — Wi-Fi carries on normally after all of it.
7. `sniff on` / `sniff` / `sniff off` — MACs showing up in batches.
