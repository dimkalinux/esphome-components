#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <cstring>
#include <map>

namespace esphome
{
    namespace espnow_failover
    {

        static const char *const TAG = "espnow_failover";

        static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
        static const uint32_t FAILOVER_TIMEOUT_MS = 30000;
        static const uint8_t BROADCAST_ADDR[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        static const uint8_t MAX_RECEIVE_QUEUE_SIZE = 10;

        struct MacAddress
        {
            uint8_t addr[6];

            bool operator<(const MacAddress &other) const { return memcmp(addr, other.addr, 6) < 0; }
            bool operator==(const MacAddress &other) const { return memcmp(addr, other.addr, 6) == 0; }
            bool operator!=(const MacAddress &other) const { return !(*this == other); }
        };

        struct HeartbeatMessage
        {
            uint8_t mac[6];
            bool is_master;
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
            bool is_master() const { return this->i_am_master_; }

            void setup() override;
            void loop() override;
            float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

            static EspNowFailoverComponent *instance();

        protected:
            MacAddress my_mac_{};
            bool i_am_master_{false};
            uint32_t last_heartbeat_sent_ms_{0};
            bool espnow_initialized_{false};

            std::map<MacAddress, PeerState> peers_;

            static uint8_t calculate_checksum_(const HeartbeatMessage &msg);
            void send_heartbeat_();
            void evaluate_role_();
            void on_receive_(const uint8_t *data, int len);
            void prune_dead_peers_();
            void log_mac_(const char *prefix, const MacAddress &mac);

            static EspNowFailoverComponent *instance_;
            static void recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len);
        };

    }
}

