# Meshtastic integration

A map of what the Meshtastic protocol offers a client (the Tab5's role towards
the node) and of what M5tabOS uses or could use. Checked against the
generated `.pb.h` files of the firmware (`src/mesh/generated/meshtastic/`) and
`AdminModule.cpp`.

## The radio's node ceiling

How many nodes a radio keeps is a **firmware compile-time constant**
(`MAX_NUM_NODES`, `src/mesh/mesh-pb-constants.h`), not a setting:

| Firmware | ESP32-S3 (Heltec V3, T-Beam S3) | Generic ESP32, C3, **C6 (Unit C6L)**, nRF52840 |
|---|---|---|
| up to 2.7.x | 100 / 200 / 250 depending on flash (4 / 8 / 16 MB) | **100** |
| 2.8.0+ | the same | **120** + a "warm tier" (identity and key of evicted nodes) |

A Heltec V3 with 8 MB of flash keeps 200 and the Unit C6L keeps 100: the same
nodes heard, lists of different sizes. The "nodes online/total" of
`LocalStats` is the radio's view and stops at its ceiling. Updating the C6L to
2.8 raises the ceiling to 120; beyond that, only outside the radio.

**Outside the radio is where the Tab5 wins.** The service's table (256 slots,
PSRAM) also learns from live packets (NODEINFO, POSITION, TELEMETRY, text),
not only from the dump. With its own NodeDB on the card (every field, not just
the name), the Tab5 remembers more nodes than the radio can keep.
`MyNodeInfo.nodedb_count` (field 15) says how many the radio has, to show both
numbers side by side.

## How the client talks to the node

- **FromRadio** (node to us): `1 id, 2 packet, 3 my_info, 4 node_info,
  5 config, 6 log_record, 7 config_complete_id, 8 rebooted, 9 moduleConfig,
  10 channel, 11 queueStatus, 13 metadata, 14 mqttClientProxyMessage,
  15 fileInfo, 16 clientNotification, 17 deviceuiConfig, 19 region_presets`.
- **ToRadio** (us to the node): `1 packet, 3 want_config_id, 4 disconnect,
  7 heartbeat`.
- **Administration** is an ordinary `MeshPacket`: destination = the node
  itself, port `ADMIN_APP` (6), payload = `AdminMessage`. Without `from`
  (= 0), the firmware treats it as LOCAL admin from a connected client and
  **does not ask for a session key** (`AdminModule.cpp`, the `mp.from == 0`
  branch). The key (`session_passkey`) is only required to administer a
  REMOTE node.

## AdminMessage - what can be done

| Field | Operation | Notes |
|---|---|---|
| 1/2 | `get_channel_request` / response | one channel per request |
| 3/4 | `get_owner_request` / response (User) | |
| 5/6 | `get_config_request` (type) / response | the dump already brings them all |
| 7/8 | `get_module_config_request` / response | likewise |
| 12/13 | `get_device_metadata` | firmware, capabilities, hardware |
| 32 | `set_owner` (User) | long/short name, `is_licensed`, `is_unmessagable` |
| 33 | `set_channel` (Channel) | name, PSK, role, MQTT uplink/downlink, position precision |
| 34 | `set_config` (Config) | **replaces the WHOLE section** (see below) |
| 35 | `set_module_config` | likewise, per module |
| 38 | `remove_by_nodenum` | removes a node from the radio's NodeDB |
| 39/40 | `set/remove_favorite_node` | **in use** (star) |
| 41/42 | `set/remove_fixed_position` | fixed position - can come from the Tab5 GPS |
| 43 | `set_time_only` | sets the node's clock |
| 47/48 | `set/remove_ignored_node` | block a node |
| 49 | `toggle_muted_node` | mute a node |
| 64/65 | `begin/commit_edit_settings` | transaction: several changes, one write |
| 97 | `reboot_seconds` | reboot |
| 98 | `shutdown_seconds` | shut down |
| 99 / 94 | `factory_reset_config` / `factory_reset_device` | |
| 100 | `nodedb_reset` | empty the radio's NodeDB |

### Changing settings without modelling the whole protobuf

`set_config` writes the whole section (`config.lora = c.payload_variant.lora`):
sending only the changed field zeroes the others. The way out that keeps the
service's parser small:

1. Keep the **raw bytes** of each section received in the dump (FromRadio 5
   and 9).
2. To change something, send the stored bytes **plus** the new field at the
   end. In protobuf, a repeated scalar field takes its last occurrence; the
   firmware's nanopb decodes it that way. This does not work for `repeated`
   fields (e.g. `ignore_incoming`), which would accumulate.
3. Wrap it in `begin_edit_settings` … `commit_edit_settings`. The firmware
   writes and, if the section needs it, **reboots by itself**; `rebooted`
   (FromRadio 8) tells us, and the service asks for the configuration again.

### Sections and the fields that matter

- **device** (Config 1): `role` (CLIENT, CLIENT_MUTE, ROUTER, REPEATER,
  TRACKER, SENSOR, CLIENT_HIDDEN, ROUTER_LATE, CLIENT_BASE…),
  `rebroadcast_mode`, `node_info_broadcast_secs`, `tzdef`,
  `led_heartbeat_disabled`.
- **position** (2): `position_broadcast_secs`, `smart_enabled`,
  `fixed_position`, `gps_mode` (**must stay NOT_PRESENT on a C6L on Grove**:
  the GPS takes the same pins as the serial link), `broadcast_smart_minimum_*`.
- **power** (3): `is_power_saving`, `ls_secs`, `min_wake_secs`.
- **network** (4): `wifi_enabled`, `wifi_ssid`, `wifi_psk`, `ntp_server` -
  the C6L's own Wi-Fi, which feeds its MQTT.
- **display** (5): the C6L screen (`screen_on_secs`, `flip_screen`, `units`).
- **lora** (6): `region` (ANZ = 6, BR_902 = 26), `modem_preset`
  (LONG_FAST = 0 …), `hop_limit`, `tx_enabled`, `tx_power`, `channel_num`,
  `sx126x_rx_boosted_gain`, `ignore_mqtt`, `config_ok_to_mqtt`.
- **bluetooth** (7): `enabled`, `mode` (random/fixed/no PIN), `fixed_pin`.
- **security** (8): keys, `admin_key` (up to 3), `serial_enabled`,
  `admin_channel_enabled`. **Do not touch** without knowing: it locks you out
  of the node.

ModuleConfig: `1 mqtt, 2 serial, 3 external_notification, 4 store_forward,
5 range_test, 6 telemetry, 7 canned_message, 9 remote_hardware,
10 neighbor_info, 11 ambient_lighting, 12 detection_sensor, 13 paxcounter,
14 statusmessage, 15 traffic_management, 17 mesh_beacon`. The useful ones
here: **mqtt** (on, server, per-channel uplink/downlink, map), **telemetry**
(intervals), **neighbor_info**, **store_forward**, **range_test**.

**Serial (ModuleConfig 2) is the rope that ties us to the node**: changing
`mode`, pins or baud drops the Grove link. It stays out of editing.

## Application ports

In use: `1 TEXT, 3 POSITION, 4 NODEINFO, 5 ROUTING, 6 ADMIN, 67 TELEMETRY`.
Useful to integrate: `8 WAYPOINT` (map points), `70 TRACEROUTE` (the route to
a node, hop by hop, with the SNR of each), `71 NEIGHBORINFO` (each node's
direct neighbours - the mesh graph), `65 STORE_FORWARD` (history kept by a
router), `66 RANGE_TEST`, `73 MAP_REPORT`, `12 KEY_VERIFICATION`.

## Other messages the node sends that are lost today

- `metadata` (13): only the firmware is read; it also carries `hasWifi`,
  `hasBluetooth`, `role`, `hw_model`, `hasPKC`, `excluded_modules`.
- `my_info` (3): only the number; it carries `reboot_count`, `nodedb_count`,
  `pio_env`, `firmware_edition`.
- `clientNotification` (16): firmware notices for the user ("favourites
  full", duplicate key, weak key). They should become status-bar notifications.
- `log_record` (6): the node's log, for diagnostics.
- `NodeInfo` 10–13: `is_favorite` (in use), `is_ignored`,
  `is_key_manually_verified`, `is_muted`.

## What is already implemented

- Phases 1, 2 and 5 below: `services/tab5_mesh_admin.{hpp,cpp}` and the
  "Node" page of the Mesh app; the Tab5's NodeDB in `nodedb.bin`.
- Checked against firmware 2.5, 2.7 and current: field numbers, no key for
  local admin, and the reboot on `commit_edit_settings`.
- `testes/teste_mesh_admin.py` proves the encoding with the official
  protobufs.

## Internet mode (MQTT) and the automatic server

With no node, the Tab5 talks to the mesh over Wi-Fi through an MQTT server
(Settings > Meshtastic > Internet). The list of servers is the user's own.
When the choice is **Automatic** and the node has not taught one, the server
and topic root now come from the time zone (`padrao_regional` in
`services/tab5_mesh_mqtt.cpp`):

| Time zone | Server | Root |
|---|---|---|
| UTC-3 (Brazil) | `platform.meshbrasil.com` | `meshdev` (the original default) |
| UTC+9:30 to UTC+13 (Australia, New Zealand), and the Perth region | `mqtt.meshtastic.org` | `msh/ANZ` |
| UTC+9 | `mqtt.meshtastic.org` | `msh/JP` |
| UTC+5:30 | `mqtt.meshtastic.org` | `msh/IN` |
| UTC-1 to UTC+3 (Europe) | `mqtt.meshtastic.org` | `msh/EU_868` |
| UTC-10 to UTC-3:30 (North America) | `mqtt.meshtastic.org` | `msh/US` |
| anything else | `mqtt.meshtastic.org` | `msh` |

## Proposed roadmap

1. **See the node** (read only, no risk): keep Config/ModuleConfig/
   metadata/my_info from the dump; a "Node" page with firmware, hardware,
   region, preset, role, hops, power, MQTT, Bluetooth, position; the radio's
   nodes (`nodedb_count`) next to the Tab5's; `clientNotification` becomes a
   notice.
2. **Safe settings**, one tap and the node writes and reboots: long/short
   name, role, `hop_limit`, `tx_power`, position/NodeInfo/telemetry intervals,
   MQTT (on, uplink/downlink), Bluetooth (on, fixed PIN), fixed position from
   the Tab5 GPS, setting the node's time.
3. **High-impact settings**, with confirmation: region, modem preset,
   channels (create, rename, PSK, role, delete), reboot, shut down, empty the
   NodeDB, remove/ignore/mute a node.
4. **Mesh**: traceroute from the node card, neighbours (NeighborInfo),
   waypoints, range test.
5. **Own NodeDB on the card**: every field of every node seen, beyond the
   radio's ceiling.
