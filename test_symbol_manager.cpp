#include "symbol_manager.h"
#include <iostream>
#include <cassert>

// Helper to build a minimal new_order message
new_order make_order(uint64_t oid, uint32_t sym, SIDE side,
                     int32_t price, uint32_t qty, uint32_t seq) {
    new_order msg{};
    msg.header.msg_type = MSG_TYPE::NEW_ORDER;
    msg.header.seq_num  = seq;
    msg.order_id        = oid;
    msg.symbol          = sym;
    msg.side            = side;
    msg.price           = price;
    msg.quantity        = qty;
    return msg;
}

int main() {
    SymbolManager sm;
    int passed = 0, failed = 0;

    auto check = [&](const char* name, bool cond) {
        if (cond) { std::cout << "PASS: " << name << "\n"; ++passed; }
        else      { std::cout << "FAIL: " << name << "\n"; ++failed; }
    };

    // ── Test 1: top-of-book flush after new order ─────────────────────────
    {
        auto msg = make_order(1, SYM_KNAN, SIDE::BUY, 100, 5, 1);
        sm.on_new_order(SYM_KNAN, &msg);
        check("bid price flushed",  sm.best_bid_price(SYM_KNAN) == 100);
        check("bid qty flushed",    sm.best_bid_qty(SYM_KNAN)   == 5);
        check("ask empty",          sm.best_ask_price(SYM_KNAN) == 0);
    }

    // ── Test 2: ask side ──────────────────────────────────────────────────
    {
        auto msg = make_order(2, SYM_KNAN, SIDE::SELL, 105, 3, 2);
        sm.on_new_order(SYM_KNAN, &msg);
        check("ask price flushed",  sm.best_ask_price(SYM_KNAN) == 105);
        check("ask qty flushed",    sm.best_ask_qty(SYM_KNAN)   == 3);
    }

    // ── Test 3: position tracking via on_fill ─────────────────────────────
    {
        sm.on_fill(SYM_KNAN, SIDE::BUY,  3, 105);
        check("position after buy",         sm.get_position(SYM_KNAN) == 3);
        sm.on_fill(SYM_KNAN, SIDE::SELL, 1, 106);
        check("position after sell",        sm.get_position(SYM_KNAN) == 2);
        check("other symbol unaffected",    sm.get_position(SYM_STED) == 0);
    }

    // ── Test 4: PnL tracking ──────────────────────────────────────────────
    {
        // buy 3 @ 105 = -315, sell 1 @ 106 = +106 → net = -209
        check("pnl correct", sm.get_total_pnl() == -209.0);
    }

    // ── Test 5: position limit guard ─────────────────────────────────────
    {
        check("breach detected",
              sm.would_breach_limit(SYM_KNAN, SIDE::BUY, 8));  // 2+8=10 > 9
        check("safe qty allowed",
              !sm.would_breach_limit(SYM_KNAN, SIDE::BUY, 6)); // 2+6=8 <= 9
    }

    // ── Test 6: snapshot NAV ──────────────────────────────────────────────
    {
        SymbolManager sm2;
        // Add asks for all 10 dorms at price 200 each → nav_ask should be 2000
        for (uint32_t i = 0; i < DORM_IDS.size(); ++i) {
            auto msg = make_order(100+i, DORM_IDS[i], SIDE::SELL, 200, 5, 10+i);
            sm2.on_new_order(DORM_IDS[i], &msg);
        }
        ArbSnapshot snap = sm2.snapshot();
        check("nav_ask correct",          snap.nav_ask == 2000);
        check("no missing asks",          !snap.any_dorm_ask_missing);
        check("KNAN position in snapshot", snap.dorms[0].position == 0);
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed.\n";
    return failed > 0 ? 1 : 0;
}