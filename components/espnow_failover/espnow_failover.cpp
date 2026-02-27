#include "espnow_failover.h"

namespace esphome
{
    namespace espnow_failover
    {
        EspNowFailoverComponent *EspNowFailoverComponent::instance_ = nullptr;
        EspNowFailoverComponent *EspNowFailoverComponent::instance() { return instance_; }

        uint8_t EspNowFailoverComponent::calculate_checksum_(const HeartbeatMessage &msg)
        {
            uint8_t cs = msg.is_master ? CHECKSUM_SEED_MASTER : CHECKSUM_SEED_BACKUP;
            cs ^= (msg.group_id & 0xFF);
            cs ^= ((msg.group_id >> 8) & 0xFF);

            for (int i = 0; i < 6; i++)
                cs ^= msg.mac[i];
            cs ^= (msg.uptime_sec & 0xFF);
            return cs;
        }

        uint16_t EspNowFailoverComponent::hash_group_id_(const std::string &group_id)
        {
            uint16_t hash = 0x1505;
            for (char c : group_id)
            {
                hash = ((hash << 5) + hash) ^ static_cast<uint8_t>(c);
            }
            return hash;
        }

        void EspNowFailoverComponent::recv_cb_(const esp_now_recv_info_t *info, const uint8_t *data, int len)
        {
            if (instance_ == nullptr) return;

            instance_->on_receive_(data, len);
        }

        void EspNowFailoverComponent::on_receive_(const uint8_t *data, int len)
        {
            if (len != sizeof(HeartbeatMessage)) return;

            HeartbeatMessage msg;
            memcpy(&msg, data, sizeof(msg));

            if (msg.group_id != this->group_id_hash_) return;
            if (calculate_checksum_(msg) != msg.checksum) return;

            portENTER_CRITICAL(&this->queue_mutex_);
            if (this->receive_queue_.size() < MAX_RECEIVE_QUEUE_SIZE)
            {
                this->receive_queue_.push_back(msg);
            }
            portEXIT_CRITICAL(&this->queue_mutex_);
        }

        void EspNowFailoverComponent::process_receive_queue_()
        {
            std::vector<HeartbeatMessage> local_queue;

            portENTER_CRITICAL(&this->queue_mutex_);
            local_queue.swap(this->receive_queue_);
            portEXIT_CRITICAL(&this->queue_mutex_);

            for (const auto &msg : local_queue)
            {
                MacAddress peer_mac{};
                memcpy(peer_mac.addr, msg.mac, 6);

                if (peer_mac == this->my_mac_)
                    continue;

                this->peers_[peer_mac] = PeerState{msg.is_master != 0, millis()};

                this->log_mac_("Heartbeat from", peer_mac);
                ESP_LOGD(TAG, "  master=%s, uptime=%us, peers_known=%d",
                         msg.is_master ? "true" : "false", msg.uptime_sec, this->peers_.size());
            }

            if (!local_queue.empty())
            {
                this->evaluate_role_();
            }
        }

        void EspNowFailoverComponent::prune_dead_peers_()
        {
            uint32_t now = millis();
            auto it = this->peers_.begin();
            while (it != this->peers_.end())
            {
                if ((now - it->second.last_seen_ms) > FAILOVER_TIMEOUT_MS)
                {
                    this->log_mac_("Peer timed out", it->first);
                    it = this->peers_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void EspNowFailoverComponent::evaluate_role_()
        {
            this->prune_dead_peers_();

            MacAddress lowest_mac = this->my_mac_;

            for (const auto &entry : this->peers_)
            {
                if (entry.first < lowest_mac)
                    lowest_mac = entry.first;
            }

            bool should_be_master = (lowest_mac == this->my_mac_);

            if (should_be_master != this->i_am_master_)
            {
                this->i_am_master_ = should_be_master;
                this->publish_is_master_state_();

                if (should_be_master)
                {
                    this->log_mac_("Lowest MAC is mine — becoming MASTER", this->my_mac_);
                }
                else
                {
                    this->log_mac_("Lower MAC peer exists — becoming BACKUP. Master is", lowest_mac);
                }
            }
        }

        void EspNowFailoverComponent::send_heartbeat_()
        {
            HeartbeatMessage msg{};
            msg.group_id = this->group_id_hash_;
            memcpy(msg.mac, this->my_mac_.addr, 6);
            msg.is_master = this->i_am_master_;
            msg.uptime_sec = millis() / 1000;
            msg.checksum = calculate_checksum_(msg);

            esp_err_t result = esp_now_send(BROADCAST_ADDR, (uint8_t *)&msg, sizeof(msg));
            if (result != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to send heartbeat: %s", esp_err_to_name(result));
            } else {
                ESP_LOGD(TAG, "Heartbeat sent. I am %s", this->i_am_master_ ? "MASTER" : "BACKUP");
            }
        }

        void EspNowFailoverComponent::publish_is_master_state_()
        {
            if (this->is_master_binary_sensor_ != nullptr)
                this->is_master_binary_sensor_->publish_state(this->i_am_master_);
        }

        void EspNowFailoverComponent::log_mac_(const char *prefix, const MacAddress &mac)
        {
            ESP_LOGD(TAG, "%s: %02X:%02X:%02X:%02X:%02X:%02X",
                     prefix, mac.addr[0], mac.addr[1], mac.addr[2],
                     mac.addr[3], mac.addr[4], mac.addr[5]);
        }

        void EspNowFailoverComponent::setup()
        {
            instance_ = this;

            this->receive_queue_.reserve(MAX_RECEIVE_QUEUE_SIZE);

            uint8_t mac_buf[6];
            esp_read_mac(mac_buf, ESP_MAC_WIFI_STA);
            memcpy(this->my_mac_.addr, mac_buf, 6);
            this->log_mac_("My MAC", this->my_mac_);

            this->i_am_master_ = true;

            if (esp_now_init() != ESP_OK)
            {
                ESP_LOGE(TAG, "ESP-NOW init failed!");
                this->mark_failed();
                return;
            }

            esp_now_register_recv_cb(recv_cb_);

            esp_now_peer_info_t peer_info = {};
            memcpy(peer_info.peer_addr, BROADCAST_ADDR, 6);
            peer_info.channel = 0;
            peer_info.encrypt = false;

            if (esp_now_add_peer(&peer_info) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to add broadcast peer");
                this->mark_failed();
                return;
            }

            this->espnow_initialized_ = true;
            this->last_heartbeat_sent_ms_ = millis();
            this->publish_is_master_state_();

            ESP_LOGI(TAG, "ESP-NOW Failover initialized (group_id='%s', hash=0x%04X). Starting as MASTER (no peers yet).",
                                this->group_id_.c_str(), this->group_id_hash_);
        }

        void EspNowFailoverComponent::loop()
        {
            if (!this->espnow_initialized_)
                return;

            this->process_receive_queue_();

            uint32_t now = millis();

            if ((now - this->last_heartbeat_sent_ms_) >= HEARTBEAT_INTERVAL_MS)
            {
                this->evaluate_role_();
                this->send_heartbeat_();
                this->last_heartbeat_sent_ms_ = now;
            }
        }

    }
}
