#ifndef PNL_TRACKER_H
#define PNL_TRACKER_H

#include <cstdint>
#include "messages.h"

// Tracks profit and loss using mark-to-market accounting.
//
// Formula:
//   total_pnl = sell_revenue - buy_cost + position * mark_price
//
// The mark-to-market term (position * mark_price) correctly adjusts for open
// positions: a long position gains value when the mark rises, a short position
// when it falls.  When position == 0 the formula reduces to the standard
// realized PNL (sell_revenue - buy_cost).
class PnlTracker {
public:
    explicit PnlTracker(double min_pnl);

    // Call whenever a fill is received from the exchange.
    void on_fill(SIDE side, uint32_t qty, int32_t price);

    // Update the mid-market / last-trade price used for MTM valuation.
    void update_mark_price(int32_t mark_price);

    // sell_revenue - buy_cost (ignores open position, no MTM)
    double get_realized_pnl() const { return sell_revenue_ - buy_cost_; }

    // position * mark_price
    double get_unrealized_pnl() const;

    // Full mark-to-market PNL (realized + unrealized)
    double get_total_pnl() const;

    int32_t get_position() const { return position_; }

    // True when total MTM PNL has dropped below the configured minimum.
    bool below_min_pnl() const { return get_total_pnl() < min_pnl_; }

private:
    double  min_pnl_;
    int32_t position_;      // net position: +ve = long, -ve = short
    double  buy_cost_;      // cumulative sum of qty*price for all buys
    double  sell_revenue_;  // cumulative sum of qty*price for all sells
    int32_t mark_price_;
};

#endif
