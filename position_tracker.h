#ifndef POSITION_TRACKER_H
#define POSITION_TRACKER_H

#include <cstdint>
#include "messages.h"

class PositionTracker {
public:
    PositionTracker(int32_t max_position);

    // Called by OEClient when a fill comes in
    void on_fill(SIDE side, uint32_t qty);

    int32_t get_position() const { return position_; }
    bool at_limit() const { return std::abs(position_) >= max_position_; }
    bool would_exceed_limit(SIDE side, uint32_t qty) const;

private:
    int32_t position_;      // positive = net long, negative = net short
    int32_t max_position_;  // absolute position limit
};

#endif