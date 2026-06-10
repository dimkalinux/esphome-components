#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace esphome
{
    namespace espnow_failover
    {

        static const char *const TAG = "espnow_failover";

        static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
        static const uint32_t FAILOVER_TIMEOUT_MS = 30000;
        // After ESP-NOW is ready, listen this long before we are allowed to act
        // as master. Prevents a split-brain double-action at boot when several
        // devices start at the same time (e.g. power restored simultaneously).
        static const uint32_t STARTUP_HOLD_MS = 3000;
        // When we discover a brand-new peer, answer with a heartbeat (instead of
        // waiting up to HEARTBEAT_INTERVAL_MS) so it learns about us within a
        // round-trip — but no more often than this, to avoid reply storms.
        static const uint32_t REPLY_MIN_GAP_MS = 1000;
        // When every known peer disappears at once, suspect a transient RF
        // problem rather than simultaneous peer death: delay self-promotion this
        // long so a live master gets a full heartbeat interval (one burst) to be
        // heard again before we act alongside it.
        static const uint32_t PROMOTION_GRACE_MS = 12000;
        // Connectionless-module power save: instead of holding the radio in
        // continuous RX (which makes a naive ESP-NOW receiver run ~10 °C
        // hotter), the radio listens only WAKE_WINDOW_MS out of every
        // WAKE_INTERVAL_MS, TSF-aligned when associated. Senders must repeat
        // each heartbeat across one full interval so a copy lands in the window.
        static const uint16_t WAKE_INTERVAL_MS = 1000;
        static const uint16_t WAKE_WINDOW_MS = 110;
        // Heartbeats are sent as bursts: one frame every BURST_FRAME_GAP_MS,
        // BURST_FRAME_COUNT times, spanning >= one wake interval. The gap (90)
        // being smaller than the window (110) guarantees at least one frame
        // falls inside the receiver's listen window wherever it sits.
        static const uint32_t BURST_FRAME_GAP_MS = 90;
        static const uint8_t BURST_FRAME_COUNT = 12;
        static const uint8_t BROADCAST_ADDR[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static const uint8_t MAX_RECEIVE_QUEUE_SIZE = 10;
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

        class EspNowFailoverComponent : public Component
        {
        public:
            bool is_master() const { return this->active_ && this->i_am_master_; }
            void set_group_id(const std::string &group_id) { this->group_id_ = group_id; this->group_id_hash_ = hash_group_id_(group_id); }
            void set_is_master_binary_sensor(binary_sensor::BinarySensor *sensor) { this->is_master_binary_sensor_ = sensor; }

            void setup() override;
            void loop() override;
            float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

            static EspNowFailoverComponent *instance();

        protected:

            binary_sensor::BinarySensor *is_master_binary_sensor_{nullptr};

            MacAddress my_mac_{};
            std::string group_id_{};
            uint16_t group_id_hash_{0};
            bool i_am_master_{false};
            bool active_{false};
            uint32_t last_heartbeat_sent_ms_{0};
            uint32_t init_done_ms_{0};
            bool espnow_initialized_{false};
            bool promotion_grace_{false};
            uint32_t promotion_grace_start_ms_{0};
            uint8_t burst_frames_left_{0};
            uint32_t last_burst_frame_ms_{0};

            bool effective_master_() const { return this->active_ && this->i_am_master_; }

            std::map<MacAddress, PeerState> peers_;

            std::vector<HeartbeatMessage> receive_queue_;
            portMUX_TYPE queue_mutex_ = portMUX_INITIALIZER_UNLOCKED;

            static uint8_t calculate_checksum_(const HeartbeatMessage &msg);
            static uint16_t hash_group_id_(const std::string &group_id);

            void start_heartbeat_burst_();
            void send_burst_frame_();
            void evaluate_role_();
            void on_receive_(const uint8_t *data, int len);
            void process_receive_queue_();
            void prune_dead_peers_();
            void log_mac_(const char *prefix, const MacAddress &mac);
            void publish_is_master_state_();

            static EspNowFailoverComponent *instance_;
            static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);
        };

    }
}
