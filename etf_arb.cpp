#include "etf_arb.h"
#include <iostream>
#include <algorithm>
#include <array>

ETFArb::ETFArb(SymbolManager& sm, OEClient& oe, ETFClient& etf)
    : sm_(sm), oe_(oe), etf_(etf) {
        oe_.set_on_fill([this](const FillEvent& f) {
        auto it = order_map_.find(f.order_id);
        if (it == order_map_.end()) return;
        sm_.on_fill(it->second.first,   // symbol
                    it->second.second,  // side
                    f.qty,
                    f.price);
        if (f.closed) order_map_.erase(it);
    });
    }

// ── Main loop ─────────────────────────────────────────────────────────────────
// Spins as fast as possible reading snapshots and checking for arb.
// No sleep — every microsecond counts.

void ETFArb::run() {
    if constexpr (DEBUG_LOG) {
        std::cout << "[ETFArb] Starting arb loop\n";
    }

    while (running_.load(std::memory_order_acquire)) {

        // Hard stop if PnL approaching the -5000 floor
        if (sm_.pnl_near_limit()) {
            std::cerr << "[ETFArb] PnL near limit — halting all trading\n";
            oe_.cancel_all_open_orders();
            return;
        }

        ArbSnapshot snap = sm_.snapshot();

        // Try both directions on every tick
        // creation first (typically higher edge when UNDY is overpriced)
        if (!try_creation_arb(snap)) {
            try_redemption_arb(snap);
        }
    }

    if constexpr (DEBUG_LOG) {
    std::cout << "[ETFArb] Loop stopped\n";
    }
}

// ── Creation arb ──────────────────────────────────────────────────────────────
// Opportunity: UNDY bid > sum(dorm asks) + MIN_EDGE
// Action:      buy all 10 dorms at ask → /create → sell UNDY at bid

bool ETFArb::try_creation_arb(const ArbSnapshot& snap) {
    // Bail early if any dorm has no ask or UNDY has no bid
    if (snap.any_dorm_ask_missing || snap.undy_best_bid_price == 0) return false;

    int32_t edge = snap.undy_best_bid_price - snap.nav_ask;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = creation_qty(snap);
    if (qty <= 0) return false;

    if constexpr (DEBUG_LOG) {
        std::cout << "[ETFArb] CREATION arb: edge=" << edge
              << " qty=" << qty
              << " nav_ask=" << snap.nav_ask
              << " undy_bid=" << snap.undy_best_bid_price << "\n";
    }

    // ── Step 1: fire all 10 dorm buys without waiting ─────────────────────
    std::array<uint64_t, 10> order_ids;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        order_ids[i] = next_id();
        order_map_[order_ids[i]] = {DORM_IDS[i], SIDE::BUY}; 
        oe_.send_new_order_no_wait(order_ids[i],
                                   DORM_IDS[i],
                                   SIDE::BUY,
                                   static_cast<uint32_t>(qty),
                                   snap.dorms[i].best_ask_price);
    }

    // ── Step 2: collect all 10 ACKs ───────────────────────────────────────
    int filled = 0;
    for (int i = 0; i < 10; ++i) {
        if (oe_.wait_for_response()) ++filled;
    }

    if (filled < 10) {
        // Some legs rejected — we have a partial position.
        // This is the main leg risk. Log it and let the arb loop
        // naturally unwind via the redemption direction.
        std::cerr << "[ETFArb] WARNING: only " << filled
                  << "/10 dorm legs filled. Partial position held.\n";
        return true;  // still counts as attempted
    }

    // ── Step 3: create UNDY ───────────────────────────────────────────────
    ETFResult r = etf_.create(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /create failed: " << r.message << "\n";
        return true;
    }

    if constexpr (DEBUG_LOG) {
        std::cout << "[ETFArb] /create OK, undy_balance=" << r.undy_balance << "\n";
    }

    // ── Step 4: sell UNDY at bid ──────────────────────────────────────────
    uint64_t undy_oid = next_id();
    order_map_[undy_oid] = {SYM_UNDY, SIDE::SELL}; 
    bool sold = oe_.send_new_order(undy_oid,
                                   SYM_UNDY,
                                   SIDE::SELL,
                                   static_cast<uint32_t>(qty),
                                   snap.undy_best_bid_price);

    if (!sold) {
        std::cerr << "[ETFArb] WARNING: UNDY sell rejected — holding UNDY inventory\n";
    }

    return true;
}

// ── Redemption arb ────────────────────────────────────────────────────────────
// Opportunity: sum(dorm bids) - UNDY ask > MIN_EDGE
// Action:      buy UNDY at ask → /redeem → sell all 10 dorms at bid

bool ETFArb::try_redemption_arb(const ArbSnapshot& snap) {
    if (snap.any_dorm_bid_missing || snap.undy_best_ask_price == 0) return false;

    int32_t edge = snap.nav_bid - snap.undy_best_ask_price;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = redemption_qty(snap);
    if (qty <= 0) return false;

    if constexpr (DEBUG_LOG) {
        std::cout << "[ETFArb] REDEMPTION arb: edge=" << edge
              << " qty=" << qty
              << " nav_bid=" << snap.nav_bid
              << " undy_ask=" << snap.undy_best_ask_price << "\n";
    }

    // ── Step 1: buy UNDY ──────────────────────────────────────────────────
    uint64_t undy_oid = next_id();
    order_map_[undy_oid] = {SYM_UNDY, SIDE::BUY};  
    bool bought = oe_.send_new_order(undy_oid,
                                     SYM_UNDY,
                                     SIDE::BUY,
                                     static_cast<uint32_t>(qty),
                                     snap.undy_best_ask_price);
    if (!bought) {
        std::cerr << "[ETFArb] UNDY buy rejected\n";
        return false;
    }

    // ── Step 2: redeem UNDY → underlyings ────────────────────────────────
    ETFResult r = etf_.redeem(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /redeem failed: " << r.message << "\n";
        return true;
    }

    if constexpr (DEBUG_LOG) {
        std::cout << "[ETFArb] /redeem OK, undy_balance=" << r.undy_balance << "\n";
    }

    // ── Step 3: sell all 10 dorms simultaneously ──────────────────────────
    std::array<uint64_t, 10> order_ids;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        order_ids[i] = next_id();
        order_map_[order_ids[i]] = {DORM_IDS[i], SIDE::SELL};
        oe_.send_new_order_no_wait(order_ids[i],
                                   DORM_IDS[i],
                                   SIDE::SELL,
                                   static_cast<uint32_t>(qty),
                                   snap.dorms[i].best_bid_price);
    }

    for (int i = 0; i < 10; ++i) {
        oe_.wait_for_response();
    }

    return true;
}

// ── Position-aware qty calculation ────────────────────────────────────────────

int32_t ETFArb::creation_qty(const ArbSnapshot& snap) const {
    int32_t qty = static_cast<int32_t>(snap.undy_best_bid_qty);

    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        const auto& d = snap.dorms[i];

        // Can't buy more than available depth
        qty = std::min(qty, static_cast<int32_t>(d.best_ask_qty));

        // Can't exceed position limit on any dorm leg
        int32_t room = SymbolManager::POSITION_LIMIT - d.position;
        qty = std::min(qty, room);
    }

    // Can't exceed UNDY position limit on the sell side
    int32_t undy_room = SymbolManager::POSITION_LIMIT + snap.undy_position;
    qty = std::min(qty, undy_room);

    return qty;
}

int32_t ETFArb::redemption_qty(const ArbSnapshot& snap) const {
    int32_t qty = static_cast<int32_t>(snap.undy_best_ask_qty);

    // Can't buy more UNDY than position limit allows
    int32_t undy_room = SymbolManager::POSITION_LIMIT - snap.undy_position;
    qty = std::min(qty, undy_room);

    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        const auto& d = snap.dorms[i];

        // Can't sell more than available bid depth
        qty = std::min(qty, static_cast<int32_t>(d.best_bid_qty));

        // Can't go shorter than -POSITION_LIMIT on any dorm
        int32_t room = SymbolManager::POSITION_LIMIT + d.position;
        qty = std::min(qty, room);
    }

    return qty;
}