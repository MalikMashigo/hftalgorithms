#include "oe_client.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>

std::ofstream logfile("oe_log.txt");

OEClient::OEClient(const char* host, int port)
    : host_(host), port_(port), sock_fd_(-1), session_id_(0), seq_num_(0) {}

OEClient::~OEClient() {
    if (sock_fd_ >= 0) close(sock_fd_);
}

// Establish TCP connection to exchange
bool OEClient::connect() {
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, host_, &addr.sin_addr);

    if (::connect(sock_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Connect failed: " << strerror(errno) << std::endl;
        return false;
    }
    std::cout << "Connected to " << host_ << ":" << port_ << std::endl;
    return true;
}

// Send login request
bool OEClient::login(const char* username, const char* password, uint32_t client_id) {
    client_id_ = client_id;

    ndfex::oe::login msg{};
    msg.header.length     = sizeof(msg);
    msg.header.msg_type   = (uint8_t)ndfex::oe::MSG_TYPE::LOGIN;
    msg.header.version    = ndfex::oe::OE_PROTOCOL_VERSION;
    msg.header.seq_num    = ++seq_num_;
    msg.header.client_id  = client_id_;
    msg.header.session_id = 0;

    strncpy((char*)msg.username, username, 15);
    strncpy((char*)msg.password, password, 15);

    send_raw(&msg, sizeof(msg));

    char buf[256];
    size_t len;
    if (!read_response(buf, len)) return false;

    auto* resp = reinterpret_cast<ndfex::oe::login_response*>(buf);
    if (resp->status != (uint8_t)ndfex::oe::LOGIN_STATUS::SUCCESS) {
        std::cerr << "Login failed, status: " << (int)resp->status << std::endl;
        return false;
    }

    session_id_ = resp->session_id;
    std::cout << "Login successful, session_id: " << session_id_ << std::endl;
    return true;
}

void OEClient::send_raw(const void* data, size_t len) {
    send(sock_fd_, data, len, 0);
    log_message("SENT", data, len);
}

bool OEClient::read_response(char* buf, size_t& out_len) {
    ndfex::oe::oe_response_header hdr;
    size_t total = 0;
    while (total < sizeof(hdr)) {
        ssize_t n = recv(sock_fd_, (char*)&hdr + total, sizeof(hdr) - total, 0);
        if (n <= 0) return false;
        total += n;
    }

    memcpy(buf, &hdr, sizeof(hdr));
    size_t remaining = hdr.length - sizeof(hdr);
    total = 0;
    while (total < remaining) {
        ssize_t n = recv(sock_fd_, buf + sizeof(hdr) + total, remaining - total, 0);
        if (n <= 0) return false;
        total += n;
    }

    out_len = hdr.length;
    log_message("RECV", buf, out_len);
    return true;
}

bool OEClient::wait_for_response(uint64_t expected_order_id) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(3000);

    while (true) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "[OEClient] Timeout waiting for order_id="
                      << expected_order_id << "\n";
            return false;
        }

    char buf[256];
    size_t len;
    while (true) {
        if (!read_response(buf, len)) return false;
        auto* hdr = reinterpret_cast<ndfex::oe::oe_response_header*>(buf);

        if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::ACK) {
            auto* ack = reinterpret_cast<ndfex::oe::order_ack*>(buf);
            live_orders_.insert(ack->order_id);
            if (ack->order_id == expected_order_id) {
                std::cout << "ACKed! order_id=" << ack->order_id << std::endl;
                return true;
            }
            // This is a resting MM order ACK arriving during arb ACK collection.
            // Fill callback has already been wired up by the caller — safe to skip.
            std::cout << "[OEClient] ACK for other order_id=" << ack->order_id
                      << " (waiting for " << expected_order_id
                      << ") — processed and continuing\n";
            continue;

        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::REJECT) {
            auto* rej = reinterpret_cast<ndfex::oe::order_reject*>(buf);
            std::cerr << "[OEClient] REJECTED order_id=" << rej->order_id
                      << " reason=" << (int)rej->reject_reason << std::endl;
            if (on_reject_cb_) on_reject_cb_(rej->order_id);

            if (rej->order_id == expected_order_id) {
                if (rej->reject_reason == (uint8_t)ndfex::oe::REJECT_REASON::UKNOWN_ORDER_ID) {
                std::cout << "[OEClient] REJECT reason=UNKNOWN_ORDER_ID order_id="
                      << rej->order_id << " — already gone, treating as success\n";
                return true;
                }
                std::cerr << "[OEClient] REJECT order_id=" << rej->order_id
                  << " reason=" << (int)rej->reject_reason
                  << " — our order, returning false\n";
                return false;
            }
            std::cerr << " — not our order (wanted " << expected_order_id
                      << "), continuing\n";
            continue;


        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::FILL) {
            auto* fill = reinterpret_cast<ndfex::oe::order_fill*>(buf);
            bool closed = (fill->flags == (uint8_t)ndfex::oe::FILL_FLAGS::CLOSED);

            if (on_fill_cb_) {
                on_fill_cb_(FillEvent{fill->order_id, fill->quantity,
                                      fill->price, closed});
            }

             std::cout << "[OEClient] Filled! order_id=" << fill->order_id
                      << " qty=" << fill->quantity
                      << " price=" << fill->price << std::endl;


            if (closed) {
                live_orders_.erase(fill->order_id);
                std::cout << "Order fully filled and closed." << std::endl;
                return true;
            }

        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::CLOSE) {
            auto* cl = reinterpret_cast<ndfex::oe::order_closed*>(buf);
            live_orders_.erase(cl->order_id);
            std::cout << "Order closed. order_id=" << cl->order_id << std::endl;
            if (on_close_cb_) on_close_cb_(cl->order_id);
            return true;
            }
        }
    }
}
void OEClient::log_message(const char* direction, const void* data, size_t len) {
    logfile << direction << " [" << len << " bytes]: ";
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        logfile << std::hex << (int)bytes[i] << " ";
    }
    logfile << std::dec << "\n";
    logfile.flush();
}

bool OEClient::send_new_order(uint64_t order_id, uint32_t symbol,
                               SIDE side, uint32_t qty, int32_t price) {
    ndfex::oe::new_order msg{};
    msg.header.length     = sizeof(msg);
    msg.header.msg_type   = (uint8_t)ndfex::oe::MSG_TYPE::NEW_ORDER;
    msg.header.version    = ndfex::oe::OE_PROTOCOL_VERSION;
    msg.header.seq_num    = ++seq_num_;
    msg.header.client_id  = client_id_;
    msg.header.session_id = session_id_;
    msg.order_id          = order_id;
    msg.symbol            = symbol;
    msg.side              = side;
    msg.quantity          = qty;
    msg.price             = price;
    msg.flags             = 0;

    send_raw(&msg, sizeof(msg));
    return wait_for_response(order_id);
}

void OEClient::send_new_order_no_wait(uint64_t order_id, uint32_t symbol,
                                       SIDE side, uint32_t qty, int32_t price) {
    ndfex::oe::new_order msg{};
    msg.header.length     = sizeof(msg);
    msg.header.msg_type   = (uint8_t)ndfex::oe::MSG_TYPE::NEW_ORDER;
    msg.header.version    = ndfex::oe::OE_PROTOCOL_VERSION;
    msg.header.seq_num    = ++seq_num_;
    msg.header.client_id  = client_id_;
    msg.header.session_id = session_id_;
    msg.order_id          = order_id;
    msg.symbol            = symbol;
    msg.side              = side;
    msg.quantity          = qty;
    msg.price             = price;
    msg.flags             = 0;

    send_raw(&msg, sizeof(msg));
    // No wait_for_response() — caller collects ACKs separately
}

bool OEClient::delete_order(uint64_t order_id) {
    ndfex::oe::delete_order msg{};
    msg.header.length     = sizeof(msg);
    msg.header.msg_type   = (uint8_t)ndfex::oe::MSG_TYPE::DELETE_ORDER;
    msg.header.version    = ndfex::oe::OE_PROTOCOL_VERSION;
    msg.header.seq_num    = ++seq_num_;
    msg.header.client_id  = client_id_;
    msg.header.session_id = session_id_;
    msg.order_id          = order_id;

    send_raw(&msg, sizeof(msg));
    return wait_for_response(order_id);
}

bool OEClient::modify_order(uint64_t order_id, SIDE side,
                             uint32_t qty, int32_t price) {
    ndfex::oe::modify_order msg{};
    msg.header.length     = sizeof(msg);
    msg.header.msg_type   = (uint8_t)ndfex::oe::MSG_TYPE::MODIFY_ORDER;
    msg.header.version    = ndfex::oe::OE_PROTOCOL_VERSION;
    msg.header.seq_num    = ++seq_num_;
    msg.header.client_id  = client_id_;
    msg.header.session_id = session_id_;
    msg.order_id          = order_id;
    msg.side              = side;
    msg.quantity          = qty;
    msg.price             = price;

    send_raw(&msg, sizeof(msg));
    return wait_for_response(order_id);
}

void OEClient::cancel_all_open_orders() {
    // Snapshot to avoid mutation while iterating
    std::vector<uint64_t> to_cancel(live_orders_.begin(), live_orders_.end());
    std::cout << "cancel_all_open_orders: " << to_cancel.size() << " orders" << std::endl;
    for (uint64_t oid : to_cancel) {
        delete_order(oid);
    }
}


// int main() {
//     OEClient client("192.168.13.100", 1234);
//     if (!client.connect()) return 1;
//     if (!client.login("group8", "Uangjrty", 8)) return 1;

//     std::string command;

//     auto parse_symbol = [](const std::string& s) -> uint32_t {
//         if (s == "gold" || s == "GOLD") return 1;
//         if (s == "blue" || s == "BLUE") return 2;
//         return std::stoul(s);
//     };

//     while (true) {
//         std::cout << "Enter order (buy/sell/delete/modify/cancelall/quit): ";
//         std::cin >> command;

//         if (command == "quit") break;

//         if (command == "cancelall") {
//             client.cancel_all_open_orders();
//         } else if (command == "buy" || command == "sell") {
//             std::string sym_input;
//             uint32_t qty;
//             int32_t price;
//             uint64_t oid;

//             std::cout << "symbol (gold/blue): "; std::cin >> sym_input;
//             std::cout << "qty: ";                std::cin >> qty;
//             std::cout << "price: ";              std::cin >> price;
//             std::cout << "order_id: ";           std::cin >> oid;

//             SIDE side = (command == "buy") ? SIDE::BUY : SIDE::SELL;
//             client.send_new_order(oid, parse_symbol(sym_input), side, qty, price);
//         } else if (command == "delete") {
//             uint64_t oid;
//             std::cout << "order_id: "; std::cin >> oid;
//             client.delete_order(oid);
//         } else if (command == "modify") {
//             uint64_t oid;
//             uint32_t qty;
//             int32_t price;
//             std::string side_input;

//             std::cout << "order_id: ";        std::cin >> oid;
//             std::cout << "side (buy/sell): "; std::cin >> side_input;
//             std::cout << "qty: ";             std::cin >> qty;
//             std::cout << "price: ";           std::cin >> price;

//             SIDE side = (side_input == "buy") ? SIDE::BUY : SIDE::SELL;
//             client.modify_order(oid, side, qty, price);
//         }
//     }

//     return 0;
// }

bool OEClient::wait_for_fill(uint64_t expected_order_id) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(5000);

    while (true) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "[OEClient] wait_for_fill timeout order_id="
                      << expected_order_id << "\n";
            return false;
        }

        char buf[256];
        size_t len;
        if (!read_response(buf, len)) return false;

        auto* hdr = reinterpret_cast<ndfex::oe::oe_response_header*>(buf);

        if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::FILL) {
            auto* fill = reinterpret_cast<ndfex::oe::order_fill*>(buf);
            bool closed = (fill->flags == (uint8_t)ndfex::oe::FILL_FLAGS::CLOSED);

            // Always fire callback — keeps MM positions accurate
            if (on_fill_cb_)
                on_fill_cb_(FillEvent{fill->order_id, fill->quantity,
                                      fill->price, closed});
            if (closed) live_orders_.erase(fill->order_id);

            std::cout << "[OEClient] wait_for_fill: FILL order_id=" << fill->order_id
                      << " qty=" << fill->quantity
                      << " price=" << fill->price
                      << " closed=" << closed;

            if (fill->order_id == expected_order_id && closed) {
                std::cout << " — target filled\n";
                return true;
            }
            // Partial fill on our order, or fill on a different order
            std::cout << (fill->order_id == expected_order_id
                            ? " — partial, continuing\n"
                            : " — other order, continuing\n");

        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::ACK) {
            auto* ack = reinterpret_cast<ndfex::oe::order_ack*>(buf);
            live_orders_.insert(ack->order_id);
            std::cout << "[OEClient] wait_for_fill: ACK order_id="
                      << ack->order_id << " (waiting for fill on "
                      << expected_order_id << ")\n";

        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::REJECT) {
            auto* rej = reinterpret_cast<ndfex::oe::order_reject*>(buf);
            if (on_reject_cb_) on_reject_cb_(rej->order_id);
            std::cerr << "[OEClient] wait_for_fill: REJECT order_id="
                      << rej->order_id << " reason=" << (int)rej->reject_reason;
            if (rej->order_id == expected_order_id) {
                std::cerr << " — our order rejected\n";
                return false;
            }
            std::cerr << " — other order\n";

        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::CLOSE) {
            auto* cl = reinterpret_cast<ndfex::oe::order_closed*>(buf);
            live_orders_.erase(cl->order_id);
            if (on_close_cb_) on_close_cb_(cl->order_id);
            std::cout << "[OEClient] wait_for_fill: CLOSE order_id="
                      << cl->order_id << "\n";
            if (cl->order_id == expected_order_id) return false;
        }
    }
}
