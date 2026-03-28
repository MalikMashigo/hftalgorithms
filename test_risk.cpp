// test_risk.cpp
// Unit tests for PnlTracker and RiskManager, plus a demo run that produces
// risk_demo_log.txt showing the system will not violate any risk limit.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>

#include "iorder_sender.h"
#include "pnl_tracker.h"
#include "risk_manager.h"

// ═══════════════════════════════════════════════════════════════════════════
// Minimal test framework
// ═══════════════════════════════════════════════════════════════════════════

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT_TRUE(expr) do { \
    if (!(expr)) { \
        std::cerr << "  FAIL: " << #expr \
                  << "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        ++g_fail; \
    } else { \
        ++g_pass; \
    } \
} while(0)

#define EXPECT_FALSE(expr)   EXPECT_TRUE(!(expr))
#define EXPECT_EQ(a,b)       EXPECT_TRUE((a) == (b))
#define EXPECT_NEAR(a,b,eps) EXPECT_TRUE(std::fabs((a)-(b)) < (eps))

static void section(const std::string& name) {
    std::cout << "\n── " << name << " ──\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// MockOrderSender
// ═══════════════════════════════════════════════════════════════════════════

class MockOrderSender : public IOrderSender {
public:
    struct Call {
        enum Type { NEW, DEL, MOD };
        Type     type;
        uint64_t order_id;
        uint32_t symbol;
        SIDE     side;
        uint32_t qty;
        int32_t  price;
    };

    bool              next_new_result    = true;
    bool              next_delete_result = true;
    bool              next_modify_result = true;
    std::vector<Call> calls;

    bool send_new_order(uint64_t id, uint32_t sym, SIDE side,
                        uint32_t qty, int32_t px) override {
        calls.push_back({Call::NEW, id, sym, side, qty, px});
        return next_new_result;
    }
    bool delete_order(uint64_t id) override {
        calls.push_back({Call::DEL, id, 0, SIDE::BUY, 0, 0});
        return next_delete_result;
    }
    bool modify_order(uint64_t id, SIDE side, uint32_t qty, int32_t px) override {
        calls.push_back({Call::MOD, id, 0, side, qty, px});
        return next_modify_result;
    }

    void reset() {
        calls.clear();
        next_new_result    = true;
        next_delete_result = true;
        next_modify_result = true;
    }

    size_t count(Call::Type t) const {
        size_t n = 0;
        for (auto& c : calls) if (c.type == t) ++n;
        return n;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Helper: build a RiskManager with explicit, moderate limits.
// Tests not focused on PNL use a very negative min_pnl so that fills at
// any realistic price never accidentally trip the PNL shutdown.
// ═══════════════════════════════════════════════════════════════════════════

static RiskLimits make_limits() {
    RiskLimits L;
    L.max_qty_per_order      = 50;
    L.max_qty_per_side       = 150;
    L.max_exposure           = 200;
    L.max_position           = 100;
    L.max_orders_per_second  = 5;
    L.max_orders_per_seq_num = 3;
    L.max_unacked_orders     = 5;
    L.min_pnl                = -999999.0;  // don't interfere with non-PNL tests
    L.min_valid_price        = 1;
    return L;
}

// ═══════════════════════════════════════════════════════════════════════════
// PnlTracker tests
// ═══════════════════════════════════════════════════════════════════════════

void test_pnl_initial_state() {
    section("PnlTracker: initial state");
    PnlTracker pnl(-1000.0);
    EXPECT_EQ(pnl.get_position(), 0);
    EXPECT_NEAR(pnl.get_total_pnl(), 0.0, 1e-9);
    EXPECT_FALSE(pnl.below_min_pnl());
}

void test_pnl_round_trip() {
    section("PnlTracker: round-trip buy/sell at profit");
    PnlTracker pnl(-1000.0);
    pnl.on_fill(SIDE::BUY,  10, 100);  // buy 10 @ 100 → cost=1000
    pnl.on_fill(SIDE::SELL, 10, 110);  // sell 10 @ 110 → rev=1100

    EXPECT_EQ(pnl.get_position(), 0);
    EXPECT_NEAR(pnl.get_realized_pnl(), 100.0, 1e-9);
    EXPECT_NEAR(pnl.get_total_pnl(),    100.0, 1e-9);
    EXPECT_FALSE(pnl.below_min_pnl());
}

void test_pnl_mark_to_market() {
    section("PnlTracker: mark-to-market with open long position");
    PnlTracker pnl(-1000.0);
    pnl.on_fill(SIDE::BUY, 10, 100);  // long 10, cost=1000
    pnl.update_mark_price(105);

    // realized = 0 − 1000 = −1000
    // unrealized = position * (mark_price - avg_cost) = 10 * (105 - 100) = 50
    EXPECT_NEAR(pnl.get_unrealized_pnl(), 50.0, 1e-9);
    EXPECT_NEAR(pnl.get_total_pnl(),       50.0, 1e-9);
    EXPECT_FALSE(pnl.below_min_pnl());
}

void test_pnl_mark_to_market_short() {
    section("PnlTracker: mark-to-market with open short position");
    PnlTracker pnl(-1000.0);
    pnl.on_fill(SIDE::SELL, 10, 100); // short 10, rev=1000
    pnl.update_mark_price(95);

    // realized=1000; unrealized=−10×95=−950; total=50
    EXPECT_NEAR(pnl.get_total_pnl(), 50.0, 1e-9);
}

void test_pnl_mixed_fills_with_mark() {
    section("PnlTracker: partial fills + mark");
    PnlTracker pnl(-1000.0);
    pnl.on_fill(SIDE::BUY,  10, 100);  // long 10, cost=1000
    pnl.on_fill(SIDE::SELL,  5, 110);  // sell 5 @ 110, rev=550, pos=+5
    pnl.update_mark_price(105);

    // total = 550 − 1000 + 5×105 = 75
    EXPECT_EQ(pnl.get_position(), 5);
    EXPECT_NEAR(pnl.get_total_pnl(), 75.0, 1e-9);
}

void test_pnl_below_minimum() {
    section("PnlTracker: below_min_pnl trigger");
    PnlTracker pnl(-100.0);
    pnl.on_fill(SIDE::BUY, 10, 200);  // cost=2000
    pnl.update_mark_price(180);        // unrealized = 10×180 − 2000 = −200

    EXPECT_TRUE(pnl.below_min_pnl());  // −200 < −100
}

void test_pnl_not_below_minimum() {
    section("PnlTracker: not below_min_pnl");
    PnlTracker pnl(-1000.0);
    pnl.on_fill(SIDE::BUY, 10, 100);
    pnl.update_mark_price(105);  // pnl = +50
    EXPECT_FALSE(pnl.below_min_pnl());
}

// ═══════════════════════════════════════════════════════════════════════════
// RiskManager tests
// ═══════════════════════════════════════════════════════════════════════════

void test_risk_valid_order() {
    section("Risk: valid order passes");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    bool ok = rm.send_new_order(1, 1, SIDE::BUY, 10, 100);
    EXPECT_TRUE(ok);
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)1);
}

// ── (a) max qty per order ────────────────────────────────────────────────────

void test_risk_max_qty_per_order() {
    section("Risk (a): max_qty_per_order");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null"); // limit = 50

    RiskResult rc = rm.check_new_order(1, SIDE::BUY, 51, 100);
    EXPECT_EQ(rc, RiskResult::QTY_PER_ORDER_EXCEEDED);

    bool ok = rm.send_new_order(1, 1, SIDE::BUY, 51, 100);
    EXPECT_FALSE(ok);
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)0);
}

// ── (b) max qty per side ─────────────────────────────────────────────────────

void test_risk_max_qty_per_side() {
    section("Risk (b): max_qty_per_side");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null"); // max_qty_per_side=150

    // Three orders of 50 → total 150 outstanding (at limit)
    EXPECT_TRUE(rm.send_new_order(1, 1, SIDE::BUY, 50, 100));
    EXPECT_TRUE(rm.send_new_order(2, 1, SIDE::BUY, 50, 100));
    EXPECT_TRUE(rm.send_new_order(3, 1, SIDE::BUY, 50, 100));

    // Any further BUY must be blocked
    RiskResult rc = rm.check_new_order(4, SIDE::BUY, 1, 100);
    EXPECT_EQ(rc, RiskResult::QTY_PER_SIDE_EXCEEDED);

    bool ok = rm.send_new_order(4, 1, SIDE::BUY, 1, 100);
    EXPECT_FALSE(ok);
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)3);
}

// ── (c) max exposure ─────────────────────────────────────────────────────────

void test_risk_max_exposure() {
    section("Risk (c): max_exposure");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_qty_per_side = 9999;   // disable side limit so only exposure fires
    L.max_exposure     = 100;
    RiskManager rm(mock, L, "/dev/null");

    // Set mark = 100 first so fills don't trigger a PNL drop
    rm.update_mark_price(100);

    // Buy 10 and fill it: position=+10, pnl ≈ 0
    rm.send_new_order(1, 1, SIDE::BUY, 10, 100);
    rm.on_fill(1, 10, 100, true);  // position=10

    // Outstanding buys: 90 (orders 2+3).  buy_exposure = 10 + 90 = 100 = limit
    rm.send_new_order(2, 1, SIDE::BUY, 50, 100);  // exposure = 10+50=60
    rm.send_new_order(3, 1, SIDE::BUY, 40, 100);  // exposure = 10+90=100

    // One more buy exceeds exposure
    RiskResult rc = rm.check_new_order(4, SIDE::BUY, 1, 100);
    EXPECT_EQ(rc, RiskResult::EXPOSURE_EXCEEDED);

    bool ok = rm.send_new_order(4, 1, SIDE::BUY, 1, 100);
    EXPECT_FALSE(ok);
}

// ── (d) invalid price ────────────────────────────────────────────────────────

void test_risk_invalid_price() {
    section("Risk (d): invalid price");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    EXPECT_EQ(rm.check_new_order(1, SIDE::BUY, 10,  0), RiskResult::INVALID_PRICE);
    EXPECT_EQ(rm.check_new_order(1, SIDE::BUY, 10, -5), RiskResult::INVALID_PRICE);

    bool ok = rm.send_new_order(1, 1, SIDE::BUY, 10, 0);
    EXPECT_FALSE(ok);
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)0);
}

// ── (e) position limit – block when at limit and order pushes further out ────

void test_risk_position_at_limit_blocked() {
    section("Risk (e): order blocked when at position limit and would increase");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_position = 20;
    RiskManager rm(mock, L, "/dev/null");

    // Fill to exactly +20 (set mark first to keep PNL near 0)
    rm.update_mark_price(100);
    rm.send_new_order(1, 1, SIDE::BUY, 20, 100);
    rm.on_fill(1, 20, 100, true);   // position = +20, pnl = 0

    // BUY would increase |pos| → blocked
    RiskResult rc = rm.check_new_order(2, SIDE::BUY, 1, 100);
    EXPECT_EQ(rc, RiskResult::POSITION_LIMIT_WOULD_INCREASE);
    EXPECT_FALSE(rm.send_new_order(2, 1, SIDE::BUY, 1, 100));

    // SELL reduces |pos| → allowed
    rc = rm.check_new_order(3, SIDE::SELL, 1, 100);
    EXPECT_EQ(rc, RiskResult::OK);
    EXPECT_TRUE(rm.send_new_order(3, 1, SIDE::SELL, 1, 100));
}

void test_risk_position_would_exceed_limit() {
    section("Risk (e): order blocked when qty would exceed position limit");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_position = 20;
    RiskManager rm(mock, L, "/dev/null");

    // From flat, a single order of 21 would exceed limit
    RiskResult rc = rm.check_new_order(1, SIDE::BUY, 21, 100);
    EXPECT_EQ(rc, RiskResult::POSITION_LIMIT_EXCEEDED);
    EXPECT_FALSE(rm.send_new_order(1, 1, SIDE::BUY, 21, 100));
}

// ── (f) max orders per second ────────────────────────────────────────────────

void test_risk_orders_per_second() {
    section("Risk (f): max_orders_per_second");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_orders_per_second  = 3;
    L.max_orders_per_seq_num = 100; // disable seq-num limit
    RiskManager rm(mock, L, "/dev/null");

    EXPECT_TRUE(rm.send_new_order(1, 1, SIDE::BUY, 1, 100));
    EXPECT_TRUE(rm.send_new_order(2, 1, SIDE::BUY, 1, 100));
    EXPECT_TRUE(rm.send_new_order(3, 1, SIDE::BUY, 1, 100));
    EXPECT_FALSE(rm.send_new_order(4, 1, SIDE::BUY, 1, 100)); // 4th in same second
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)3);
}

// ── (g) max orders per seq num ───────────────────────────────────────────────

void test_risk_orders_per_seq_num() {
    section("Risk (g): max_orders_per_seq_num");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_orders_per_second  = 100; // disable per-second limit
    L.max_orders_per_seq_num = 2;
    RiskManager rm(mock, L, "/dev/null");

    rm.on_seq_num_update(1);
    EXPECT_TRUE(rm.send_new_order(1, 1, SIDE::BUY, 1, 100));
    EXPECT_TRUE(rm.send_new_order(2, 1, SIDE::BUY, 1, 100));
    EXPECT_FALSE(rm.send_new_order(3, 1, SIDE::BUY, 1, 100)); // 3rd blocked

    rm.on_seq_num_update(2);  // new seq num resets counter
    EXPECT_TRUE(rm.send_new_order(4, 1, SIDE::BUY, 1, 100));
}

// ── (h) max unacked orders ───────────────────────────────────────────────────

void test_risk_max_unacked_orders() {
    section("Risk (h): max_unacked_orders = 0 blocks all orders");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_unacked_orders = 0;
    RiskManager rm(mock, L, "/dev/null");

    // unacked(0) >= limit(0) → every order blocked before touching exchange
    EXPECT_FALSE(rm.send_new_order(1, 1, SIDE::BUY, 1, 100));
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)0);
}

// ── (i) duplicate order ID ───────────────────────────────────────────────────

void test_risk_duplicate_order_id() {
    section("Risk (i): duplicate order_id blocked");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    EXPECT_TRUE(rm.send_new_order(42, 1, SIDE::BUY, 10, 100));
    EXPECT_EQ(rm.check_new_order(42, SIDE::BUY, 10, 100), RiskResult::DUPLICATE_ORDER_ID);
    EXPECT_FALSE(rm.send_new_order(42, 1, SIDE::BUY, 10, 100));
}

// ── modify checks ────────────────────────────────────────────────────────────

void test_risk_modify_invalid_price() {
    section("Risk: modify with invalid price blocked");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    rm.send_new_order(1, 1, SIDE::BUY, 10, 100);
    EXPECT_EQ(rm.check_modify_order(1, SIDE::BUY, 10, 0), RiskResult::INVALID_PRICE);
}

void test_risk_modify_unknown_order() {
    section("Risk: modify on unknown order blocked");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    EXPECT_EQ(rm.check_modify_order(99, SIDE::BUY, 10, 100), RiskResult::UNKNOWN_ORDER_ID);
}

void test_risk_modify_qty_increase_capped() {
    section("Risk: modify qty increase that would exceed side limit blocked");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_qty_per_order = 100;  // raise per-order limit so only side limit fires
    L.max_qty_per_side  = 50;
    RiskManager rm(mock, L, "/dev/null");

    rm.send_new_order(1, 1, SIDE::BUY, 50, 100); // outstanding=50 = side limit
    // delta = 51 - 50 = 1; outstanding(50) + delta(1) = 51 > 50 → side limit
    EXPECT_EQ(rm.check_modify_order(1, SIDE::BUY, 51, 100), RiskResult::QTY_PER_SIDE_EXCEEDED);
}

// ── cancel all open orders ───────────────────────────────────────────────────

void test_risk_cancel_all() {
    section("Risk: cancel_all_open_orders cancels every open order");
    MockOrderSender mock;
    RiskManager rm(mock, make_limits(), "/dev/null");

    rm.send_new_order(10, 1, SIDE::BUY,  10, 100);
    rm.send_new_order(11, 1, SIDE::SELL, 10, 101);
    rm.send_new_order(12, 1, SIDE::BUY,   5,  99);
    EXPECT_EQ(rm.open_order_ids().size(), (size_t)3);

    rm.cancel_all_open_orders();
    EXPECT_EQ(mock.count(MockOrderSender::Call::DEL), (size_t)3);
    EXPECT_EQ(rm.open_order_ids().size(), (size_t)0);
}

// ── PNL shutdown ─────────────────────────────────────────────────────────────

void test_risk_pnl_shutdown() {
    section("Risk: system shuts down when PNL < min_pnl");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.min_pnl = -100.0;
    RiskManager rm(mock, L, "/dev/null");

    // Buy at 200 with mark at 200 → PNL = 0 on fill
    rm.update_mark_price(200);
    rm.send_new_order(1, 1, SIDE::BUY, 10, 200);
    rm.on_fill(1, 10, 200, true);   // position=+10, buy_cost=2000, pnl=0
    EXPECT_FALSE(rm.is_shutdown());

    // Mark falls sharply: pnl = 10×180 − 2000 = −200 < −100
    rm.update_mark_price(180);
    rm.check_and_shutdown_if_needed();
    EXPECT_TRUE(rm.is_shutdown());

    // Subsequent orders must be blocked
    EXPECT_FALSE(rm.send_new_order(2, 1, SIDE::BUY, 1, 100));
    EXPECT_EQ(mock.count(MockOrderSender::Call::NEW), (size_t)1);
}

// ── Position shutdown ────────────────────────────────────────────────────────
//
// Scenario: two orders pass the risk check individually (each buy of 9 is
// within the 10-unit position limit from flat), but when both fill the
// cumulative position (18) exceeds max_position (10) → shutdown.

void test_risk_position_shutdown() {
    section("Risk: system shuts down when |position| > max_position");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.max_position = 10;
    RiskManager rm(mock, L, "/dev/null");

    rm.update_mark_price(100);   // keep PNL near 0

    // Both pass individual position checks while position is still 0
    rm.send_new_order(1, 1, SIDE::BUY, 9, 100);
    rm.send_new_order(2, 1, SIDE::BUY, 9, 100);

    // First fill: position = 9 (≤ 10, OK)
    rm.on_fill(1, 9, 100, true);
    EXPECT_FALSE(rm.is_shutdown());

    // Second fill: position = 18 (> 10 → shutdown)
    rm.on_fill(2, 9, 100, true);
    EXPECT_TRUE(rm.is_shutdown());
}

// ── Open orders cancelled on shutdown ────────────────────────────────────────

void test_risk_shutdown_cancels_open_orders() {
    section("Risk: open orders cancelled when shutdown is triggered");
    MockOrderSender mock;
    RiskLimits L = make_limits();
    L.min_pnl = -1.0;
    RiskManager rm(mock, L, "/dev/null");

    rm.send_new_order(1, 1, SIDE::BUY,  10, 100);
    rm.send_new_order(2, 1, SIDE::SELL, 10, 101);
    rm.send_new_order(3, 1, SIDE::BUY,   5,  99);
    EXPECT_EQ(rm.open_order_ids().size(), (size_t)3);

    // Fill order 1 at mark=100 so PNL is 0 on fill, then crash the mark
    rm.update_mark_price(100);
    rm.on_fill(1, 10, 100, true);   // position=+10, cost=1000, pnl=0
    EXPECT_FALSE(rm.is_shutdown());

    // Mark collapses: pnl = 10×50 − 1000 = −500 < −1 → shutdown + cancel-all
    rm.update_mark_price(50);
    rm.check_and_shutdown_if_needed();

    EXPECT_TRUE(rm.is_shutdown());
    EXPECT_EQ(rm.open_order_ids().size(), (size_t)0);
    // Orders 2 and 3 must have been cancelled (order 1 was already closed)
    EXPECT_EQ(mock.count(MockOrderSender::Call::DEL), (size_t)2);
}

// ═══════════════════════════════════════════════════════════════════════════
// DEMO – scripted scenario producing risk_demo_log.txt
// ═══════════════════════════════════════════════════════════════════════════

void run_demo() {
    std::cout << "\n\n════════════════════════════════════════════\n"
              << "  DEMO: Risk system vs scripted scenario\n"
              << "  (see risk_demo_log.txt for full audit trail)\n"
              << "════════════════════════════════════════════\n\n";

    MockOrderSender mock;
    RiskLimits L;
    // max_qty_per_side > max_exposure so that exposure fires in (c) but
    // for (b) we use a dedicated sub-manager with side < exposure.
    L.max_qty_per_order      = 20;
    L.max_qty_per_side       = 200;  // kept high for main manager
    L.max_exposure           = 50;   // exposure fires in (c)
    L.max_position           = 30;
    L.max_orders_per_second  = 100;
    L.max_orders_per_seq_num = 100;
    L.max_unacked_orders     = 10;
    L.min_pnl                = -200.0;
    L.min_valid_price        = 1;

    RiskManager rm(mock, L, "risk_demo_log.txt");
    rm.update_mark_price(100);  // establish baseline mark price

    auto try_send = [&](const std::string& label,
                        uint64_t id, uint32_t sym, SIDE side,
                        uint32_t qty, int32_t px) {
        std::cout << label << ": ";
        bool ok = rm.send_new_order(id, sym, side, qty, px);
        std::cout << (ok ? "SENT" : "BLOCKED") << "\n";
        return ok;
    };

    // ── (d) invalid price ────────────────────────────────────────────────────
    std::cout << "\n--- (d) Invalid price ---\n";
    try_send("px=0  (must block)", 1, 1, SIDE::BUY, 5, 0);
    try_send("px=-1 (must block)", 2, 1, SIDE::BUY, 5, -1);
    try_send("px=100 (ok)",        3, 1, SIDE::BUY, 5, 100);
    rm.cancel_all_open_orders();  // reset

    // ── (a) max qty per order ────────────────────────────────────────────────
    std::cout << "\n--- (a) Max qty per order ---\n";
    try_send("qty=21 (must block, limit=20)", 10, 1, SIDE::BUY, 21, 100);
    try_send("qty=20 (ok)",                  11, 1, SIDE::BUY, 20, 100);
    rm.cancel_all_open_orders();

    // ── (b) max qty per side ─────────────────────────────────────────────────
    // Use a dedicated manager with tight side limit (40) > exposure (200)
    // so the side limit is the first to fire.
    std::cout << "\n--- (b) Max qty per side (limit=40, exposure=200) ---\n";
    {
        MockOrderSender mock_b;
        RiskLimits Lb = L;
        Lb.max_qty_per_side = 40;
        Lb.max_exposure     = 200;  // high, so only side limit fires
        RiskManager rmb(mock_b, Lb, "/dev/null");

        rmb.update_mark_price(100);
        std::cout << "BUY 20 [id=20]: " << (rmb.send_new_order(20,1,SIDE::BUY,20,100)?"SENT":"BLOCKED") << "\n";
        std::cout << "BUY 20 [id=21]: " << (rmb.send_new_order(21,1,SIDE::BUY,20,100)?"SENT":"BLOCKED") << "\n";
        std::cout << "BUY 1  [id=22] (must block – qty/side=40): "
                  << (rmb.send_new_order(22,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
    }

    // ── (c) max exposure ─────────────────────────────────────────────────────
    std::cout << "\n--- (c) Max exposure (limit=50) ---\n";
    // Get some position first so exposure = position + outstanding
    try_send("BUY 10 [id=30]", 30, 1, SIDE::BUY, 10, 100);
    rm.on_fill(30, 10, 100, true);  // pos=10, pnl=10*100−1000=0

    // exposure = 10 (pos) + outstanding.  Send 40 more → exposure=50=limit
    try_send("BUY 20 [id=31]", 31, 1, SIDE::BUY, 20, 100);  // exposure=30
    try_send("BUY 20 [id=32]", 32, 1, SIDE::BUY, 20, 100);  // exposure=50
    try_send("BUY 1  [id=33] (must block – exposure)", 33, 1, SIDE::BUY, 1, 100);
    rm.cancel_all_open_orders();

    // ── (e) position limit ───────────────────────────────────────────────────
    std::cout << "\n--- (e) Position limit (limit=30) ---\n";
    // Already at pos=10.  Send 20 to reach +30 (at limit)
    try_send("BUY 20 [id=40]", 40, 1, SIDE::BUY, 20, 100);
    rm.on_fill(40, 20, 100, true);  // pos=30, pnl=30*100−3000=0

    // Any further BUY blocked (at_limit && would_increase)
    try_send("BUY 1 [id=41] (must block – at position limit)", 41, 1, SIDE::BUY, 1, 100);
    // A SELL is still allowed (reduces position)
    try_send("SELL 5 [id=42] (ok – reduces position)", 42, 1, SIDE::SELL, 5, 100);
    rm.on_fill(42, 5, 100, true);  // pos=25
    rm.cancel_all_open_orders();

    // ── (f) orders per second ────────────────────────────────────────────────
    std::cout << "\n--- (f) Orders per second (limit=3) ---\n";
    {
        MockOrderSender mock2;
        RiskLimits L2 = L;
        L2.max_orders_per_second  = 3;
        L2.max_orders_per_seq_num = 100;
        RiskManager rm2(mock2, L2, "/dev/null");

        std::cout << "order 1: "; std::cout << (rm2.send_new_order(1,1,SIDE::BUY,1,100) ? "SENT":"BLOCKED") << "\n";
        std::cout << "order 2: "; std::cout << (rm2.send_new_order(2,1,SIDE::BUY,1,100) ? "SENT":"BLOCKED") << "\n";
        std::cout << "order 3: "; std::cout << (rm2.send_new_order(3,1,SIDE::BUY,1,100) ? "SENT":"BLOCKED") << "\n";
        std::cout << "order 4 (must block): "; std::cout << (rm2.send_new_order(4,1,SIDE::BUY,1,100) ? "SENT":"BLOCKED") << "\n";
    }

    // ── (g) orders per seq num ───────────────────────────────────────────────
    std::cout << "\n--- (g) Orders per seq-num update (limit=2) ---\n";
    {
        MockOrderSender mock3;
        RiskLimits L3 = L;
        L3.max_orders_per_second  = 100;
        L3.max_orders_per_seq_num = 2;
        RiskManager rm3(mock3, L3, "/dev/null");
        rm3.on_seq_num_update(1);

        std::cout << "order 1 (seq=1): "; std::cout << (rm3.send_new_order(1,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
        std::cout << "order 2 (seq=1): "; std::cout << (rm3.send_new_order(2,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
        std::cout << "order 3 (seq=1, must block): "; std::cout << (rm3.send_new_order(3,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
        rm3.on_seq_num_update(2);
        std::cout << "order 4 (seq=2, ok): "; std::cout << (rm3.send_new_order(4,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
    }

    // ── (h) max unacked ──────────────────────────────────────────────────────
    std::cout << "\n--- (h) Max unacked orders (limit=0 → all blocked) ---\n";
    {
        MockOrderSender mock4;
        RiskLimits L4 = L;
        L4.max_unacked_orders = 0;
        RiskManager rm4(mock4, L4, "/dev/null");
        std::cout << "order with unacked_limit=0 (must block): ";
        std::cout << (rm4.send_new_order(1,1,SIDE::BUY,1,100)?"SENT":"BLOCKED") << "\n";
    }

    // ── Cancel-all demo ──────────────────────────────────────────────────────
    std::cout << "\n--- Cancel all open orders ---\n";
    // rm still has pos=25, no open orders (cancelled above).  Add some new ones.
    try_send("BUY  5 [id=50]", 50, 1, SIDE::BUY,  5, 100);
    try_send("SELL 3 [id=51]", 51, 1, SIDE::SELL, 3, 100);
    std::cout << "Open orders before cancel: " << rm.open_order_ids().size() << "\n";
    rm.cancel_all_open_orders();
    std::cout << "Open orders after cancel: " << rm.open_order_ids().size() << "\n";

    // ── PNL shutdown demo ────────────────────────────────────────────────────
    // At this point: pos=25, mark=100, pnl=0 (sells covered the buy cost MTM).
    // Crash the mark so pnl = 25*80 − cost < -200 → shutdown.
    std::cout << "\n--- PNL-driven shutdown (min_pnl=-200) ---\n";
    std::cout << "PNL before mark drop: " << rm.get_total_pnl() << "  (mark=100)\n";
    rm.update_mark_price(80);  // pnl = 25*80 - 3000 + sell_rev ≈ 2000 - 3000 + 500 = -500 < -200
    std::cout << "PNL after mark drop to 80: " << rm.get_total_pnl() << "\n";
    rm.check_and_shutdown_if_needed();
    std::cout << "Is shutdown: " << (rm.is_shutdown() ? "YES" : "NO") << "\n";
    try_send("BUY 1 [id=99] (must block – shutdown)", 99, 1, SIDE::BUY, 1, 100);

    std::cout << "\nFinal state:"
              << "\n  position    = " << rm.get_position()
              << "\n  total_pnl   = " << rm.get_total_pnl()
              << "\n  open_orders = " << rm.open_order_ids().size()
              << "\n  is_shutdown = " << (rm.is_shutdown() ? "YES" : "NO")
              << "\n\nSee risk_demo_log.txt for the full audit trail.\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║         HFT Risk System Tests            ║\n"
              << "╚══════════════════════════════════════════╝\n";

    // ── PnlTracker ──────────────────────────────────────────────────────────
    test_pnl_initial_state();
    test_pnl_round_trip();
    test_pnl_mark_to_market();
    test_pnl_mark_to_market_short();
    test_pnl_mixed_fills_with_mark();
    test_pnl_below_minimum();
    test_pnl_not_below_minimum();

    // ── RiskManager ─────────────────────────────────────────────────────────
    test_risk_valid_order();
    test_risk_max_qty_per_order();
    test_risk_max_qty_per_side();
    test_risk_max_exposure();
    test_risk_invalid_price();
    test_risk_position_at_limit_blocked();
    test_risk_position_would_exceed_limit();
    test_risk_orders_per_second();
    test_risk_orders_per_seq_num();
    test_risk_max_unacked_orders();
    test_risk_duplicate_order_id();
    test_risk_modify_invalid_price();
    test_risk_modify_unknown_order();
    test_risk_modify_qty_increase_capped();
    test_risk_cancel_all();
    test_risk_pnl_shutdown();
    test_risk_position_shutdown();
    test_risk_shutdown_cancels_open_orders();

    // ── Summary ──────────────────────────────────────────────────────────────
    std::cout << "\n\n══════════════════════════════════════════\n"
              << "  Results: " << g_pass << " passed, " << g_fail << " failed\n"
              << "══════════════════════════════════════════\n";

    run_demo();

    return g_fail == 0 ? 0 : 1;
}
