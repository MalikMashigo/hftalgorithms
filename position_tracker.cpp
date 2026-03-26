#include "position_tracker.h"
#include <cstdlib>

PositionTracker::PositionTracker(int32_t max_position)
    : position_(0), max_position_(max_position) {}

// Position update: buys add and sells subtract
void PositionTracker::on_fill(SIDE side, uint32_t qty) {
    if (side == SIDE::BUY) {
        position_ += qty;
    } else {
        position_ -= qty;
    }
}

// Used by risk system to check before sending an order
// Returns true if sending this order would push position over the limit
bool PositionTracker::would_exceed_limit(SIDE side, uint32_t qty) const {
    int32_t new_position = position_;
    if (side == SIDE::BUY) {
        new_position += qty;
    } else {
        new_position -= qty;
    }
    return std::abs(new_position) > max_position_;
}