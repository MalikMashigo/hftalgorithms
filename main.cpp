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
#include "market_maker.h"
#include "listener.h"

static constexpr const char* EXCHANGE_HOST = "192.168.13.100";
static constexpr int         EXCHANGE_PORT = 1234;
static constexpr const char* ETF_URL       = "http://129.74.160.245:5000";
static constexpr const char* TEAM_NAME     = "group8";
static constexpr const char* PASSWORD      = "Uangjrty";
static constexpr uint32_t    CLIENT_ID     = 8;

static constexpr uint32_t GOLD_TICK = 10;
static constexpr uint32_t BLUE_TICK = 5;
static constexpr int32_t  MM_POSITION_LIMIT = 8;

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

    // ── MarketMaker (separate class for GOLD and BLUE quotes) ───────────────
    MarketMaker mm(oe, sm, SYM_GOLD, SYM_BLUE);

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
            std::this_thread::sleep_for(std::chrono::seconds(1));
            double pnl = sm.get_total_pnl();
            std::cout << "[PnL] total=" << pnl << " | positions: ";
            for (uint32_t id = 1; id <= 13; ++id) {
                int32_t pos = sm.get_position(id);
                if (pos != 0) std::cout << "sym" << id << "=" << pos << " ";
            }
            std::cout << "\n";
            std::cout.flush();
            if (pnl < -4000.0)
                std::cerr << "[PnL] WARNING: approaching -5000 floor!\n";
        }
    });

    // ── Market maker thread (GOLD + BLUE using MarketMaker class) ────────────
    std::thread mm_thread([&]() {
        std::cout << "[MM] Market maker thread started\n";
        while (!global_shutdown.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            mm.on_book_update(SYM_GOLD);
            mm.on_book_update(SYM_BLUE);
        }
        std::cout << "[MM] Market maker shut down\n";
    });

    // ── Arb thread ────────────────────────────────────────────────────────────
    std::thread arb_thread([&]() {
        arb.run();
    });

    md_thread.join();
    arb_thread.join();
    mm_thread.join();
    pnl_thread.join();
    return 0;
}