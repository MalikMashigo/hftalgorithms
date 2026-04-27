#include "listener.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <queue>
#include <unordered_map>

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip);

struct BufferedMessage {
    char   data[1500];
    size_t length;
};

void run_listener(SymbolManager& sm) {
    // order_id → symbol_id routing for delete/modify/trade messages
    std::unordered_map<uint64_t, uint32_t> order_to_symbol;

    std::queue<BufferedMessage> live_buffer;

    const char* live_addr   = "239.0.0.1";
    const int   live_port   = 12345;
    const char* replay_addr = "239.0.0.2";
    const int   replay_port = 12345;
    const char* local_ip    = "192.168.13.16";

    int live_sock   = create_multicast_socket(live_addr,   live_port,   local_ip);
    int replay_sock = create_multicast_socket(replay_addr, replay_port, local_ip);

    int epoll_fd = epoll_create(128);
    if (epoll_fd < 0) {
        std::cerr << "Failed to create epoll: " << strerror(errno) << "\n";
        return;
    }

    epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = replay_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, replay_sock, &ev);
    ev.data.fd = live_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, live_sock, &ev);

    char     buf[1500];
    bool     caught_up        = false;
    bool     received_snapshot = false;
    uint32_t messages_processed = 0;
    int      no_replay_count  = 0;

    // ── Inline helpers ────────────────────────────────────────────────────────

    // Route a new_order into SymbolManager and record order→symbol mapping
    auto dispatch_new_order = [&](new_order* msg) {
        sm.on_new_order(msg->symbol, msg);
        order_to_symbol[msg->order_id] = msg->symbol;
    };

    auto dispatch_delete_order = [&](delete_order* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_delete_order(it->second, msg);
        order_to_symbol.erase(it);
    };

    auto dispatch_modify_order = [&](modify_order* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_modify_order(it->second, msg);
    };

    auto dispatch_trade = [&](trade* msg) {
        auto it = order_to_symbol.find(msg->order_id);
        if (it == order_to_symbol.end()) return;
        sm.on_trade(it->second, msg);
    };

    std::cout << "[Listener] Starting market data feed...\n";

    while (true) {
        epoll_event events[16];
        int timeout = caught_up ? -1 : 10;
        int nfds    = epoll_wait(epoll_fd, events, 16, timeout);

        if (nfds < 0) {
            std::cerr << "[Listener] epoll_wait failed: " << strerror(errno) << "\n";
            return;
        }

        // ── Catch-up: switch to live feed once replay goes quiet ─────────────
        if (nfds == 0 && !caught_up && received_snapshot) {
            if (++no_replay_count > 100) {
                std::cout << "[Listener] Caught up — draining "
                          << live_buffer.size() << " buffered messages\n";
                caught_up = true;

                while (!live_buffer.empty()) {
                    BufferedMessage& bm = live_buffer.front();
                    memcpy(buf, bm.data, bm.length);

                    md_header* hdr = reinterpret_cast<md_header*>(buf);
                    if (hdr->magic_number == MAGIC_NUMBER) {
                        switch (hdr->msg_type) {
                            case MSG_TYPE::NEW_ORDER:
                                dispatch_new_order(reinterpret_cast<new_order*>(buf));
                                break;
                            case MSG_TYPE::DELETE_ORDER:
                                dispatch_delete_order(reinterpret_cast<delete_order*>(buf));
                                break;
                            case MSG_TYPE::MODIFY_ORDER:
                                dispatch_modify_order(reinterpret_cast<modify_order*>(buf));
                                break;
                            case MSG_TYPE::TRADE:
                                dispatch_trade(reinterpret_cast<trade*>(buf));
                                break;
                            default:
                                break;
                        }
                    }
                    live_buffer.pop();
                }

                std::cout << "[Listener] Now on live feed.\n";
            }
            continue;
        }

        // ── Process incoming packets ──────────────────────────────────────────
        for (int i = 0; i < nfds; ++i) {
            ssize_t bytes = read(events[i].data.fd, buf, sizeof(buf));
            if (bytes <= 0) continue;

            // Buffer live messages until we've caught up with replay
            if (!caught_up && events[i].data.fd == live_sock) {
                if (live_buffer.size() > 100000) {
                    std::cerr << "[Listener] FATAL: live buffer overflow\n";
                    return;
                }
                BufferedMessage bm;
                memcpy(bm.data, buf, bytes);
                bm.length = bytes;
                live_buffer.push(bm);
                continue;
            }

            md_header* hdr = reinterpret_cast<md_header*>(buf);

            // ── Snapshot ──────────────────────────────────────────────────────
            if (hdr->magic_number == SNAPSHOT_MAGIC_NUMBER) {
                ssize_t offset = 0;
                while (offset < bytes) {
                    md_header* shdr = reinterpret_cast<md_header*>(buf + offset);
                    if (shdr->msg_type != MSG_TYPE::SNAPSHOT_INFO) break;

                    snapshot_info* snap = reinterpret_cast<snapshot_info*>(buf + offset);
                    uint32_t symbol     = snap->symbol;

                    std::cout << "[Listener] Snapshot: symbol=" << symbol
                              << " seq=" << snap->last_md_seq_num
                              << " bids=" << snap->bid_count
                              << " asks=" << snap->ask_count << "\n";

                    offset += snap->header.length;

                    uint32_t total_orders = snap->bid_count + snap->ask_count;
                    for (uint32_t j = 0; j < total_orders && offset < bytes; ++j) {
                        if (offset + (ssize_t)sizeof(new_order) > bytes) break;
                        new_order* om = reinterpret_cast<new_order*>(buf + offset);
                        if (om->header.msg_type != MSG_TYPE::NEW_ORDER) break;
                        dispatch_new_order(om);
                        offset += om->header.length;
                    }

                    received_snapshot = true;
                    no_replay_count   = 0;
                }
                continue;
            }

            // ── Normal market data ────────────────────────────────────────────
            if (hdr->magic_number != MAGIC_NUMBER) continue;

            switch (hdr->msg_type) {
                case MSG_TYPE::NEW_ORDER:
                    dispatch_new_order(reinterpret_cast<new_order*>(buf));
                    break;
                case MSG_TYPE::DELETE_ORDER:
                    dispatch_delete_order(reinterpret_cast<delete_order*>(buf));
                    break;
                case MSG_TYPE::MODIFY_ORDER:
                    dispatch_modify_order(reinterpret_cast<modify_order*>(buf));
                    break;
                case MSG_TYPE::TRADE:
                    dispatch_trade(reinterpret_cast<trade*>(buf));
                    break;
                case MSG_TYPE::HEARTBEAT:
                    no_replay_count = 0;
                    break;
                default:
                    break;
            }

            no_replay_count = 0;
            ++messages_processed;

            if (messages_processed % 5000 == 0) {
                std::cout << "[Listener] " << messages_processed
                          << " messages processed\n";
            }
        }
    }

    close(epoll_fd);
    close(live_sock);
    close(replay_sock);
}

// ── Multicast socket setup ────────────────────────────────────────────────────
// Unchanged from original

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << "\n";
        exit(1);
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind: " << strerror(errno) << "\n";
        exit(1);
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
    mreq.imr_interface.s_addr = inet_addr(local_ip);

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "Failed to join multicast group " << mcast_addr
                  << ": " << strerror(errno) << "\n";
        exit(1);
    }

    std::cout << "[Listener] Joined multicast " << mcast_addr << ":" << port << "\n";
    return fd;
}