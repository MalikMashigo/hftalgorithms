#include "pnl_tracker.h"

PnlTracker::PnlTracker(double min_pnl)
    : min_pnl_(min_pnl)
    , position_(0)
    , buy_cost_(0.0)
    , sell_revenue_(0.0)
    , mark_price_(0) {}

// Called by RiskManager::on_fill() every time the exchange confirms a fill.
void PnlTracker::on_fill(SIDE side, uint32_t qty, int32_t price) {
    if (side == SIDE::BUY) {
        position_ += static_cast<int32_t>(qty);
        buy_cost_  += static_cast<double>(qty) * price;
    } else {
        position_      -= static_cast<int32_t>(qty);
        sell_revenue_  += static_cast<double>(qty) * price;
    }
}

// Called by the market data listener whenever the best bid/ask updates.
void PnlTracker::update_mark_price(int32_t mark_price) {
    mark_price_ = mark_price;
}

// Returns the profit/loss on the current open position relative to its average entry price
// For a long position:  avg_cost = buy_cost / position
// For a short position: avg_cost = sell_revenue_ / (-position_)
// unrealized = position * (mark_price - avg_cost)
double PnlTracker::get_unrealized_pnl() const {
    if (position_ == 0) return 0.0;
    double avg_cost = (position_ > 0) ? buy_cost_ / position_ : sell_revenue_ / (-position_);
    return static_cast<double>(position_) * (mark_price_ - avg_cost);
}

// Returns total PNL = realized + unrealized
// Computed as sell_revenue_ - buy_cost_ + position_ * mark_price_
// Where sell_revenue_ - buy_cost_ is the realized PNL from completed round trips
// and position_ * mark_price_  is the gross mark-to-market value of open position
double PnlTracker::get_total_pnl() const {
    return sell_revenue_ - buy_cost_ + static_cast<double>(position_) * mark_price_;
}
