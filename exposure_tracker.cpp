#include "exposure_tracker.h"
#include <iostream>

ExposureTracker::ExposureTracker(const PositionTracker& position_tracker, uint32_t max_exposure)
    : position_tracker_(position_tracker)
    , max_exposure_(max_exposure)
    , outstanding_buy_qty_(0)
    , outstanding_sell_qty_(0) {
    pending_orders_.reserve(1000);
}

// After sending an order, add it to pending and update outstanding qtys
void ExposureTracker::on_order_sent(uint64_t order_id, SIDE side, uint32_t qty, int32_t price) {
    pending_orders_[order_id] = {side, qty, price};
    if (side == SIDE::BUY) {
        outstanding_buy_qty_ += qty;
    } else {
        outstanding_sell_qty_ += qty;
    }
}

// When the order is rejected or canceled, remove the order entirely from outstanding
void ExposureTracker::on_order_removed(uint64_t order_id) {
    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) return;

    if (it->second.side == SIDE::BUY) {
        outstanding_buy_qty_ -= it->second.qty;
    } else {
        outstanding_sell_qty_ -= it->second.qty;
    }
    pending_orders_.erase(it);
}

// When the order is filled, reduce outstanding by filled qty
// If the order is fully filled, remove the order entirely
void ExposureTracker::on_fill(uint64_t order_id, uint32_t filled_qty) {
    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) return;

    if (it->second.side == SIDE::BUY) {
        outstanding_buy_qty_ -= filled_qty;
    } else {
        outstanding_sell_qty_ -= filled_qty;
    }

    it->second.qty -= filled_qty;

    // Clean up if fully filled
    if (it->second.qty == 0) {
        pending_orders_.erase(it);
    }
}

// buy exposure - calculates how long we could end up if all buys fill
uint32_t ExposureTracker::get_buy_exposure() const {
    int32_t pos = position_tracker_.get_position();
    return static_cast<uint32_t>(std::max(0, pos) + outstanding_buy_qty_);
}

// sell exposure - calculates how short we could end up if all sells fill
uint32_t ExposureTracker::get_sell_exposure() const {
    int32_t pos = position_tracker_.get_position();
    return static_cast<uint32_t>(std::max(0, -pos) + outstanding_sell_qty_);
}

// Exposure check to be used by risk system before sending
bool ExposureTracker::would_exceed_exposure(SIDE side, uint32_t qty) const {
    if (side == SIDE::BUY) {
        return (get_buy_exposure() + qty) > max_exposure_;
    } else {
        return (get_sell_exposure() + qty) > max_exposure_;
    }
}