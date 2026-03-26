#ifndef RISK_MANAGER_H
#define RISK_MANAGER_H

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <string>
#include <fstream>

#include "iorder_sender.h"
#include "position_tracker.h"
#include "exposure_tracker.h"
#include "pnl_tracker.h"

// ── Risk parameter bundle ────────────────────────────────────────────────────

struct RiskLimits {
    uint32_t max_qty_per_order      = 100;   // single order qty ceiling
    uint32_t max_qty_per_side       = 500;   // total outstanding qty on one side
    uint32_t max_exposure           = 1000;  // max exposure per side (pos + outstanding)
    int32_t  max_position           = 200;   // absolute position limit
    uint32_t max_orders_per_second  = 10;    // order-rate limit
    uint32_t max_orders_per_seq_num = 5;     // orders allowed per md seq-num tick
    uint32_t max_unacked_orders     = 5;     // orders sent but not yet ACK/REJ
    double   min_pnl                = -10000.0; // shutdown floor
    int32_t  min_valid_price        = 1;     // price must be >= this
};

// ── Outcome codes returned by risk checks ───────────────────────────────────

enum class RiskResult {
    OK,
    SYSTEM_SHUTDOWN,
    INVALID_PRICE,
    QTY_PER_ORDER_EXCEEDED,
    QTY_PER_SIDE_EXCEEDED,
    EXPOSURE_EXCEEDED,
    POSITION_LIMIT_WOULD_INCREASE,  // abs(pos) at limit and order pushes further out
    POSITION_LIMIT_EXCEEDED,        // order would move position past limit
    RATE_LIMIT_PER_SECOND,
    RATE_LIMIT_PER_SEQ_NUM,
    UNACKED_LIMIT_EXCEEDED,
    DUPLICATE_ORDER_ID,
    UNKNOWN_ORDER_ID,
};

const char* risk_result_str(RiskResult r);

// ── Per-order metadata tracked while an order is live ───────────────────────

struct OpenOrderInfo {
    uint32_t symbol;
    SIDE     side;
    uint32_t qty;
    int32_t  price;
};

// ── RiskManager ─────────────────────────────────────────────────────────────
//
// Wraps an IOrderSender, enforcing all risk limits before forwarding each
// order.  The caller must notify RiskManager of exchange responses (fills,
// rejects, closes) so that internal state stays accurate.

class RiskManager {
public:
    RiskManager(IOrderSender& sender,
                const RiskLimits& limits,
                const std::string& log_path = "risk_log.txt");

    // ── Order entry ──────────────────────────────────────────────────────────

    // Pure risk check (does not send, does not mutate state).
    // Rate-limit checks are NOT included here (they require mutation).
    RiskResult check_new_order(uint64_t order_id, SIDE side,
                               uint32_t qty, int32_t price) const;

    RiskResult check_modify_order(uint64_t order_id, SIDE new_side,
                                  uint32_t new_qty, int32_t new_price) const;

    // Full pipeline: risk-check → rate-limit check → send.
    bool send_new_order(uint64_t order_id, uint32_t symbol,
                        SIDE side, uint32_t qty, int32_t price);

    bool modify_order(uint64_t order_id, SIDE side,
                      uint32_t qty, int32_t price);

    bool delete_order(uint64_t order_id);

    // Cancel every currently open order and update internal state.
    void cancel_all_open_orders();

    // ── Exchange response callbacks ──────────────────────────────────────────

    // Call when the exchange reports a fill on one of our orders.
    // `closed` = true when this fill fully closes the order.
    void on_fill(uint64_t order_id, uint32_t qty, int32_t price, bool closed);

    // Call when an order is rejected by the exchange.
    void on_reject(uint64_t order_id);

    // Call when an order is closed (e.g. cancelled) by the exchange.
    void on_close(uint64_t order_id);

    // ── Market data hooks ────────────────────────────────────────────────────

    // Feed the latest best mid-price for mark-to-market PNL valuation.
    void update_mark_price(int32_t mark_price);

    // Notify that a new market-data sequence number has been observed.
    // Resets the per-seq-num order counter.
    void on_seq_num_update(uint32_t seq_num);

    // ── Introspection ────────────────────────────────────────────────────────

    bool    is_shutdown()    const { return is_shutdown_; }
    double  get_total_pnl()  const { return pnl_tracker_.get_total_pnl(); }
    int32_t get_position()   const { return position_tracker_.get_position(); }

    const PositionTracker& position_tracker() const { return position_tracker_; }
    const ExposureTracker& exposure_tracker() const { return exposure_tracker_; }
    const PnlTracker&      pnl_tracker()      const { return pnl_tracker_; }

    // Returns snapshot of all currently open orders.
    std::vector<uint64_t> open_order_ids() const;

    // Checks PNL and position against shutdown thresholds; if either is
    // breached, sets is_shutdown_ = true and calls cancel_all_open_orders().
    void check_and_shutdown_if_needed();

private:
    IOrderSender&   sender_;
    RiskLimits      limits_;
    PositionTracker position_tracker_;
    ExposureTracker exposure_tracker_;
    PnlTracker      pnl_tracker_;

    std::unordered_map<uint64_t, OpenOrderInfo> open_orders_;

    // Rate-limit state
    uint32_t orders_this_second_;
    std::chrono::steady_clock::time_point second_window_start_;
    uint32_t orders_this_seq_num_;
    uint32_t last_seq_num_;
    uint32_t unacked_orders_;

    bool is_shutdown_;
    bool in_shutdown_; // reentry guard for cancel_all during shutdown

    mutable std::ofstream log_file_;

    void        log(const std::string& msg) const;
    void        refresh_rate_window();
    bool        would_increase_abs_position(SIDE side) const;
    RiskResult  check_rate_limits() const;
};

#endif
