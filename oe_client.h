#ifndef OE_CLIENT_H
#define OE_CLIENT_H

#include <functional>
#include <unordered_set>
#include "oe_messages.h"
#include "iorder_sender.h"

// ── Fill event delivered via callback ────────────────────────────────────────

struct FillEvent {
    uint64_t order_id;
    uint32_t qty;
    int32_t  price;
    bool     closed; // true = order fully filled or otherwise closed
};

// ── OEClient ─────────────────────────────────────────────────────────────────
//
// TCP order-entry client.  Implements IOrderSender so a RiskManager can use
// it in production (vs a mock in tests).
//
// Callbacks are invoked synchronously inside wait_for_response() and allow
// an external RiskManager (or strategy) to track fills, rejects, and closes
// without polling.

class OEClient : public IOrderSender {
public:
    OEClient(const char* host, int port);
    ~OEClient();

    bool connect();
    bool login(const char* username, const char* password, uint32_t client_id);

    // IOrderSender implementation
    bool send_new_order(uint64_t order_id, uint32_t symbol, SIDE side,
                        uint32_t qty, int32_t price) override;
    bool delete_order(uint64_t order_id) override;
    bool modify_order(uint64_t order_id, SIDE side,
                      uint32_t qty, int32_t price) override;

    // Cancel every order that has been ACK'd and not yet fully filled/closed.
    void cancel_all_open_orders();

    // ── Response callbacks ───────────────────────────────────────────────────
    using FillCb   = std::function<void(const FillEvent&)>;
    using RejectCb = std::function<void(uint64_t order_id)>;
    using CloseCb  = std::function<void(uint64_t order_id)>;

    void set_on_fill  (FillCb   cb) { on_fill_cb_   = cb; }
    void set_on_reject(RejectCb cb) { on_reject_cb_ = cb; }
    void set_on_close (CloseCb  cb) { on_close_cb_  = cb; }

private:
    const char* host_;
    int         port_;
    int         sock_fd_;
    uint64_t    session_id_;
    uint32_t    seq_num_;
    uint32_t    client_id_;

    // Orders that have been ACK'd but not yet fully filled or closed.
    std::unordered_set<uint64_t> live_orders_;

    FillCb   on_fill_cb_;
    RejectCb on_reject_cb_;
    CloseCb  on_close_cb_;

    void send_raw(const void* data, size_t len);
    bool read_response(char* buf, size_t& len);
    bool wait_for_response();
    void log_message(const char* direction, const void* data, size_t len);
};

#endif
