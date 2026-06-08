#pragma once

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// This transport uses the BSD/lwIP socket abstraction shared by ESPHome's
// `socket` component. It is available on the ESP-IDF and lwIP-sockets builds
// (i.e. all ESP32 targets). Guard the socket-specific bits so the component
// still compiles (as a no-op) on platforms without it.
#if defined(USE_NETWORK) && (defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS))
#define USE_UDP_FAILOVER_SOCKETS
#endif

#ifdef USE_UDP_FAILOVER_SOCKETS
#include "esphome/components/socket/socket.h"
#endif

#include <cstring>
#include <map>
#include <memory>
#include <string>

namespace esphome
{
    namespace udp_failover
    {

        static const char *const TAG = "udp_failover";

        static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
        static const uint32_t FAILOVER_TIMEOUT_MS = 30000;
        // After the socket is ready, listen this long before we are allowed to act
        // as master. Prevents a split-brain double-action at boot when several
        // devices start at the same time (e.g. power restored simultaneously).
        static const uint32_t STARTUP_HOLD_MS = 3000;
        // When we discover a brand-new peer, answer with a heartbeat (instead of
        // waiting up to HEARTBEAT_INTERVAL_MS) so it learns about us within a
        // round-trip — but no more often than this, to avoid reply storms.
        static const uint32_t REPLY_MIN_GAP_MS = 1000;
        // Re-issue the IGMP join on this cadence. With no querier on the segment,
        // an AP/switch doing IGMP snooping ages out its membership entry and
        // silently stops forwarding the group to us; a periodic re-join sends a
        // fresh membership report that keeps the forwarding path alive.
        static const uint32_t MEMBERSHIP_REJOIN_MS = 120000;
        static const uint8_t CHECKSUM_SEED_MASTER = 0xAA;
        static const uint8_t CHECKSUM_SEED_BACKUP = 0x55;

        struct MacAddress
        {
            uint8_t addr[6];

            bool operator<(const MacAddress &other) const { return memcmp(addr, other.addr, 6) < 0; }
            bool operator==(const MacAddress &other) const { return memcmp(addr, other.addr, 6) == 0; }
            bool operator!=(const MacAddress &other) const { return !(*this == other); }
        };

        struct __attribute__((packed)) HeartbeatMessage
        {
            uint16_t group_id;
            uint8_t mac[6];
            uint8_t is_master;
            uint32_t uptime_sec;
            uint8_t checksum;
        };

        struct PeerState
        {
            bool is_master;
            uint32_t last_seen_ms;
        };

        class UdpFailoverComponent : public Component
        {
        public:
            bool is_master() const { return this->active_ && this->i_am_master_; }
            void set_group_id(const std::string &group_id) { this->group_id_ = group_id; this->group_id_hash_ = hash_group_id_(group_id); }
            void set_multicast_address(const std::string &address) { this->multicast_address_ = address; }
            void set_port(uint16_t port) { this->port_ = port; }
            void set_is_master_binary_sensor(binary_sensor::BinarySensor *sensor) { this->is_master_binary_sensor_ = sensor; }

            void setup() override;
            void loop() override;
            void dump_config() override;
            float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

        protected:

            binary_sensor::BinarySensor *is_master_binary_sensor_{nullptr};

            MacAddress my_mac_{};
            std::string group_id_{};
            uint16_t group_id_hash_{0};
            std::string multicast_address_{};
            uint16_t port_{0};
            bool i_am_master_{false};
            bool active_{false};
            uint32_t last_heartbeat_sent_ms_{0};
            uint32_t last_init_attempt_ms_{0};
            uint32_t socket_ready_ms_{0};
            uint32_t last_rejoin_ms_{0};
            bool initialized_{false};

            bool effective_master_() const { return this->active_ && this->i_am_master_; }

            std::map<MacAddress, PeerState> peers_;

#ifdef USE_UDP_FAILOVER_SOCKETS
            std::unique_ptr<socket::Socket> socket_{nullptr};
            struct sockaddr_storage dest_addr_{};
            socklen_t dest_addr_len_{0};

            bool init_socket_();
            void rejoin_multicast_();
            void close_socket_();
#endif

            static uint8_t calculate_checksum_(const HeartbeatMessage &msg);
            static uint16_t hash_group_id_(const std::string &group_id);

            void send_heartbeat_();
            void receive_packets_();
            bool handle_message_(const HeartbeatMessage &msg);
            void evaluate_role_();
            void prune_dead_peers_();
            void log_mac_(const char *prefix, const MacAddress &mac);
            void publish_is_master_state_();
        };

    }
}
