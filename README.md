# ESPHome Components

A collection of custom [ESPHome](https://esphome.io/) components for ESP32 devices.

## Components

### `espnow_failover`

Automatic master/backup failover for ESP32 devices using [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now).

Multiple ESP32 nodes communicate over ESP-NOW via periodic heartbeats. The node with the lowest MAC address is automatically elected as **master**. If the master goes offline, a backup is promoted within 30 seconds.

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

sensor:
  - platform: espnow_failover
    peer_count:
      name: "Peer Count"
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

#### Sensor

| Name | Description |
|------|-------------|
| `peer_count` | **Optional.** The number of currently known live peers (excluding self). |

All options from [Sensor](https://esphome.io/components/sensor/) are supported.

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

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

