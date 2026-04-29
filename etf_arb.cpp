#include "etf_arb.h"
#include <iostream>
#include <algorithm>

ETFArb::ETFArb(SymbolManager& sm, OEClient& oe, ETFClient& etf,
               std::atomic<bool>& shutdown, OrderMap& mm_order_map)
    : sm_(sm), oe_(oe), etf_(etf), global_shutdown_(shutdown),
      mm_order_map_(mm_order_map)
{
    oe_.set_on_fill([this](const FillEvent& f) {
        std::cout << "[FILL] order=" << f.order_id
                  << " qty=" << f.qty
                  << " price=" << f.price << "\n";

        // Check arb orders first
        auto arb_it = order_map_.find(f.order_id);
        if (arb_it != order_map_.end()) {
            sm_.on_fill(arb_it->second.first,
                        arb_it->second.second,
                        f.qty, f.price);
            if (f.closed) order_map_.erase(arb_it);
            return;
        }

        // Then check MM orders
        auto mm_it = mm_order_map_.find(f.order_id);
        if (mm_it != mm_order_map_.end()) {
            sm_.on_fill(mm_it->second.first,
                        mm_it->second.second,
                        f.qty, f.price);
            if (f.closed) {
                mm_order_map_.erase(mm_it);
                if (f.order_id == blue_bid_id_) blue_bid_id_ = 0;
                if (f.order_id == blue_ask_id_) blue_ask_id_ = 0;
            }
            return;
        }

        std::cerr << "[FILL] WARNING: unknown order_id=" << f.order_id << "\n";
    });
}

void ETFArb::run() {
  std::cout << "[ETFArb] Starting arb loop\n";

    while (running_.load(std::memory_order_acquire)) {

        // ── Global PnL guard ──────────────────────────────────────────────
        if (sm_.pnl_near_limit()) {
            std::cerr << "[ETFArb] PnL near limit — unwinding and going dormant\n";
            oe_.cancel_all_open_orders();

            for (uint32_t id = 1; id <= 13; ++id) {
                int32_t pos = sm_.get_position(id);
                if (pos == 0) continue;
                SIDE side     = pos > 0 ? SIDE::SELL : SIDE::BUY;
                int32_t price = pos > 0 ? sm_.best_bid_price(id)
                                        : sm_.best_ask_price(id);
                if (price <= 0) continue;
                uint64_t oid = next_id();
                order_map_[oid] = {id, side};
                oe_.send_new_order(oid, id, side,
                                   static_cast<uint32_t>(std::abs(pos)), price);
            }

            // Wait until flat then resume
            while (true) {
                bool flat = true;
                for (uint32_t id = 1; id <= 13; ++id)
                    if (sm_.get_position(id) != 0) { flat = false; break; }
                if (flat) break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            std::cerr << "[ETFArb] Flat — resuming\n";
            continue;
        }

        // ── Wait for previous arb to complete ────────────────────────────
        if (arb_in_progress_.load(std::memory_order_acquire)) {
            bool all_flat = true;
            for (uint32_t id : DORM_IDS)
                if (sm_.get_position(id) != 0) { all_flat = false; break; }
            if (sm_.get_position(SYM_UNDY) != 0) all_flat = false;

            if (all_flat) {
                arb_in_progress_.store(false, std::memory_order_release);
                std::cout << "[ETFArb] Flat — ready for next arb\n";
                continue;
            }

            // Timeout after 3s — force unwind
            auto elapsed = std::chrono::steady_clock::now() - arb_start_time_;
            if (elapsed > std::chrono::seconds(3)) {
                std::cerr << "[ETFArb] Arb timeout — force unwinding\n";
                unwind_dorm_longs();

                int32_t undy_pos = sm_.get_position(SYM_UNDY);
                if (undy_pos > 0) {
                    int32_t bid = sm_.best_bid_price(SYM_UNDY);
                    if (bid > 0) {
                        uint64_t oid = next_id();
                        order_map_[oid] = {SYM_UNDY, SIDE::SELL};
                        oe_.send_new_order(oid, SYM_UNDY, SIDE::SELL,
                                           static_cast<uint32_t>(undy_pos), bid);
                    }
                }
                arb_in_progress_.store(false, std::memory_order_release);
            }
            continue;
        }

        // ── Arb opportunities ─────────────────────────────────────────────
        ArbSnapshot snap = sm_.snapshot();

        if (!try_creation_arb(snap))
            try_redemption_arb(snap);

        // ── Debug ─────────────────────────────────────────────────────────
        static int tick = 0;
        if (++tick % 100000 == 0) {
            std::cout << "[ARB] creation_edge="
                      << (snap.undy_best_bid_price - snap.nav_ask)
                      << " redemption_edge="
                      << (snap.nav_bid - snap.undy_best_ask_price)
                      << " missing_asks=" << snap.any_dorm_ask_missing
                      << " in_progress=" << arb_in_progress_.load()
                      << "\n";
        }
    }

    std::cout << "[ETFArb] Loop stopped\n";
}

bool ETFArb::try_creation_arb(const ArbSnapshot& snap) {
    if (snap.any_dorm_ask_missing || snap.undy_best_bid_price == 0) return false;
    if (arb_in_progress_.load(std::memory_order_acquire)) return false; 

    int32_t edge = snap.undy_best_bid_price - snap.nav_ask;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = creation_qty(snap);
    if (qty <= 0) return false;

    arb_in_progress_.store(true, std::memory_order_release);
    arb_start_time_ = std::chrono::steady_clock::now();

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
    int acked = 0;
    for (int i = 0; i < 10; ++i) {
    if (oe_.wait_for_response(order_ids[i])) {
        ++acked;
        std::cout << "[ETFArb] Leg " << acked << "/10 ACK'd\n";
    } else {
        std::cerr << "[ETFArb] Leg " << (acked + 1) << "/10 REJECTED\n";
    }
}
if (acked < 10) {
    std::cerr << "[ETFArb] Only " << acked << "/10 legs ACK'd — unwinding\n";
    unwind_dorm_longs();
    return true;
}
std::cout << "[ETFArb] All 10 legs ACK'd — polling for fills\n";

    // int filled = 0;
    // for (int i = 0; i < 10; ++i)
    //     if (oe_.wait_for_response()) ++filled;

    // if (filled < 10) {
    //     std::cerr << "[ETFArb] Only " << filled << "/10 legs filled — unwinding\n";
    //     unwind_dorm_longs();
    //     return true;
    // }


    // Step 3: pre-flight check before /create
    {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(5000);
    while (true) {
        bool all_filled = true;
        for (size_t i = 0; i < DORM_IDS.size(); ++i) {
            int32_t pos = sm_.get_position(DORM_IDS[i]);
            if (pos < qty) {
                all_filled = false;
                // Print once per check so you can see which leg is slow
                std::cout << "[ETFArb] Waiting on sym=" << DORM_IDS[i]
                          << " pos=" << pos << " need=" << qty << "\n";
            }
        }
        if (all_filled) {
            std::cout << "[ETFArb] All 10 dorm fills confirmed — proceeding to /create\n";
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "[ETFArb] Dorm fill timeout after 2s — dumping positions:\n";
            for (uint32_t id : DORM_IDS)
                std::cerr << "  sym=" << id
                          << " pos=" << sm_.get_position(id)
                          << " need=" << qty << "\n";
            std::cerr << "[ETFArb] Unwinding partial fills\n";
            unwind_dorm_longs();
            arb_in_progress_.store(false, std::memory_order_release);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
    // bool all_filled = true;
    // for (size_t i = 0; i < DORM_IDS.size(); ++i) {
    //     if (sm_.get_position(DORM_IDS[i]) < qty) {
    //         std::cerr << "[ETFArb] Pre-flight failed: sym=" << DORM_IDS[i] << "\n";
    //         all_filled = false;
    //     }
    // }
    // if (!all_filled) {
    //     unwind_dorm_longs();
    //     return true;
    // }

    // Step 4: create UNDY
    ETFResult r = etf_.create(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /create failed: " << r.message << " — unwinding\n";
        unwind_dorm_longs();
        return true;
    }

    // Manually update SymbolManager — /create doesn't generate fills
for (uint32_t id : DORM_IDS) {
    sm_.on_fill(id, SIDE::SELL, static_cast<uint32_t>(qty),
                sm_.best_bid_price(id));  // use current bid as notional price
}
sm_.on_fill(SYM_UNDY, SIDE::BUY, static_cast<uint32_t>(qty),
            snap.undy_best_bid_price);

    std::cout << "[ETFArb] /create OK, undy_balance=" << r.undy_balance << "\n";

    // Step 5: sell UNDY until flat
    int32_t undy_pos = sm_.get_position(SYM_UNDY);
    int32_t attempts = 0;

    while (undy_pos > 0 && attempts < 5) {
        int32_t bid = sm_.best_bid_price(SYM_UNDY);
        if (bid <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        undy_pos = sm_.get_position(SYM_UNDY);
        continue;
        }


        uint64_t undy_oid = next_id();
        order_map_[undy_oid] = {SYM_UNDY, SIDE::SELL};
        oe_.send_new_order(undy_oid, SYM_UNDY,
                       SIDE::SELL, 
                       static_cast<uint32_t>(undy_pos),
                       bid); 
        
        // Wait briefly then check if position closed
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        undy_pos = sm_.get_position(SYM_UNDY);
        ++attempts;
    }

    if (undy_pos > 0) {
    std::cerr << "[ETFArb] WARNING: " << undy_pos 
              << " UNDY lots still unsold after " << attempts << " attempts\n";
    }

    return true;
}

bool ETFArb::try_redemption_arb(const ArbSnapshot& snap) {
    if (snap.any_dorm_bid_missing || snap.undy_best_ask_price == 0) return false;
    if (arb_in_progress_.load(std::memory_order_acquire)) return false; 

    int32_t edge = snap.nav_bid - snap.undy_best_ask_price;
    if (edge <= MIN_EDGE) return false;

    int32_t qty = redemption_qty(snap);
    if (qty <= 0) return false;

    arb_in_progress_.store(true, std::memory_order_release);
    arb_start_time_ = std::chrono::steady_clock::now();

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
        arb_in_progress_.store(false, std::memory_order_release);
        return false;
    }

    std::cout << "[ETFArb] UNDY buy ACK'd — waiting for fill (need pos >= "
          << qty << ")\n";
    
          
    if (!oe_.wait_for_fill(undy_oid)) {
    std::cerr << "[ETFArb] UNDY fill failed/timeout — aborting redemption\n";
    arb_in_progress_.store(false, std::memory_order_release);
    return true;
    }
    std::cout << "[ETFArb] UNDY fill confirmed pos="
          << sm_.get_position(SYM_UNDY) << " — proceeding to /redeem\n";  

    // Step 2: redeem
    ETFResult r = etf_.redeem(qty);
    if (!r.success) {
        std::cerr << "[ETFArb] /redeem failed: " << r.message << "\n";
        return true;
    }

    // Manually update SymbolManager — /redeem doesn't generate fills
sm_.on_fill(SYM_UNDY, SIDE::SELL, static_cast<uint32_t>(qty),
            snap.undy_best_ask_price);
for (uint32_t id : DORM_IDS) {
    sm_.on_fill(id, SIDE::BUY, static_cast<uint32_t>(qty),
                sm_.best_ask_price(id));
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
        oe_.wait_for_response(order_ids[i]);

    return true;
}

void ETFArb::unwind_dorm_longs() {
    std::array<uint64_t, 10> sent_ids;
    std::array<uint32_t, 10> sent_syms;
    int sent = 0;
    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        int32_t pos = sm_.get_position(DORM_IDS[i]);
        //if (pos <= 0) continue;
        if (pos > 0) {
            int32_t bid = sm_.best_bid_price(DORM_IDS[i]);
            if (bid <= 0) {
            std::cerr << "[ETFArb] unwind_dorm_longs: no bid for sym="
                      << DORM_IDS[i] << " pos=" << pos << " — skipping\n";
            continue;
            }
            uint64_t oid = next_id();
            order_map_[oid] = {DORM_IDS[i], SIDE::SELL};
            oe_.send_new_order_no_wait(oid, DORM_IDS[i],
                                   SIDE::SELL,
                                   static_cast<uint32_t>(pos), bid);
            sent_ids[sent]  = oid;
            sent_syms[sent] = DORM_IDS[i];
            ++sent;
            std::cout << "[ETFArb] unwind_dorm_longs: sent sell sym="
                  << DORM_IDS[i] << " pos=" << pos
                  << " @ " << bid << " order_id=" << oid << "\n";
        }
        else if (pos <= -8) {
            int32_t ask = sm_.best_ask_price(DORM_IDS[i]);
            if (ask <= 0) {
                std::cerr << "[ETFArb] unwind_dorm_longs: no ask for sym="
                          << DORM_IDS[i] << " pos=" << pos << " — skipping\n";
                continue;
            }
            std::cerr << "[ETFArb] unwind_dorm_longs: sym=" << DORM_IDS[i]
                      << " short pos=" << pos << " near limit — emergency cover\n";
            uint64_t oid = next_id();
            order_map_[oid] = {DORM_IDS[i], SIDE::BUY};
            oe_.send_new_order_no_wait(oid, DORM_IDS[i], SIDE::BUY,
                                       static_cast<uint32_t>(std::abs(pos)), ask);
            sent_ids[sent]  = oid;
            sent_syms[sent] = DORM_IDS[i];
            ++sent;
        }
    }
    for (int i = 0; i < sent; ++i) {
        if(oe_.wait_for_response(sent_ids[i])){
             std::cout << "[ETFArb] unwind_dorm_longs: confirmed sym="
                      << sent_syms[i] << " order_id=" << sent_ids[i] << "\n";
        }
        else {
            std::cerr << "[ETFArb] unwind_dorm_longs: timeout/reject sym="
                      << sent_syms[i] << " order_id=" << sent_ids[i] << "\n";
        }
    }
    std::cout << "[ETFArb] unwind_dorm_longs: done sent=" << sent << "\n";
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

void ETFArb::run_with_mm(int32_t mm_limit, uint32_t blue_tick) {
    std::cout << "[Bot] Starting combined arb + MM loop\n";

    uint64_t mm_order_id  = 90000;
    //uint64_t blue_bid_id  = 0, blue_ask_id = 0;
    //uint64_t blue_bid_id_  = 0, blue_ask_id_ = 0;
    uint64_t blue_flatten_id = 0;
    int32_t  last_blue_mid = 0;

    auto safe_delete = [&](uint64_t& oid) {
        if (oid == 0) return;
        oe_.delete_order(oid);
        oid = 0;
    };

    while (running_.load(std::memory_order_acquire)) {

        // ── PnL guard ─────────────────────────────────────────────────────
        if (sm_.pnl_near_limit()) {
            std::cerr << "[Bot] PnL near limit — unwinding\n";
            safe_delete(blue_bid_id_);
            safe_delete(blue_ask_id_);
            oe_.cancel_all_open_orders();
            for (uint32_t id = 1; id <= 13; ++id) {
                int32_t pos = sm_.get_position(id);
                if (pos == 0) continue;
                SIDE side     = pos > 0 ? SIDE::SELL : SIDE::BUY;
                int32_t price = pos > 0 ? sm_.best_bid_price(id)
                                        : sm_.best_ask_price(id);
                if (price <= 0) continue;
                uint64_t oid = next_id();
                order_map_[oid] = {id, side};
                oe_.send_new_order(oid, id, side,
                                   static_cast<uint32_t>(std::abs(pos)), price);
            }
            while (true) {
                bool flat = true;
                for (uint32_t id = 1; id <= 13; ++id)
                    if (sm_.get_position(id) != 0) { flat = false; break; }
                if (flat) break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            std::cerr << "[Bot] Flat — resuming\n";
            continue;
        }

        // ── ETF arb ───────────────────────────────────────────────────────
        if (arb_in_progress_.load(std::memory_order_acquire)) {
            bool all_flat = true;
            for (uint32_t id : DORM_IDS)
                if (sm_.get_position(id) > 0) { all_flat = false; break; }
            if (sm_.get_position(SYM_UNDY) != 0) all_flat = false;

            if (all_flat) {
                // Clean up residuals
                for (uint32_t id : DORM_IDS) {
                    int32_t pos = sm_.get_position(id);
                    if (pos == 0) continue;
                    SIDE side     = pos > 0 ? SIDE::SELL : SIDE::BUY;
                    int32_t price = pos > 0 ? sm_.best_bid_price(id)
                                            : sm_.best_ask_price(id);
                    if (price <= 0) continue;
                    uint64_t oid = next_id();
                    order_map_[oid] = {id, side};
                    oe_.send_new_order(oid, id, side,
                                       static_cast<uint32_t>(std::abs(pos)), price);
                }
                arb_in_progress_.store(false, std::memory_order_release);
                std::cout << "[Bot] Arb flat — ready\n";
            } else {
                auto elapsed = std::chrono::steady_clock::now() - arb_start_time_;
                if (elapsed > std::chrono::seconds(3)) {
                    std::cerr << "[Bot] Arb timeout — force unwinding\n";
                    unwind_dorm_longs();
                    int32_t undy_pos = sm_.get_position(SYM_UNDY);
                    if (undy_pos > 0) {
                        int32_t bid = sm_.best_bid_price(SYM_UNDY);
                        if (bid > 0) {
                            uint64_t oid = next_id();
                            order_map_[oid] = {SYM_UNDY, SIDE::SELL};
                            oe_.send_new_order(oid, SYM_UNDY, SIDE::SELL,
                                               static_cast<uint32_t>(undy_pos), bid);
                        }
                    }
                    arb_in_progress_.store(false, std::memory_order_release);
                }
            }
        } else {
            ArbSnapshot snap = sm_.snapshot();
            if (!try_creation_arb(snap))
                try_redemption_arb(snap);
        }

        // ── BLUE market maker ─────────────────────────────────────────────
        int32_t blue_pos = sm_.get_position(SYM_BLUE);

        // Position guards
        if (blue_pos >= mm_limit - 1 && blue_bid_id_ != 0) {
            safe_delete(blue_bid_id_);
            
        }
        if (blue_pos <= -(mm_limit - 1) && blue_ask_id_ != 0) {
            safe_delete(blue_ask_id_);
            
        }


        if (blue_pos < 0 && blue_flatten_id == 0) {
            int32_t ask = sm_.best_ask_price(SYM_BLUE);
            if (ask > 0) {
                blue_flatten_id = ++mm_order_id;
                mm_order_map_[blue_flatten_id] = {SYM_BLUE, SIDE::BUY};
                oe_.send_new_order(blue_flatten_id, SYM_BLUE, SIDE::BUY,
                                   static_cast<uint32_t>(-blue_pos), ask);
                std::cout << "[MM] Flatten BLUE short pos=" << blue_pos
                  << " order_id=" << blue_flatten_id << "\n";
            }
        }
        if (blue_pos >= 0) blue_flatten_id = 0; 

        int32_t blue_bid = sm_.best_bid_price(SYM_BLUE);
        int32_t blue_ask = sm_.best_ask_price(SYM_BLUE);

        if (blue_bid > 0 && blue_ask > 0) {
            int32_t blue_mid = (blue_bid + blue_ask) / 2;
            blue_mid = (blue_mid / (int32_t)blue_tick) * (int32_t)blue_tick;

            if (blue_mid != last_blue_mid && !arb_in_progress_.load()) {
                safe_delete(blue_bid_id_);
                safe_delete(blue_ask_id_);

                blue_pos = sm_.get_position(SYM_BLUE);
                if (blue_pos < mm_limit) {
                    blue_bid_id_ = ++mm_order_id;
                    mm_order_map_[blue_bid_id_] = {SYM_BLUE, SIDE::BUY};
                    oe_.send_new_order(blue_bid_id_, SYM_BLUE,
                                      SIDE::BUY, 1, blue_mid - blue_tick);
                }
                blue_pos = sm_.get_position(SYM_BLUE);
                if (blue_pos > -mm_limit) {
                    blue_ask_id_ = ++mm_order_id;
                    mm_order_map_[blue_ask_id_] = {SYM_BLUE, SIDE::SELL};
                    oe_.send_new_order(blue_ask_id_, SYM_BLUE,
                                      SIDE::SELL, 1, blue_mid + blue_tick);
                }
                last_blue_mid = blue_mid;
            }
        }
    }

    // Cleanup
    safe_delete(blue_bid_id_);
    safe_delete(blue_ask_id_);
    std::cout << "[Bot] Loop stopped\n";
}