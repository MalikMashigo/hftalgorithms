#include "etf_arb.h"
#include <iostream>
#include <algorithm>

ETFArb::ETFArb(SymbolManager& sm, OEClient& oe, ETFClient& etf,
               std::atomic<bool>& shutdown)
    : sm_(sm), oe_(oe), etf_(etf), global_shutdown_(shutdown)
{
    oe_.set_on_fill([this](const FillEvent& f) {
        auto it = order_map_.find(f.order_id);
        if (it == order_map_.end()) return;
        sm_.on_fill(it->second.first, it->second.second, f.qty, f.price);
        if (f.closed) order_map_.erase(it);
    });
}

void ETFArb::run() {
    std::cout << "[ETFArb] Starting arb loop\n";

    while (running_.load(std::memory_order_acquire)) {

        if (sm_.pnl_near_limit()) {
            std::cerr << "[ETFArb] PnL near limit — SHUTTING DOWN\n";
            oe_.cancel_all_open_orders();
            global_shutdown_.store(true, std::memory_order_release);
            return;
        }

        ArbSnapshot snap = sm_.snapshot();

        // Debug — remove after confirming trades are firing
        static int tick = 0;
        if (++tick % 100000 == 0) {
            bool creation_viable  = !snap.any_dorm_ask_missing &&
                                     snap.undy_best_bid_price > 0 &&
                                    (snap.undy_best_bid_price - snap.nav_ask) > MIN_EDGE;
            bool redemption_viable = !snap.any_dorm_bid_missing &&
                                      snap.undy_best_ask_price > 0 &&
                                     (snap.nav_bid - snap.undy_best_ask_price) > MIN_EDGE;
            std::cout << "[ARB] creation_viable="  << creation_viable
                      << " redemption_viable="     << redemption_viable
                      << " creation_edge="         << (snap.undy_best_bid_price - snap.nav_ask)
                      << " redemption_edge="       << (snap.nav_bid - snap.undy_best_ask_price)
                      << " missing_asks="          << snap.any_dorm_ask_missing
                      << "\n";
            for (size_t i = 0; i < DORM_IDS.size(); ++i)
                if (snap.dorms[i].best_ask_price == 0)
                    std::cout << "[ARB] missing ask: sym=" << DORM_IDS[i] << "\n";
        }

        if (!try_creation_arb(snap))
            try_redemption_arb(snap);
    }

    std::cout << "[ETFArb] Loop stopped\n";
}

bool ETFArb::try_creation_arb(const ArbSnapshot& snap) {
    if (snap.any_dorm_ask_missing || snap.undy_best_bid_price == 0) return false;

    int32_t edge = snap.undy_best_bid_price - snap.nav_ask;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = creation_qty(snap);
    if (qty <= 0) return false;

    std::cout << "[ETFArb] CREATION arb: edge=" << edge
              << " qty=" << qty << "\n";

    // Step 1: fire all 10 dorm buys
    std::array<uint64_t, 10> order_ids;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        order_ids[i] = next_id();
        order_map_[order_ids[i]] = {DORM_IDS[i], SIDE::BUY};
        oe_.send_new_order_no_wait(order_ids[i], DORM_IDS[i],
                                   SIDE::BUY, static_cast<uint32_t>(qty),
                                   snap.dorms[i].best_ask_price);
    }

    // Step 2: collect ACKs
    int filled = 0;
    for (int i = 0; i < 10; ++i)
        if (oe_.wait_for_response()) ++filled;

    if (filled < 10) {
        std::cerr << "[ETFArb] Only " << filled << "/10 legs filled — unwinding\n";
        unwind_dorm_longs();
        return true;
    }

    // Step 3: pre-flight check before /create
    bool all_filled = true;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        if (sm_.get_position(DORM_IDS[i]) < qty) {
            std::cerr << "[ETFArb] Pre-flight failed: sym=" << DORM_IDS[i] << "\n";
            all_filled = false;
        }
    }
    if (!all_filled) {
        unwind_dorm_longs();
        return true;
    }

    // Step 4: create UNDY
    ETFResult r = etf_.create(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /create failed: " << r.message << " — unwinding\n";
        unwind_dorm_longs();
        return true;
    }

    std::cout << "[ETFArb] /create OK, undy_balance=" << r.undy_balance << "\n";

    // Step 5: sell UNDY
    uint64_t undy_oid = next_id();
    order_map_[undy_oid] = {SYM_UNDY, SIDE::SELL};
    oe_.send_new_order(undy_oid, SYM_UNDY,
                       SIDE::SELL, static_cast<uint32_t>(qty),
                       snap.undy_best_bid_price);
    return true;
}

bool ETFArb::try_redemption_arb(const ArbSnapshot& snap) {
    if (snap.any_dorm_bid_missing || snap.undy_best_ask_price == 0) return false;

    int32_t edge = snap.nav_bid - snap.undy_best_ask_price;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = redemption_qty(snap);
    if (qty <= 0) return false;

    std::cout << "[ETFArb] REDEMPTION arb: edge=" << edge
              << " qty=" << qty << "\n";

    // Step 1: buy UNDY
    uint64_t undy_oid = next_id();
    order_map_[undy_oid] = {SYM_UNDY, SIDE::BUY};
    bool bought = oe_.send_new_order(undy_oid, SYM_UNDY,
                                     SIDE::BUY, static_cast<uint32_t>(qty),
                                     snap.undy_best_ask_price);
    if (!bought) {
        std::cerr << "[ETFArb] UNDY buy rejected\n";
        return false;
    }

    // Step 2: redeem
    ETFResult r = etf_.redeem(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /redeem failed: " << r.message << "\n";
        return true;
    }

    std::cout << "[ETFArb] /redeem OK, undy_balance=" << r.undy_balance << "\n";

    // Step 3: sell all 10 dorms
    std::array<uint64_t, 10> order_ids;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        order_ids[i] = next_id();
        order_map_[order_ids[i]] = {DORM_IDS[i], SIDE::SELL};
        oe_.send_new_order_no_wait(order_ids[i], DORM_IDS[i],
                                   SIDE::SELL, static_cast<uint32_t>(qty),
                                   snap.dorms[i].best_bid_price);
    }
    for (int i = 0; i < 10; ++i)
        oe_.wait_for_response();

    return true;
}

void ETFArb::unwind_dorm_longs() {
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        int32_t pos = sm_.get_position(DORM_IDS[i]);
        if (pos <= 0) continue;
        int32_t bid = sm_.best_bid_price(DORM_IDS[i]);
        if (bid <= 0) continue;
        uint64_t oid = next_id();
        order_map_[oid] = {DORM_IDS[i], SIDE::SELL};
        oe_.send_new_order_no_wait(oid, DORM_IDS[i],
                                   SIDE::SELL,
                                   static_cast<uint32_t>(pos), bid);
    }
    for (size_t i = 0; i < DORM_IDS.size(); ++i)
        oe_.wait_for_response();
}

int32_t ETFArb::creation_qty(const ArbSnapshot& snap) const {
    int32_t qty = static_cast<int32_t>(snap.undy_best_bid_qty);
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        const auto& d = snap.dorms[i];
        qty = std::min(qty, static_cast<int32_t>(d.best_ask_qty));
        qty = std::min(qty, SymbolManager::POSITION_LIMIT - d.position);
    }
    qty = std::min(qty, SymbolManager::POSITION_LIMIT + snap.undy_position);
    return qty;
}

int32_t ETFArb::redemption_qty(const ArbSnapshot& snap) const {
    int32_t qty = static_cast<int32_t>(snap.undy_best_ask_qty);
    qty = std::min(qty, SymbolManager::POSITION_LIMIT - snap.undy_position);
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        const auto& d = snap.dorms[i];
        qty = std::min(qty, static_cast<int32_t>(d.best_bid_qty));
        qty = std::min(qty, SymbolManager::POSITION_LIMIT + d.position);
    }
    return qty;
}