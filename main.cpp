#include <thread>
#include <iostream>
#include <atomic>
#include <unordered_map>
#include <utility>
#include <chrono>

#include "symbol_manager.h"
#include "oe_client.h"
#include "etf_client.h"
#include "etf_arb.h"
#include "listener.h"

static constexpr const char* EXCHANGE_HOST = "192.168.13.100";
static constexpr int         EXCHANGE_PORT = 1234;
static constexpr const char* ETF_URL       = "http://129.74.160.245:5000";
static constexpr const char* TEAM_NAME     = "group8";
static constexpr const char* PASSWORD      = "Uangjrty";
static constexpr uint32_t    CLIENT_ID     = 8;

static constexpr uint32_t GOLD_TICK = 10;
static constexpr uint32_t BLUE_TICK = 5;
static constexpr int32_t  MM_POSITION_LIMIT = 6;

int main() {
    // ── Shared state ──────────────────────────────────────────────────────────
    SymbolManager       sm;
    std::atomic<bool>   global_shutdown{false};

    // order_id → {symbol, side} for fill routing from market maker
    std::unordered_map<uint64_t, std::pair<uint32_t, SIDE>> mm_order_map;

    // ── Order entry client ────────────────────────────────────────────────────
    OEClient oe(EXCHANGE_HOST, EXCHANGE_PORT);
    if (!oe.connect()) {
        std::cerr << "Failed to connect to exchange\n";
        return 1;
    }
    if (!oe.login(TEAM_NAME, PASSWORD, CLIENT_ID)) {
        std::cerr << "Login failed\n";
        return 1;
    }

    // Fill callback for market maker orders
    // (ETFArb registers its own fill callback in its constructor,
    //  overwriting this one — so ETFArb must be constructed after this)
    // oe.set_on_fill([&](const FillEvent& f) {
    //     std::cout << "[FILL] order=" << f.order_id
    //           << " qty=" << f.qty
    //           << " price=" << f.price
    //           << " closed=" << f.closed << "\n";
    //     auto it = mm_order_map.find(f.order_id);
    //     if (it != mm_order_map.end()) {
    //         sm.on_fill(it->second.first, it->second.second, f.qty, f.price);
    //         if (f.closed) mm_order_map.erase(it);
    //     }
    // });

    // ── ETF client ────────────────────────────────────────────────────────────
    ETFClient etf(ETF_URL, TEAM_NAME, PASSWORD);
    if (!etf.health_check()) {
        std::cerr << "ETF service unreachable\n";
        return 1;
    }

    // ── ETFArb (registers its own fill callback on construction) ─────────────
    ETFArb arb(sm, oe, etf, global_shutdown, mm_order_map);
    oe.set_on_close([&](uint64_t order_id) {
    mm_order_map.erase(order_id);
    });

    // ── Market data thread ────────────────────────────────────────────────────
    std::thread md_thread([&]() {
        run_listener(sm);
    });

    // Wait for books to populate
    std::cout << "Waiting 3s for market data snapshot...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ── PnL monitor thread ────────────────────────────────────────────────────
    std::thread pnl_thread([&]() {
        while (!global_shutdown.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            double pnl = sm.get_total_pnl();
            std::cout << "[PnL] total=" << pnl << " | positions: ";
            for (uint32_t id = 1; id <= 13; ++id) {
                int32_t pos = sm.get_position(id);
                if (pos != 0) std::cout << "sym" << id << "=" << pos << " ";
            }
            std::cout << "\n";
            if (pnl < -4000.0)
                std::cerr << "[PnL] WARNING: approaching -5000 floor!\n";
        }
    });
    pnl_thread.detach();

    // ── Market maker thread (GOLD + BLUE) ─────────────────────────────────────
    std::thread mm_thread([&]() {
        std::cout << "[MM] Thread started\n"; 
        uint64_t mm_order_id = 90000;
        uint64_t gold_bid_id = 0, gold_ask_id = 0;
        uint64_t blue_bid_id = 0, blue_ask_id = 0;
        int32_t  last_gold_mid = 0, last_blue_mid = 0;

        auto safe_delete = [&](uint64_t& oid) {
            if (oid == 0) return;
            oe.delete_order(oid);
            oid = 0;
        };

        while (!global_shutdown.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Position guards — cancel resting orders if too long/short ─────────
    int32_t gold_pos = sm.get_position(SYM_GOLD);
    if (gold_pos >= MM_POSITION_LIMIT - 1 && gold_bid_id != 0) safe_delete(gold_bid_id);
    if (gold_pos <= -(MM_POSITION_LIMIT - 1) && gold_ask_id != 0) safe_delete(gold_ask_id);

    int32_t blue_pos = sm.get_position(SYM_BLUE);
    if (blue_pos >= MM_POSITION_LIMIT - 1 && blue_bid_id != 0) safe_delete(blue_bid_id);
    if (blue_pos <= -(MM_POSITION_LIMIT - 1) && blue_ask_id != 0) safe_delete(blue_ask_id);

    // ── GOLD ──────────────────────────────────────────────────────────────
    int32_t gold_bid = sm.best_bid_price(SYM_GOLD);
    int32_t gold_ask = sm.best_ask_price(SYM_GOLD);

    if (gold_bid > 0 && gold_ask > 0) {
        int32_t gold_mid = (gold_bid + gold_ask) / 2;
        gold_mid = (gold_mid / (int32_t)GOLD_TICK) * (int32_t)GOLD_TICK;

        if (gold_mid != last_gold_mid) {
            safe_delete(gold_bid_id);
            safe_delete(gold_ask_id);

            gold_pos = sm.get_position(SYM_GOLD);  // fresh read after deletes
            if (gold_pos < MM_POSITION_LIMIT) {
                gold_bid_id = ++mm_order_id;
                mm_order_map[gold_bid_id] = {SYM_GOLD, SIDE::BUY};
                oe.send_new_order(gold_bid_id, SYM_GOLD,
                                  SIDE::BUY, 1, gold_mid - GOLD_TICK);
            }

            gold_pos = sm.get_position(SYM_GOLD);  // fresh read before ask
            if (gold_pos > -MM_POSITION_LIMIT) {
                gold_ask_id = ++mm_order_id;
                mm_order_map[gold_ask_id] = {SYM_GOLD, SIDE::SELL};
                oe.send_new_order(gold_ask_id, SYM_GOLD,
                                  SIDE::SELL, 1, gold_mid + GOLD_TICK);
            }

            last_gold_mid = gold_mid;
        }
    }

    // ── BLUE ──────────────────────────────────────────────────────────────
    int32_t blue_bid = sm.best_bid_price(SYM_BLUE);
    int32_t blue_ask = sm.best_ask_price(SYM_BLUE);

    if (blue_bid > 0 && blue_ask > 0) {
        int32_t blue_mid = (blue_bid + blue_ask) / 2;
        blue_mid = (blue_mid / (int32_t)BLUE_TICK) * (int32_t)BLUE_TICK;

        if (blue_mid != last_blue_mid) {
            safe_delete(blue_bid_id);
            safe_delete(blue_ask_id);

            blue_pos = sm.get_position(SYM_BLUE);  // fresh read after deletes
            if (blue_pos < MM_POSITION_LIMIT) {
                blue_bid_id = ++mm_order_id;
                mm_order_map[blue_bid_id] = {SYM_BLUE, SIDE::BUY};
                oe.send_new_order(blue_bid_id, SYM_BLUE,
                                  SIDE::BUY, 1, blue_mid - BLUE_TICK);
            }

            blue_pos = sm.get_position(SYM_BLUE);  // fresh read before ask
            if (blue_pos > -MM_POSITION_LIMIT) {
                blue_ask_id = ++mm_order_id;
                mm_order_map[blue_ask_id] = {SYM_BLUE, SIDE::SELL};
                oe.send_new_order(blue_ask_id, SYM_BLUE,
                                  SIDE::SELL, 1, blue_mid + BLUE_TICK);
            }

            last_blue_mid = blue_mid;
        }
    }
}

        // Cleanup on shutdown
        if (gold_bid_id) safe_delete(gold_bid_id);
        if (gold_ask_id) safe_delete(gold_ask_id);
        if (blue_bid_id) safe_delete(blue_bid_id);
        if (blue_ask_id) safe_delete(blue_ask_id);
        std::cout << "[MM] Market maker shut down\n";
    });

    // ── Arb thread ────────────────────────────────────────────────────────────
    std::thread arb_thread([&]() {
        arb.run();
    });

    md_thread.join();
    arb_thread.join();
    mm_thread.join();
    return 0;
}