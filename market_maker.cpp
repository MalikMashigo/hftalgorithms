#include "market_maker.h"
#include <iostream>
#include <cmath>

MarketMaker::MarketMaker(IOrderSender& sender,
                         SymbolManager& symbols,
                         uint32_t gold_id, uint32_t blue_id)
    : sender_(sender), symbols_(symbols),
      gold_id_(gold_id), blue_id_(blue_id) {}

void MarketMaker::on_book_update(uint32_t symbol_id) {
    if (symbol_id == gold_id_)
        requote(gold_id_, GOLD_TICK, gold_bid_order_id_, gold_ask_order_id_);
    else if (symbol_id == blue_id_)
        requote(blue_id_, BLUE_TICK, blue_bid_order_id_, blue_ask_order_id_);
}

void MarketMaker::requote(uint32_t symbol_id, int32_t tick_size,
                          std::atomic<uint64_t>& bid_oid, std::atomic<uint64_t>& ask_oid) {
    int32_t best_bid = symbols_.best_bid_price(symbol_id);
    int32_t best_ask = symbols_.best_ask_price(symbol_id);

    // Need a valid market to quote against
    if (best_bid <= 0 || best_ask <= 0) return;
    if (best_ask <= best_bid) return;

    int32_t mid = (best_bid + best_ask) / 2;
    int32_t half = SPREAD_TICKS * tick_size;

    int32_t our_bid = round_to_tick(mid - half, tick_size);
    int32_t our_ask = round_to_tick(mid + half, tick_size);

    // Don't quote if we'd cross or be inside the market
    if (our_bid <= 0 || our_ask <= our_bid) return;

    int32_t position = symbols_.get_position(symbol_id);

    // Apply inventory skew to defend against toxic positions
    int32_t skew = position * tick_size;
    our_bid = round_to_tick(mid - half - skew, tick_size);
    our_ask = round_to_tick(mid + half + skew, tick_size);

    // Ensure bid/ask are still valid after skew
    if (our_bid <= 0 || our_ask <= our_bid) return;

    // ── Cancel and replace bid ──
    if (position < POS_LIMIT) {
        uint64_t old_bid = bid_oid.load(std::memory_order_acquire);
        if (old_bid != 0) {
            sender_.delete_order(old_bid);
            bid_oid.store(0, std::memory_order_release);
        }

        // Use a deterministic order ID based on symbol and side
        uint64_t new_oid = (static_cast<uint64_t>(symbol_id) << 32) | 0x00000001;
        if (sender_.send_new_order(new_oid, symbol_id, SIDE::BUY, QUOTE_SIZE, our_bid)) {
            bid_oid.store(new_oid, std::memory_order_release);
        }
    } else {
        // Cancel existing bid if position limit reached
        uint64_t old_bid = bid_oid.load(std::memory_order_acquire);
        if (old_bid != 0) {
            sender_.delete_order(old_bid);
            bid_oid.store(0, std::memory_order_release);
        }
    }

    // ── Cancel and replace ask ──
    if (position > -POS_LIMIT) {
        uint64_t old_ask = ask_oid.load(std::memory_order_acquire);
        if (old_ask != 0) {
            sender_.delete_order(old_ask);
            ask_oid.store(0, std::memory_order_release);
        }

        // Use a deterministic order ID based on symbol and side
        uint64_t new_oid = (static_cast<uint64_t>(symbol_id) << 32) | 0x00000002;
        if (sender_.send_new_order(new_oid, symbol_id, SIDE::SELL, QUOTE_SIZE, our_ask)) {
            ask_oid.store(new_oid, std::memory_order_release);
        }
    } else {
        // Cancel existing ask if position limit reached
        uint64_t old_ask = ask_oid.load(std::memory_order_acquire);
        if (old_ask != 0) {
            sender_.delete_order(old_ask);
            ask_oid.store(0, std::memory_order_release);
        }
    }
}

void MarketMaker::on_fill(uint32_t symbol_id, SIDE side, uint32_t qty, int32_t price) {
    // Position tracker should already be updated by OEClient fill callback
    // Requote immediately after a fill to stay in the market
    std::cout << "[MM] Fill: symbol=" << symbol_id
              << " side=" << (side == SIDE::BUY ? "BUY" : "SELL")
              << " qty=" << qty << " px=" << price << "\n";

    on_book_update(symbol_id);
}

int32_t MarketMaker::round_to_tick(int32_t price, int32_t tick) {
    if (price <= 0) return price;
    return (price / tick) * tick;
}
