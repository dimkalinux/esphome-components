# ESPHome Components

A collection of custom [ESPHome](https://esphome.io/) components for ESP32 devices.

## Components

### `espnow_failover`

Automatic master/backup failover for ESP32 devices using [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now).

Multiple ESP32 nodes communicate over ESP-NOW via periodic heartbeats. The node with the lowest MAC address is automatically elected as **master**. If the master goes offline, a backup is promoted within 30 seconds.

> [!TIP]
> If your nodes are already connected to WiFi, consider [`udp_failover`](#udp_failover) instead. It keeps the same election logic but sends heartbeats over WiFi/UDP, which lets the radio stay in modem-sleep — so the device runs noticeably cooler than ESP-NOW, which holds the receiver continuously awake.

**Features:**

- Connectionless peer discovery via broadcast heartbeats
- Deterministic master election based on lowest MAC address
- Automatic failover when the master becomes unreachable
- Zero configuration — no hardcoded addresses or roles
- Multiple independent failover groups on the same network via configurable `group_id`

#### Usage

Add this repository as an external component source in your ESPHome YAML config:

```yaml
external_components:
  - source: github://dimkalinux/esphome-components
    components: [espnow_failover]

espnow_failover:
  id: failover
  group_id: "pump"

binary_sensor:
  - platform: espnow_failover
    is_master:
      name: "Is Master"
```

Deploy the same configuration to two or more ESP32 devices. All nodes that should participate in the same failover group **must use the same `group_id`**. Nodes with different group IDs will ignore each other.

#### Configuration Variables

| Name | Type | Description |
|------|------|-------------|
| `group_id` | **Required**, `string` | A string identifier (2–8 characters) for the failover group. Only nodes with the same `group_id` will discover each other and participate in master election. |

#### Binary Sensor

| Name | Description |
|------|-------------|
| `is_master` | **Optional.** `true` when this node is the elected master, `false` when it is a backup. Uses `connectivity` device class. |

All options from [Binary Sensor](https://esphome.io/components/binary_sensor/) are supported.

#### How It Works

1. On startup, each node initializes ESP-NOW and begins broadcasting heartbeat messages every **10 seconds**.
2. Heartbeats contain the sender's MAC address, current role, and uptime.
3. Each node tracks all known peers. If a peer hasn't sent a heartbeat within **30 seconds**, it is considered dead and pruned.
4. After each heartbeat cycle, the node with the **lowest MAC address** among all live peers (including itself) becomes the master.

#### Checking Role in Lambdas

You can also use the component's `is_master()` method directly in ESPHome lambdas:

```yaml
espnow_failover:
  id: failover
  group_id: "pump"

switch:
  - platform: template
    name: "Master-only Switch"
    turn_on_action:
      - if:
          condition:
            lambda: 'return id(failover).is_master();'
          then:
            - logger.log: "I am master, executing action"
```

### `udp_failover`

Automatic master/backup failover for ESP32 devices over the **existing WiFi network**, using UDP multicast heartbeats.

It behaves exactly like [`espnow_failover`](#espnow_failover) — lowest MAC address wins, automatic promotion of a backup within 30 seconds — but the heartbeats travel as UDP multicast packets over WiFi instead of ESP-NOW. Because it relies on the WiFi link the device already maintains, the radio can stay in its normal `LIGHT` modem-sleep (packets are delivered on the AP's DTIM cycle) instead of keeping the receiver continuously powered. In practice this makes the device run **significantly cooler than the ESP-NOW transport**.

**Features:**

- Same deterministic master election as `espnow_failover` (lowest MAC address wins)
- Heartbeats over UDP multicast on the existing WiFi link — no always-on radio receiver, so the device stays cool
- Automatic failover when the master becomes unreachable (30 s timeout)
- Split-brain-safe startup: a node listens before it acts, so several devices powering on at the same time elect a single master *before* any of them perform a master-only action
- Multiple independent failover groups on the same network via configurable `group_id`
- Zero configuration — no hardcoded addresses or roles

**Requirements:**

- ESP32 on an ESP-IDF (or lwIP-sockets) build — the component uses ESPHome's `network` and `socket` components, which are loaded automatically.
- All participating nodes on the **same subnet**, with an access point/router that forwards multicast (the default on virtually all home routers).
- Keep WiFi `power_save_mode` at its default (`LIGHT`) — that is what keeps the device cool. Setting it to `NONE` defeats the purpose.

#### Usage

```yaml
external_components:
  - source: github://dimkalinux/esphome-components
    components: [udp_failover]

udp_failover:
  id: failover
  group_id: "pump"

binary_sensor:
  - platform: udp_failover
    is_master:
      name: "Is Master"
```

Deploy the same configuration to two or more ESP32 devices on the same WiFi network. All nodes that should participate in the same failover group **must use the same `group_id`** (and the same `multicast_address`/`port` if you change them). Nodes with a different `group_id` ignore each other even if they share the multicast group.

#### Configuration Variables

| Name | Type | Description |
|------|------|-------------|
| `group_id` | **Required**, `string` | A string identifier (2–8 characters) for the failover group. Only nodes with the same `group_id` participate in the same master election. |
| `multicast_address` | **Optional**, `IPv4` | The multicast group used for heartbeats. Defaults to `239.255.84.79` (an administratively-scoped, link-local group). Change it only if it conflicts with other traffic on your network. |
| `port` | **Optional**, `port` | UDP port for heartbeats. Defaults to `14479`. |

#### Binary Sensor

| Name | Description |
|------|-------------|
| `is_master` | **Optional.** `true` when this node is the elected master, `false` when it is a backup or still in its startup hold-down. Uses `connectivity` device class. |

All options from [Binary Sensor](https://esphome.io/components/binary_sensor/) are supported.

#### How It Works

1. Once WiFi is connected, each node joins the multicast group and **immediately announces itself**, then broadcasts a heartbeat every **10 seconds**. Heartbeats carry the sender's MAC address, current role, and uptime.
2. For a short **startup hold-down** (~3 seconds) the node stays passive — `is_master()` returns `false` — while it listens. This guarantees that devices powering on at the same time discover each other and agree on a single master *before* any of them acts, avoiding a split-brain where two nodes both run a master-only action at boot.
3. When a node hears a previously-unknown peer it answers right away (**reply-on-discovery**), so a freshly-booted device is learned within a round-trip instead of waiting up to a full heartbeat interval.
4. Each node tracks all known peers. A peer that hasn't been heard from within **30 seconds** is considered dead and pruned.
5. The node with the **lowest MAC address** among all live peers (including itself) becomes the master. The worst case during a transition is a single skipped action — never a duplicated one.

#### Checking Role in Lambdas

As with `espnow_failover`, you can call `is_master()` directly in lambdas:

```yaml
udp_failover:
  id: failover
  group_id: "pump"

switch:
  - platform: template
    name: "Master-only Switch"
    turn_on_action:
      - if:
          condition:
            lambda: 'return id(failover).is_master();'
          then:
            - logger.log: "I am master, executing action"
```

## Choosing a transport

| | [`espnow_failover`](#espnow_failover) | [`udp_failover`](#udp_failover) |
|---|---|---|
| Transport | ESP-NOW (link-layer) | UDP multicast over WiFi |
| Requires WiFi | No | Yes (same subnet) |
| Radio power | Keeps the receiver continuously awake — **runs hotter** | Works within WiFi modem-sleep — **runs cooler** |
| Works without an AP | Yes | No |
| Election / failover | Lowest MAC wins, 30 s timeout | Identical |

Use `espnow_failover` when the nodes are not (always) on WiFi or you want AP-independent coordination. Use `udp_failover` when the nodes are already on WiFi and you care about keeping them cool — it is a drop-in replacement with the same `is_master()` API.

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
