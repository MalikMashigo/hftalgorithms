#ifndef IORDER_SENDER_H
#define IORDER_SENDER_H

#include <cstdint>
#include "messages.h"

// Abstract interface for order submission, used by RiskManager so that
// OEClient can be swapped for a mock in unit tests.
class IOrderSender {
public:
    virtual bool send_new_order(uint64_t order_id, uint32_t symbol, SIDE side,
                                uint32_t qty, int32_t price) = 0;
    virtual bool delete_order(uint64_t order_id) = 0;
    virtual bool modify_order(uint64_t order_id, SIDE side,
                              uint32_t qty, int32_t price) = 0;
    virtual ~IOrderSender() = default;
};

#endif
