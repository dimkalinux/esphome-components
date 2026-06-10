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

            bool discovered_new = false;

            for (const auto &msg : local_queue)
            {
                MacAddress peer_mac{};
                memcpy(peer_mac.addr, msg.mac, 6);

                if (peer_mac == this->my_mac_)
                    continue;

                bool is_new = this->peers_.find(peer_mac) == this->peers_.end();
                this->peers_[peer_mac] = PeerState{msg.is_master != 0, millis()};
                if (is_new)
                    discovered_new = true;

                this->log_mac_(is_new ? "New peer" : "Heartbeat from", peer_mac);
                ESP_LOGD(TAG, "  master=%s, uptime=%us, peers_known=%d",
                         msg.is_master ? "true" : "false", msg.uptime_sec, this->peers_.size());
            }

            if (!local_queue.empty())
            {
                this->evaluate_role_();
            }

            // Answer a newly-discovered peer promptly so it learns about us within
            // a round-trip instead of waiting up to one heartbeat interval. The gap
            // guard stops two devices from echoing each other indefinitely.
            if (discovered_new && (millis() - this->last_heartbeat_sent_ms_) >= REPLY_MIN_GAP_MS)
                this->start_heartbeat_burst_();
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
            bool had_peers = !this->peers_.empty();
            this->prune_dead_peers_();

            MacAddress lowest_mac = this->my_mac_;

            for (const auto &entry : this->peers_)
            {
                if (entry.first < lowest_mac)
                    lowest_mac = entry.first;
            }

            bool should_be_master = (lowest_mac == this->my_mac_);

            if (!should_be_master)
            {
                // A lower-MAC peer is audible again — any pending promotion is moot.
                this->promotion_grace_ = false;
            }
            else if (!this->i_am_master_)
            {
                // Every peer we knew went silent at once. That looks less like all
                // of them dying simultaneously and more like a transient RF problem
                // on our side. Hold the promotion for one full burst cycle; if the
                // master is alive we will hear it and stand down, if it is truly
                // dead we only lose the grace time.
                if (had_peers && this->peers_.empty() && !this->promotion_grace_)
                {
                    this->promotion_grace_ = true;
                    this->promotion_grace_start_ms_ = millis();
                    ESP_LOGW(TAG, "All peers lost at once — suspecting RF trouble, delaying promotion %ums", PROMOTION_GRACE_MS);
                }
                if (this->promotion_grace_)
                {
                    if ((millis() - this->promotion_grace_start_ms_) < PROMOTION_GRACE_MS)
                        return;
                    this->promotion_grace_ = false;
                    ESP_LOGW(TAG, "Promotion grace elapsed with no peer heard — proceeding");
                }
            }

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

        void EspNowFailoverComponent::start_heartbeat_burst_()
        {
            if (this->burst_frames_left_ > 0)
                return; // a burst is already in flight

            this->burst_frames_left_ = BURST_FRAME_COUNT;
            this->last_heartbeat_sent_ms_ = millis();
            ESP_LOGD(TAG, "Heartbeat burst started. I am %s", this->effective_master_() ? "MASTER" : "BACKUP");
            this->send_burst_frame_();
        }

        void EspNowFailoverComponent::send_burst_frame_()
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
                ESP_LOGW(TAG, "Failed to send heartbeat frame: %s", esp_err_to_name(result));
            } else {
                ESP_LOGV(TAG, "Heartbeat frame sent (%u left in burst)", this->burst_frames_left_ - 1);
            }
            this->burst_frames_left_--;
            this->last_burst_frame_ms_ = millis();
        }

        void EspNowFailoverComponent::publish_is_master_state_()
        {
            if (this->is_master_binary_sensor_ != nullptr)
                this->is_master_binary_sensor_->publish_state(this->effective_master_());
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

            // Keep modem sleep active and duty-cycle ESP-NOW reception instead of
            // holding the radio in continuous RX — continuous RX is what made
            // this component run ~10 °C hotter than the UDP transport. The wake
            // window only works if WiFi power save stays on, so force MIN_MODEM
            // even if something else changed it.
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            esp_err_t ps_err = esp_wifi_connectionless_module_set_wake_interval(WAKE_INTERVAL_MS);
            if (ps_err == ESP_OK)
                ps_err = esp_now_set_wake_window(WAKE_WINDOW_MS);
            if (ps_err != ESP_OK)
                ESP_LOGW(TAG, "Connectionless power save setup failed (%s) — radio stays in continuous RX", esp_err_to_name(ps_err));
            else
                ESP_LOGI(TAG, "RX duty-cycled: %ums window per %ums interval", WAKE_WINDOW_MS, WAKE_INTERVAL_MS);

            this->espnow_initialized_ = true;
            this->init_done_ms_ = millis();
            // Stay inactive (neither master nor acting) through the startup hold,
            // so we never act before hearing any peer.
            this->publish_is_master_state_();

            ESP_LOGI(TAG, "ESP-NOW Failover initialized (group_id='%s', hash=0x%04X). Listening before electing.",
                                this->group_id_.c_str(), this->group_id_hash_);

            // Announce ourselves immediately so peers learn about us right away
            // instead of after the first interval.
            this->start_heartbeat_burst_();
        }

        void EspNowFailoverComponent::loop()
        {
            if (!this->espnow_initialized_)
                return;

            this->process_receive_queue_();

            uint32_t now = millis();

            // Continue an in-flight burst: one frame per gap until the burst has
            // spanned a full receiver wake interval.
            if (this->burst_frames_left_ > 0 && (now - this->last_burst_frame_ms_) >= BURST_FRAME_GAP_MS)
                this->send_burst_frame_();

            // While in the startup hold, keep announcing back-to-back instead of
            // trusting a single burst: if the boot announce is lost, an existing
            // master never replies and we would elect ourselves alongside it.
            if (!this->active_ && (now - this->last_heartbeat_sent_ms_) >= REPLY_MIN_GAP_MS)
                this->start_heartbeat_burst_();

            // Stay passive until we have listened for the hold-down window, then
            // commit to the role we elected from whatever peers we heard.
            if (!this->active_ && (now - this->init_done_ms_) >= STARTUP_HOLD_MS)
            {
                this->active_ = true;
                this->evaluate_role_();
                this->publish_is_master_state_();
                ESP_LOGI(TAG, "Startup hold elapsed — now active as %s", this->effective_master_() ? "MASTER" : "BACKUP");
            }

            if ((now - this->last_heartbeat_sent_ms_) >= HEARTBEAT_INTERVAL_MS)
            {
                this->evaluate_role_();
                this->start_heartbeat_burst_();
            }
        }

    }
}
