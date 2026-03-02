#ifndef OE_CLIENT_H
#define OE_CLIENT_H

#include "oe_messages.h"

class OEClient {
public:
    OEClient(const char* host, int port);
    ~OEClient();

    bool connect();
    bool login(const char* username, const char* password, uint32_t client_id);
    bool send_new_order(uint64_t order_id, uint32_t symbol, SIDE side, uint32_t qty, int32_t price);
    bool delete_order(uint64_t order_id);
    bool modify_order(uint64_t order_id, SIDE side, uint32_t qty, int32_t price);

private:
    const char* host_;
    int port_;
    int sock_fd_;
    uint64_t session_id_;
    uint32_t seq_num_;
    uint32_t client_id_;

    void send_raw(const void* data, size_t len);
    bool read_response(char* buf, size_t& len); 
    bool wait_for_response();
    void log_message(const char* direction, const void* data, size_t len);
};

#endif