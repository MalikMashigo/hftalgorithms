#pragma once

#include <cstdint>
#include <unordered_map>
#include <atomic>
#include "iorder_sender.h"
#include "symbol_manager.h"
#include "messages.h"

class MarketMaker {
public:
    MarketMaker(IOrderSender& sender,
                SymbolManager& symbols,
                uint32_t gold_id,
                uint32_t blue_id);

    void on_book_update(uint32_t symbol_id);
    void on_fill(uint32_t symbol_id, SIDE side, uint32_t qty, int32_t price);

private:
    IOrderSender& sender_;
    SymbolManager& symbols_;

    uint32_t gold_id_;
    uint32_t blue_id_;

    // Track our resting order IDs so we can cancel/replace
    std::atomic<uint64_t> gold_bid_order_id_{0};
    std::atomic<uint64_t> gold_ask_order_id_{0};
    std::atomic<uint64_t> blue_bid_order_id_{0};
    std::atomic<uint64_t> blue_ask_order_id_{0};

    static constexpr int32_t GOLD_TICK      = 10;
    static constexpr int32_t BLUE_TICK      = 5;
    static constexpr int32_t SPREAD_TICKS   = 2;   // quote 2 ticks each side
    static constexpr uint32_t QUOTE_SIZE    = 1;    // shares per side
    static constexpr int32_t POS_LIMIT      = 8;    // matches MM_POSITION_LIMIT

    void requote(uint32_t symbol_id, int32_t tick_size,
                 std::atomic<uint64_t>& bid_oid, std::atomic<uint64_t>& ask_oid);

    int32_t round_to_tick(int32_t price, int32_t tick);
};
