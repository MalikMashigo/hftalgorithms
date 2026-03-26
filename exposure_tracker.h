#ifndef EXPOSURE_TRACKER_H
#define EXPOSURE_TRACKER_H

#include <cstdint>
#include <unordered_map>
#include "messages.h"
#include "position_tracker.h"

struct PendingOrder {
    SIDE side;
    uint32_t qty;
    int32_t price;
};

class ExposureTracker {
public:
    ExposureTracker(const PositionTracker& position_tracker, uint32_t max_exposure);

    // Called by OEClient when an order is sent
    void on_order_sent(uint64_t order_id, SIDE side, uint32_t qty, int32_t price);

    // Called when order is rejected or cancelled since we want to remove it from outstanding orders
    void on_order_removed(uint64_t order_id);

    // Called when an order is filled fill. In that case we reduce the outstanding qty by the filled amount
    void on_fill(uint64_t order_id, uint32_t filled_qty);

    // buy_exposure = position + total outstanding buy qty
    uint32_t get_buy_exposure() const;
    // sell_exposure = -position + total outstanding sell qty
    uint32_t get_sell_exposure() const;

    bool would_exceed_exposure(SIDE side, uint32_t qty) const;

    uint32_t get_outstanding_buy_qty() const { return outstanding_buy_qty_; }
    uint32_t get_outstanding_sell_qty() const { return outstanding_sell_qty_; }
    size_t get_outstanding_order_count() const { return pending_orders_.size(); }

private:
    const PositionTracker& position_tracker_;  // Need a reference to the position tracker to calculate exposure
    uint32_t max_exposure_;
    uint32_t outstanding_buy_qty_;
    uint32_t outstanding_sell_qty_;

    // Need to know side/qty when an order is removed or partially filled
    std::unordered_map<uint64_t, PendingOrder> pending_orders_;
};

#endif