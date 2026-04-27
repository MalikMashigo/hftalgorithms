#include <thread>
#include <iostream>

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

int main() {
    // ── Shared state ─────────────────────────────────────────────────────────
    SymbolManager sm;

    // ── Order entry client ───────────────────────────────────────────────────
    OEClient oe(EXCHANGE_HOST, EXCHANGE_PORT);
    if (!oe.connect()) {
        std::cerr << "Failed to connect to exchange\n";
        return 1;
    }
    if (!oe.login(TEAM_NAME, PASSWORD, CLIENT_ID)) {
        std::cerr << "Login failed\n";
        return 1;
    }

    // Hook fill callback → SymbolManager so positions stay up to date
    // oe.set_on_fill([&](const FillEvent& f) {
    //     // We need to know the symbol for each fill.
    //     // OEClient doesn't currently carry symbol in FillEvent — see note below.
    //     // For now, route all fills through a local order→symbol map.
    //     // TODO: add symbol tracking (see note below)
    //     std::cout << "[FILL] order=" << f.order_id
    //               << " qty=" << f.qty
    //               << " price=" << f.price << "\n";
    // });

    // ── ETF client ───────────────────────────────────────────────────────────
    ETFClient etf(ETF_URL, TEAM_NAME, PASSWORD);
    if (!etf.health_check()) {
        std::cerr << "ETF service unreachable\n";
        return 1;
    }

    // ── Launch market data thread ────────────────────────────────────────────
    std::thread md_thread([&]() {
        run_listener(sm);
    });

    // ── Wait briefly for order books to populate ─────────────────────────────
    std::cout << "Waiting 3s for market data snapshot...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ── Launch arb strategy ───────────────────────────────────────────────────
    ETFArb arb(sm, oe, etf);
    std::thread arb_thread([&]() {
        arb.run();
    });

    md_thread.join();
    arb_thread.join();
    return 0;
}