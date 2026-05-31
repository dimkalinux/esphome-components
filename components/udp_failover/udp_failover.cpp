#include "udp_failover.h"

#ifdef USE_UDP_FAILOVER_SOCKETS
#include "esphome/components/network/util.h"
#include <cerrno>
#endif

namespace esphome
{
    namespace udp_failover
    {
        uint8_t UdpFailoverComponent::calculate_checksum_(const HeartbeatMessage &msg)
        {
            uint8_t cs = msg.is_master ? CHECKSUM_SEED_MASTER : CHECKSUM_SEED_BACKUP;
            cs ^= (msg.group_id & 0xFF);
            cs ^= ((msg.group_id >> 8) & 0xFF);

            for (int i = 0; i < 6; i++)
                cs ^= msg.mac[i];
            cs ^= (msg.uptime_sec & 0xFF);
            return cs;
        }

        uint16_t UdpFailoverComponent::hash_group_id_(const std::string &group_id)
        {
            uint16_t hash = 0x1505;
            for (char c : group_id)
            {
                hash = ((hash << 5) + hash) ^ static_cast<uint8_t>(c);
            }
            return hash;
        }

        bool UdpFailoverComponent::handle_message_(const HeartbeatMessage &msg)
        {
            MacAddress peer_mac{};
            memcpy(peer_mac.addr, msg.mac, 6);

            // Ignore our own heartbeat (e.g. multicast loopback).
            if (peer_mac == this->my_mac_)
                return false;

            bool is_new = this->peers_.find(peer_mac) == this->peers_.end();
            this->peers_[peer_mac] = PeerState{msg.is_master != 0, millis()};

            this->log_mac_(is_new ? "New peer" : "Heartbeat from", peer_mac);
            ESP_LOGD(TAG, "  master=%s, uptime=%us, peers_known=%d",
                     msg.is_master ? "true" : "false", msg.uptime_sec, this->peers_.size());
            return is_new;
        }

        void UdpFailoverComponent::prune_dead_peers_()
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

        void UdpFailoverComponent::evaluate_role_()
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

        void UdpFailoverComponent::publish_is_master_state_()
        {
            if (this->is_master_binary_sensor_ != nullptr)
                this->is_master_binary_sensor_->publish_state(this->effective_master_());
        }

        void UdpFailoverComponent::log_mac_(const char *prefix, const MacAddress &mac)
        {
            ESP_LOGD(TAG, "%s: %02X:%02X:%02X:%02X:%02X:%02X",
                     prefix, mac.addr[0], mac.addr[1], mac.addr[2],
                     mac.addr[3], mac.addr[4], mac.addr[5]);
        }

        void UdpFailoverComponent::setup()
        {
            get_mac_address_raw(this->my_mac_.addr);
            this->log_mac_("My MAC", this->my_mac_);

            // Stay inactive (neither master nor acting) until the socket is ready
            // and the startup hold-down has elapsed, so we never act before
            // hearing any peer. The socket is opened lazily in loop() once the
            // network is connected.
            this->publish_is_master_state_();

            ESP_LOGI(TAG, "UDP Failover configured (group_id='%s', hash=0x%04X, group=%s:%u). Listening before electing.",
                     this->group_id_.c_str(), this->group_id_hash_, this->multicast_address_.c_str(), this->port_);
        }

#ifdef USE_UDP_FAILOVER_SOCKETS
        bool UdpFailoverComponent::init_socket_()
        {
            this->socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (this->socket_ == nullptr)
            {
                ESP_LOGE(TAG, "Could not create socket");
                return false;
            }

            int enable = 1;
            this->socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

            if (this->socket_->setblocking(false) < 0)
            {
                ESP_LOGE(TAG, "Unable to set non-blocking: errno %d", errno);
                this->socket_ = nullptr;
                return false;
            }

            struct sockaddr_in bind_addr {};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = ESPHOME_INADDR_ANY;
            bind_addr.sin_port = htons(this->port_);
            if (this->socket_->bind((struct sockaddr *) &bind_addr, sizeof(bind_addr)) != 0)
            {
                ESP_LOGW(TAG, "Unable to bind socket on port %u: errno %d", this->port_, errno);
                this->socket_ = nullptr;
                return false;
            }

            struct ip_mreq imreq {};
            imreq.imr_interface.s_addr = ESPHOME_INADDR_ANY;
            if (inet_aton(this->multicast_address_.c_str(), &imreq.imr_multiaddr) == 0)
            {
                ESP_LOGE(TAG, "Invalid multicast address '%s'", this->multicast_address_.c_str());
                this->socket_ = nullptr;
                return false;
            }
            if (this->socket_->setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) < 0)
            {
                ESP_LOGW(TAG, "Failed to join multicast group %s: errno %d", this->multicast_address_.c_str(), errno);
                this->socket_ = nullptr;
                return false;
            }

            // Destination for outgoing heartbeats (the multicast group).
            struct sockaddr_in *dest = (struct sockaddr_in *) &this->dest_addr_;
            dest->sin_family = AF_INET;
            dest->sin_port = htons(this->port_);
            inet_aton(this->multicast_address_.c_str(), &dest->sin_addr);
            this->dest_addr_len_ = sizeof(struct sockaddr_in);

            return true;
        }

        void UdpFailoverComponent::receive_packets_()
        {
            bool received_any = false;
            bool discovered_new = false;
            HeartbeatMessage msg;

            for (;;)
            {
                ssize_t len = this->socket_->read(&msg, sizeof(msg));
                if (len <= 0)
                    break;
                if (len != (ssize_t) sizeof(msg))
                    continue;
                if (msg.group_id != this->group_id_hash_)
                    continue;
                if (calculate_checksum_(msg) != msg.checksum)
                    continue;

                if (this->handle_message_(msg))
                    discovered_new = true;
                received_any = true;
            }

            if (received_any)
                this->evaluate_role_();

            // Answer a newly-discovered peer promptly so it learns about us within
            // a round-trip instead of waiting up to one heartbeat interval. The gap
            // guard stops two devices from echoing each other indefinitely.
            if (discovered_new && (millis() - this->last_heartbeat_sent_ms_) >= REPLY_MIN_GAP_MS)
                this->send_heartbeat_();
        }

        void UdpFailoverComponent::send_heartbeat_()
        {
            HeartbeatMessage msg{};
            msg.group_id = this->group_id_hash_;
            memcpy(msg.mac, this->my_mac_.addr, 6);
            msg.is_master = this->i_am_master_;
            msg.uptime_sec = millis() / 1000;
            msg.checksum = calculate_checksum_(msg);

            ssize_t result = this->socket_->sendto(&msg, sizeof(msg), 0,
                                                   (struct sockaddr *) &this->dest_addr_, this->dest_addr_len_);
            if (result < 0)
            {
                ESP_LOGW(TAG, "Failed to send heartbeat: errno %d", errno);
            }
            else
            {
                ESP_LOGD(TAG, "Heartbeat sent. I am %s", this->i_am_master_ ? "MASTER" : "BACKUP");
            }
            this->last_heartbeat_sent_ms_ = millis();
        }

        void UdpFailoverComponent::loop()
        {
            if (!this->initialized_)
            {
                uint32_t now = millis();
                if ((now - this->last_init_attempt_ms_) < 1000)
                    return;
                this->last_init_attempt_ms_ = now;

                if (!network::is_connected())
                    return;
                if (!this->init_socket_())
                    return;

                this->initialized_ = true;
                this->socket_ready_ms_ = millis();
                ESP_LOGI(TAG, "UDP Failover socket ready on %s:%u", this->multicast_address_.c_str(), this->port_);

                // Announce ourselves immediately so peers learn about us right away
                // instead of after the first interval.
                this->send_heartbeat_();
            }

            this->receive_packets_();

            uint32_t now = millis();

            // Stay passive until we have listened for the hold-down window, then
            // commit to the role we elected from whatever peers we heard.
            if (!this->active_ && (now - this->socket_ready_ms_) >= STARTUP_HOLD_MS)
            {
                this->active_ = true;
                this->evaluate_role_();
                this->publish_is_master_state_();
                ESP_LOGI(TAG, "Startup hold elapsed — now active as %s", this->effective_master_() ? "MASTER" : "BACKUP");
            }

            if ((now - this->last_heartbeat_sent_ms_) >= HEARTBEAT_INTERVAL_MS)
            {
                this->evaluate_role_();
                this->send_heartbeat_();
            }
        }
#else  // !USE_UDP_FAILOVER_SOCKETS
        void UdpFailoverComponent::receive_packets_() {}
        void UdpFailoverComponent::send_heartbeat_() {}
        void UdpFailoverComponent::loop() {}
#endif

        void UdpFailoverComponent::dump_config()
        {
            ESP_LOGCONFIG(TAG,
                          "UDP Failover:\n"
                          "  Group ID: '%s' (hash=0x%04X)\n"
                          "  Multicast: %s:%u",
                          this->group_id_.c_str(), this->group_id_hash_,
                          this->multicast_address_.c_str(), this->port_);
#ifndef USE_UDP_FAILOVER_SOCKETS
            ESP_LOGE(TAG, "  Socket transport not available on this platform!");
#endif
        }

    }
}
