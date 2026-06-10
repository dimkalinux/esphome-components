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
                // of them dying simultaneously and more like our RX path dying
                // (snooping entry flushed, AP rebooted). Refresh the membership
                // and hold the promotion; if the master is alive we will hear it
                // and stand down, if it is truly dead we only lose the grace time.
                if (had_peers && this->peers_.empty() && !this->promotion_grace_)
                {
                    this->promotion_grace_ = true;
                    this->promotion_grace_start_ms_ = millis();
#ifdef USE_UDP_FAILOVER_SOCKETS
                    this->rejoin_multicast_();
                    this->last_rejoin_ms_ = millis();
#endif
                    ESP_LOGW(TAG, "All peers lost at once — suspecting RX failure, delaying promotion %ums", PROMOTION_GRACE_MS);
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

        void UdpFailoverComponent::rejoin_multicast_()
        {
            if (this->socket_ == nullptr)
                return;

            struct ip_mreq imreq {};
            imreq.imr_interface.s_addr = ESPHOME_INADDR_ANY;
            if (inet_aton(this->multicast_address_.c_str(), &imreq.imr_multiaddr) == 0)
                return;

            // Drop then re-add so lwIP emits a fresh, unsolicited membership
            // report. A bare re-add is a no-op when the group is already joined,
            // which would not refresh the AP's snooping entry.
            this->socket_->setsockopt(IPPROTO_IP, IP_DROP_MEMBERSHIP, &imreq, sizeof(imreq));
            if (this->socket_->setsockopt(IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) < 0)
            {
                // A socket without membership is exactly the silent-RX state we
                // are trying to kill — rebuild it from scratch rather than limp
                // along deaf until the next rejoin attempt.
                ESP_LOGW(TAG, "Multicast re-join failed (errno %d) — rebuilding socket", errno);
                this->close_socket_();
            }
            else
            {
                ESP_LOGD(TAG, "Multicast membership refreshed for %s", this->multicast_address_.c_str());
            }
        }

        void UdpFailoverComponent::close_socket_()
        {
            this->socket_ = nullptr;  // unique_ptr dtor closes the fd
            this->initialized_ = false;
            // Re-listen through the startup hold before acting again, and forget
            // peers we can no longer hear, so a reconnect never resumes as a
            // stale master.
            this->active_ = false;
            this->peers_.clear();
            this->promotion_grace_ = false;
            this->publish_is_master_state_();
        }

        // TODO(split-brain): capture the sender address with recvfrom() and keep a
        // last-known unicast address per peer, then send heartbeats unicast to
        // known peers in addition to the multicast. Unicast is ACKed at the WiFi
        // layer and does not depend on IGMP snooping, which would remove the whole
        // class of "multicast forwarding died" failures instead of patching
        // around it.
        void UdpFailoverComponent::receive_packets_()
        {
            if (this->socket_ == nullptr)
                return;

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
            if (this->socket_ == nullptr)
                return;

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
            // Tear the socket down on network loss so a reconnect rebuilds it and
            // re-joins the multicast group. A stale socket kept across a WiFi drop
            // silently stops receiving (and its membership is gone anyway).
            if (this->initialized_ && !network::is_connected())
            {
                ESP_LOGW(TAG, "Network lost — closing socket, will re-init on reconnect");
                this->close_socket_();
            }

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
                this->last_rejoin_ms_ = this->socket_ready_ms_;
                ESP_LOGI(TAG, "UDP Failover socket ready on %s:%u", this->multicast_address_.c_str(), this->port_);

                // Announce ourselves immediately so peers learn about us right away
                // instead of after the first interval.
                this->send_heartbeat_();
            }

            this->receive_packets_();

            uint32_t now = millis();

            // Keep the AP's IGMP-snooping entry fresh so the group keeps being
            // forwarded to us even with no querier on the segment.
            if ((now - this->last_rejoin_ms_) >= MEMBERSHIP_REJOIN_MS)
            {
                this->rejoin_multicast_();
                this->last_rejoin_ms_ = now;
            }

            // While in the startup hold, repeat the announce every second instead
            // of trusting a single unacked multicast packet: if the lone boot
            // announce is lost (common for multicast over WiFi), an existing
            // master never replies and we would elect ourselves alongside it.
            if (!this->active_ && (now - this->last_heartbeat_sent_ms_) >= REPLY_MIN_GAP_MS)
                this->send_heartbeat_();

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
