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

#### Usage

Add this repository as an external component source in your ESPHome YAML config:

```yaml
external_components:
  - source: github://dimkalinux/esphome-components
    components: [espnow_failover]

espnow_failover:
```

Deploy the same configuration to two or more ESP32 devices. The component will handle role assignment automatically.

#### Configuration Variables

This component currently has no user-configurable options beyond the default ESPHome component settings.

#### How It Works

1. On startup, each node initializes ESP-NOW and begins broadcasting heartbeat messages every **10 seconds**.
2. Heartbeats contain the sender's MAC address, current role, and uptime.
3. Each node tracks all known peers. If a peer hasn't sent a heartbeat within **30 seconds**, it is considered dead and pruned.
4. After each heartbeat cycle, the node with the **lowest MAC address** among all live peers (including itself) becomes the master.

#### Checking Role in Lambdas

You can use the component's `is_master()` method in ESPHome lambdas:

```yaml
binary_sensor:
  - platform: template
    name: "Is Master"
    lambda: |-
      return id(failover).is_master();

espnow_failover:
  id: failover
```

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

