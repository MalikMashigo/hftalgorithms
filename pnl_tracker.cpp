#include "pnl_tracker.h"

PnlTracker::PnlTracker(double min_pnl)
    : min_pnl_(min_pnl)
    , position_(0)
    , buy_cost_(0.0)
    , sell_revenue_(0.0)
    , mark_price_(0) {}

void PnlTracker::on_fill(SIDE side, uint32_t qty, int32_t price) {
    if (side == SIDE::BUY) {
        position_ += static_cast<int32_t>(qty);
        buy_cost_  += static_cast<double>(qty) * price;
    } else {
        position_      -= static_cast<int32_t>(qty);
        sell_revenue_  += static_cast<double>(qty) * price;
    }
}

void PnlTracker::update_mark_price(int32_t mark_price) {
    mark_price_ = mark_price;
}

double PnlTracker::get_unrealized_pnl() const {
    return static_cast<double>(position_) * mark_price_;
}

double PnlTracker::get_total_pnl() const {
    return sell_revenue_ - buy_cost_ + static_cast<double>(position_) * mark_price_;
}
