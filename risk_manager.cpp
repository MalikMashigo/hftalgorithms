#include "risk_manager.h"

#include <iostream>
#include <sstream>
#include <cmath>
#include <ctime>

// ── Helper: enum → string ────────────────────────────────────────────────────

const char* risk_result_str(RiskResult r) {
    switch (r) {
        case RiskResult::OK:                          return "OK";
        case RiskResult::SYSTEM_SHUTDOWN:             return "SYSTEM_SHUTDOWN";
        case RiskResult::INVALID_PRICE:               return "INVALID_PRICE";
        case RiskResult::QTY_PER_ORDER_EXCEEDED:      return "QTY_PER_ORDER_EXCEEDED";
        case RiskResult::QTY_PER_SIDE_EXCEEDED:       return "QTY_PER_SIDE_EXCEEDED";
        case RiskResult::EXPOSURE_EXCEEDED:           return "EXPOSURE_EXCEEDED";
        case RiskResult::POSITION_LIMIT_WOULD_INCREASE: return "POSITION_LIMIT_WOULD_INCREASE";
        case RiskResult::POSITION_LIMIT_EXCEEDED:     return "POSITION_LIMIT_EXCEEDED";
        case RiskResult::RATE_LIMIT_PER_SECOND:       return "RATE_LIMIT_PER_SECOND";
        case RiskResult::RATE_LIMIT_PER_SEQ_NUM:      return "RATE_LIMIT_PER_SEQ_NUM";
        case RiskResult::UNACKED_LIMIT_EXCEEDED:      return "UNACKED_LIMIT_EXCEEDED";
        case RiskResult::DUPLICATE_ORDER_ID:          return "DUPLICATE_ORDER_ID";
        case RiskResult::UNKNOWN_ORDER_ID:            return "UNKNOWN_ORDER_ID";
        default:                                      return "UNKNOWN";
    }
}

// ── Constructor ──────────────────────────────────────────────────────────────

RiskManager::RiskManager(IOrderSender& sender,
                         const RiskLimits& limits,
                         const std::string& log_path)
    : sender_(sender)
    , limits_(limits)
    , position_tracker_(limits.max_position)
    , exposure_tracker_(position_tracker_, limits.max_exposure)
    , pnl_tracker_(limits.min_pnl)
    , orders_this_second_(0)
    , second_window_start_(std::chrono::steady_clock::now())
    , orders_this_seq_num_(0)
    , last_seq_num_(0)
    , unacked_orders_(0)
    , is_shutdown_(false)
    , in_shutdown_(false)
    , log_file_(log_path)
{
    log("=== RiskManager started ===");
    std::ostringstream ss;
    ss << "Limits: max_qty_per_order=" << limits_.max_qty_per_order
       << " max_qty_per_side=" << limits_.max_qty_per_side
       << " max_exposure=" << limits_.max_exposure
       << " max_position=" << limits_.max_position
       << " max_ord/s=" << limits_.max_orders_per_second
       << " max_ord/seq=" << limits_.max_orders_per_seq_num
       << " max_unacked=" << limits_.max_unacked_orders
       << " min_pnl=" << limits_.min_pnl;
    log(ss.str());
}

// ── Private helpers ──────────────────────────────────────────────────────────

void RiskManager::log(const std::string& msg) const {
    if (log_file_.is_open()) {
        log_file_ << "[RISK] " << msg << "\n";
        log_file_.flush();
    }
    std::cout << "[RISK] " << msg << "\n";
}

void RiskManager::refresh_rate_window() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - second_window_start_).count();
    if (elapsed >= 1000) {
        orders_this_second_   = 0;
        second_window_start_  = now;
    }
}

bool RiskManager::would_increase_abs_position(SIDE side) const {
    int32_t pos = position_tracker_.get_position();
    // BUY increases |pos| when pos >= 0 (making us longer)
    // SELL increases |pos| when pos <= 0 (making us shorter)
    return (side == SIDE::BUY && pos >= 0) || (side == SIDE::SELL && pos <= 0);
}

RiskResult RiskManager::check_rate_limits() const {
    if (orders_this_second_ >= limits_.max_orders_per_second)
        return RiskResult::RATE_LIMIT_PER_SECOND;
    if (orders_this_seq_num_ >= limits_.max_orders_per_seq_num)
        return RiskResult::RATE_LIMIT_PER_SEQ_NUM;
    if (unacked_orders_ >= limits_.max_unacked_orders)
        return RiskResult::UNACKED_LIMIT_EXCEEDED;
    return RiskResult::OK;
}

// ── Pure risk checks (const, no mutation) ────────────────────────────────────

RiskResult RiskManager::check_new_order(uint64_t order_id, SIDE side,
                                         uint32_t qty, int32_t price) const {
    if (is_shutdown_)
        return RiskResult::SYSTEM_SHUTDOWN;

    if (price < limits_.min_valid_price)
        return RiskResult::INVALID_PRICE;

    if (qty > limits_.max_qty_per_order)
        return RiskResult::QTY_PER_ORDER_EXCEEDED;

    // Total outstanding qty on this side must not exceed max_qty_per_side
    uint32_t side_outstanding = (side == SIDE::BUY)
        ? exposure_tracker_.get_outstanding_buy_qty()
        : exposure_tracker_.get_outstanding_sell_qty();
    if (side_outstanding + qty > limits_.max_qty_per_side)
        return RiskResult::QTY_PER_SIDE_EXCEEDED;

    if (exposure_tracker_.would_exceed_exposure(side, qty))
        return RiskResult::EXPOSURE_EXCEEDED;

    // If already at the absolute position limit, block any order that would
    // push the position further away from zero.
    if (position_tracker_.at_limit() && would_increase_abs_position(side))
        return RiskResult::POSITION_LIMIT_WOULD_INCREASE;

    if (position_tracker_.would_exceed_limit(side, qty))
        return RiskResult::POSITION_LIMIT_EXCEEDED;

    if (open_orders_.count(order_id))
        return RiskResult::DUPLICATE_ORDER_ID;

    return RiskResult::OK;
}

RiskResult RiskManager::check_modify_order(uint64_t order_id, SIDE new_side,
                                            uint32_t new_qty, int32_t new_price) const {
    if (is_shutdown_)
        return RiskResult::SYSTEM_SHUTDOWN;

    auto it = open_orders_.find(order_id);
    if (it == open_orders_.end())
        return RiskResult::UNKNOWN_ORDER_ID;

    const auto& existing = it->second;

    if (new_price < limits_.min_valid_price)
        return RiskResult::INVALID_PRICE;

    if (new_qty > limits_.max_qty_per_order)
        return RiskResult::QTY_PER_ORDER_EXCEEDED;

    if (new_side == existing.side) {
        // Same side: only the incremental increase in qty adds new risk
        if (new_qty > existing.qty) {
            uint32_t delta = new_qty - existing.qty;
            uint32_t side_outstanding = (new_side == SIDE::BUY)
                ? exposure_tracker_.get_outstanding_buy_qty()
                : exposure_tracker_.get_outstanding_sell_qty();
            if (side_outstanding + delta > limits_.max_qty_per_side)
                return RiskResult::QTY_PER_SIDE_EXCEEDED;
            if (exposure_tracker_.would_exceed_exposure(new_side, delta))
                return RiskResult::EXPOSURE_EXCEEDED;
            if (position_tracker_.at_limit() && would_increase_abs_position(new_side))
                return RiskResult::POSITION_LIMIT_WOULD_INCREASE;
            if (position_tracker_.would_exceed_limit(new_side, delta))
                return RiskResult::POSITION_LIMIT_EXCEEDED;
        }
        // Decreasing qty on same side is always safe from a risk perspective
    } else {
        // Side flip: treat full new_qty as new exposure (conservative)
        uint32_t side_outstanding = (new_side == SIDE::BUY)
            ? exposure_tracker_.get_outstanding_buy_qty()
            : exposure_tracker_.get_outstanding_sell_qty();
        if (side_outstanding + new_qty > limits_.max_qty_per_side)
            return RiskResult::QTY_PER_SIDE_EXCEEDED;
        if (exposure_tracker_.would_exceed_exposure(new_side, new_qty))
            return RiskResult::EXPOSURE_EXCEEDED;
        if (position_tracker_.at_limit() && would_increase_abs_position(new_side))
            return RiskResult::POSITION_LIMIT_WOULD_INCREASE;
        if (position_tracker_.would_exceed_limit(new_side, new_qty))
            return RiskResult::POSITION_LIMIT_EXCEEDED;
    }
    return RiskResult::OK;
}

// ── Order entry (mutating) ───────────────────────────────────────────────────

bool RiskManager::send_new_order(uint64_t order_id, uint32_t symbol,
                                  SIDE side, uint32_t qty, int32_t price) {
    refresh_rate_window();

    RiskResult rc = check_new_order(order_id, side, qty, price);
    if (rc != RiskResult::OK) {
        std::ostringstream ss;
        ss << "BLOCKED new_order id=" << order_id
           << " sym=" << symbol
           << " side=" << (side == SIDE::BUY ? "BUY" : "SELL")
           << " qty=" << qty << " px=" << price
           << " reason=" << risk_result_str(rc);
        log(ss.str());
        return false;
    }

    rc = check_rate_limits();
    if (rc != RiskResult::OK) {
        std::ostringstream ss;
        ss << "BLOCKED new_order id=" << order_id << " reason=" << risk_result_str(rc);
        log(ss.str());
        return false;
    }

    {
        std::ostringstream ss;
        ss << "SENDING new_order id=" << order_id
           << " sym=" << symbol
           << " side=" << (side == SIDE::BUY ? "BUY" : "SELL")
           << " qty=" << qty << " px=" << price;
        log(ss.str());
    }

    ++unacked_orders_;
    ++orders_this_second_;
    ++orders_this_seq_num_;

    bool ok = sender_.send_new_order(order_id, symbol, side, qty, price);

    --unacked_orders_;

    if (ok) {
        open_orders_[order_id] = {symbol, side, qty, price};
        exposure_tracker_.on_order_sent(order_id, side, qty, price);
        log("ACK new_order id=" + std::to_string(order_id));
    } else {
        log("REJECT new_order id=" + std::to_string(order_id));
    }

    check_and_shutdown_if_needed();
    return ok;
}

bool RiskManager::modify_order(uint64_t order_id, SIDE side,
                                uint32_t qty, int32_t price) {
    refresh_rate_window();

    RiskResult rc = check_modify_order(order_id, side, qty, price);
    if (rc != RiskResult::OK) {
        std::ostringstream ss;
        ss << "BLOCKED modify id=" << order_id << " reason=" << risk_result_str(rc);
        log(ss.str());
        return false;
    }

    rc = check_rate_limits();
    if (rc != RiskResult::OK) {
        std::ostringstream ss;
        ss << "BLOCKED modify id=" << order_id << " reason=" << risk_result_str(rc);
        log(ss.str());
        return false;
    }

    auto it = open_orders_.find(order_id);
    OpenOrderInfo old_info = it->second; // copy before modifying

    {
        std::ostringstream ss;
        ss << "SENDING modify id=" << order_id
           << " side=" << (side == SIDE::BUY ? "BUY" : "SELL")
           << " qty=" << qty << " px=" << price;
        log(ss.str());
    }

    ++unacked_orders_;
    ++orders_this_second_;
    ++orders_this_seq_num_;

    bool ok = sender_.modify_order(order_id, side, qty, price);

    --unacked_orders_;

    if (ok) {
        // Update exposure: remove old registration, add new one
        exposure_tracker_.on_order_removed(order_id);
        exposure_tracker_.on_order_sent(order_id, side, qty, price);
        open_orders_[order_id] = {old_info.symbol, side, qty, price};
        log("ACK modify id=" + std::to_string(order_id));
    } else {
        log("REJECT modify id=" + std::to_string(order_id));
    }

    check_and_shutdown_if_needed();
    return ok;
}

bool RiskManager::delete_order(uint64_t order_id) {
    std::ostringstream ss;
    ss << "SENDING delete id=" << order_id;
    log(ss.str());

    bool ok = sender_.delete_order(order_id);

    if (ok) {
        exposure_tracker_.on_order_removed(order_id);
        open_orders_.erase(order_id);
        log("ACK delete id=" + std::to_string(order_id));
    } else {
        log("REJECT delete id=" + std::to_string(order_id));
    }
    return ok;
}

void RiskManager::cancel_all_open_orders() {
    std::ostringstream ss;
    ss << "CANCEL_ALL: " << open_orders_.size() << " open orders";
    log(ss.str());

    // Snapshot keys to avoid iterator invalidation while deleting
    std::vector<uint64_t> ids;
    ids.reserve(open_orders_.size());
    for (auto& kv : open_orders_) ids.push_back(kv.first);

    for (uint64_t oid : ids) {
        bool ok = sender_.delete_order(oid);
        if (ok) {
            exposure_tracker_.on_order_removed(oid);
            open_orders_.erase(oid);
            log("CANCELLED id=" + std::to_string(oid));
        } else {
            log("CANCEL_FAILED id=" + std::to_string(oid));
        }
    }
}

// ── Exchange response callbacks ──────────────────────────────────────────────

void RiskManager::on_fill(uint64_t order_id, uint32_t qty, int32_t price, bool closed) {
    auto it = open_orders_.find(order_id);
    if (it == open_orders_.end()) {
        log("on_fill: unknown order id=" + std::to_string(order_id));
        return;
    }

    SIDE side = it->second.side;

    position_tracker_.on_fill(side, qty);
    pnl_tracker_.on_fill(side, qty, price);
    exposure_tracker_.on_fill(order_id, qty);

    {
        std::ostringstream ss;
        ss << "FILL id=" << order_id
           << " side=" << (side == SIDE::BUY ? "BUY" : "SELL")
           << " qty=" << qty << " px=" << price
           << " pos=" << position_tracker_.get_position()
           << " pnl=" << pnl_tracker_.get_total_pnl()
           << (closed ? " [CLOSED]" : "");
        log(ss.str());
    }

    if (closed) open_orders_.erase(order_id);

    check_and_shutdown_if_needed();
}

void RiskManager::on_reject(uint64_t order_id) {
    log("REJECT (exchange) id=" + std::to_string(order_id));
    exposure_tracker_.on_order_removed(order_id);
    open_orders_.erase(order_id);
}

void RiskManager::on_close(uint64_t order_id) {
    log("CLOSE id=" + std::to_string(order_id));
    exposure_tracker_.on_order_removed(order_id);
    open_orders_.erase(order_id);
}

// ── Market data hooks ────────────────────────────────────────────────────────

void RiskManager::update_mark_price(int32_t mark_price) {
    pnl_tracker_.update_mark_price(mark_price);
}

void RiskManager::on_seq_num_update(uint32_t seq_num) {
    if (seq_num != last_seq_num_) {
        orders_this_seq_num_ = 0;
        last_seq_num_ = seq_num;
    }
}

// ── Introspection ────────────────────────────────────────────────────────────

std::vector<uint64_t> RiskManager::open_order_ids() const {
    std::vector<uint64_t> ids;
    ids.reserve(open_orders_.size());
    for (auto& kv : open_orders_) ids.push_back(kv.first);
    return ids;
}

// ── Shutdown ─────────────────────────────────────────────────────────────────

void RiskManager::check_and_shutdown_if_needed() {
    if (is_shutdown_ || in_shutdown_) return;

    bool pnl_breach = pnl_tracker_.below_min_pnl();
    bool pos_breach = (std::abs(position_tracker_.get_position()) > limits_.max_position);

    if (!pnl_breach && !pos_breach) return;

    is_shutdown_ = true;

    if (pnl_breach) {
        std::ostringstream ss;
        ss << "SHUTDOWN triggered: PNL " << pnl_tracker_.get_total_pnl()
           << " < min_pnl " << limits_.min_pnl;
        log(ss.str());
    }
    if (pos_breach) {
        std::ostringstream ss;
        ss << "SHUTDOWN triggered: |position| "
           << std::abs(position_tracker_.get_position())
           << " > max_position " << limits_.max_position;
        log(ss.str());
    }

    in_shutdown_ = true;
    cancel_all_open_orders();
    in_shutdown_ = false;

    log("SHUTDOWN complete. All open orders cancelled.");
}
