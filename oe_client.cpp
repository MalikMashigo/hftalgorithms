#include "oe_client.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <fstream>

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

//Send login request
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

    // Read response
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
    // First read the header to get the length
    ndfex::oe::oe_response_header hdr;
    size_t total = 0;
    while (total < sizeof(hdr)) {
        ssize_t n = recv(sock_fd_, (char*)&hdr + total, sizeof(hdr) - total, 0);
        if (n <= 0) return false;
        total += n;
    }

    // Then read the rest of the message
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

bool OEClient::wait_for_response() {
    char buf[256];
    size_t len;
    //Loop until a terminal response is received
    while (true) {
        if (!read_response(buf, len)) return false;
        auto* hdr = reinterpret_cast<ndfex::oe::oe_response_header*>(buf);

        if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::ACK) {
            std::cout << "ACKed!" << std::endl;
            return true;
        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::REJECT) {
            auto* rej = reinterpret_cast<ndfex::oe::order_reject*>(buf);
            std::cerr << "REJECTED, reason: " << (int)rej->reject_reason << std::endl;
            return false;
        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::FILL) {
            auto* fill = reinterpret_cast<ndfex::oe::order_fill*>(buf);
            std::cout << "Filled! qty: " << fill->quantity << " price: " << fill->price << std::endl;
            if (fill->flags == (uint8_t)ndfex::oe::FILL_FLAGS::CLOSED) {
                std::cout << "Order fully filled and closed." << std::endl;
                return true;
            }
        } else if (hdr->msg_type == (uint8_t)ndfex::oe::MSG_TYPE::CLOSE) {
            std::cout << "Order closed." << std::endl;
            return true;
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

bool OEClient::send_new_order(uint64_t order_id, uint32_t symbol, SIDE side, uint32_t qty, int32_t price) {
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

    return wait_for_response();
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

    return wait_for_response();
}

bool OEClient::modify_order(uint64_t order_id, SIDE side, uint32_t qty, int32_t price) {
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

    return wait_for_response();
}



int main() {
    OEClient client("192.168.13.100", 1234);
    if (!client.connect()) return 1;
    if (!client.login("group8", "Uangjrty", 8)) return 1;

    std::string command;

    auto parse_symbol = [](const std::string& s) -> uint32_t {
        if (s == "gold" || s == "GOLD") return 1;
        if (s == "blue" || s == "BLUE") return 2;
        return std::stoul(s);
    };

    while (true) {
        std::cout << "Enter order (buy/sell/delete/modify/quit): ";
        std::cin >> command;

        if (command == "quit") break;

        if (command == "buy" || command == "sell") {
            std::string sym_input;
            uint32_t qty;
            int32_t price;
            uint64_t oid;

            std::cout << "symbol (gold/blue): "; std::cin >> sym_input;
            std::cout << "qty: ";                std::cin >> qty;
            std::cout << "price: ";              std::cin >> price;
            std::cout << "order_id: "; std::cin >> oid;


            SIDE side = (command == "buy") ? SIDE::BUY : SIDE::SELL;
            client.send_new_order(oid, parse_symbol(sym_input), side, qty, price);
        }
        else if (command == "delete") {
            uint64_t oid;
            std::cout << "order_id: "; std::cin >> oid;
            client.delete_order(oid); 
        }
        else if (command == "modify") {
            uint64_t oid;
            uint32_t qty;
            int32_t price;
            std::string side_input;

            std::cout << "order_id: ";    std::cin >> oid;
            std::cout << "side (buy/sell): "; std::cin >> side_input;
            std::cout << "qty: ";         std::cin >> qty;
            std::cout << "price: ";       std::cin >> price;

            SIDE side = (side_input == "buy") ? SIDE::BUY : SIDE::SELL;
            client.modify_order(oid, side, qty, price);
        }
    }
    
    
    return 0;
}