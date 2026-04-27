#pragma once

#include <atomic>
#include <cstdint>

#include "symbol_manager.h"
#include "etf_client.h"
#include "oe_client.h"

// Minimum edge in price ticks required before firing an arb.
// Covers transaction costs and leg risk. Calibrate on Day 1.
static constexpr int32_t MIN_EDGE = 10;

// ETFArb continuously scans for ETF arb opportunities and executes them.
// Runs on its own thread. Reads from SymbolManager (lock-free),
// fires orders via OEClient, and calls ETFClient for create/redeem.
class ETFArb {
public:
    ETFArb(SymbolManager& sm, OEClient& oe, ETFClient& etf);

    // Blocks forever. Run on a dedicated thread.
    void run();

    // Signal the loop to stop cleanly.
    void stop() { running_.store(false, std::memory_order_release); }

private:
    SymbolManager& sm_;
    OEClient&      oe_;
    ETFClient&     etf_;

    std::atomic<bool>     running_{true};
    std::atomic<uint64_t> next_order_id_{1000};  // unique IDs for our orders
    std::unordered_map<uint64_t, std::pair<uint32_t, SIDE>> order_map_;
    static constexpr bool DEBUG_LOG = true; 

    uint64_t next_id() {
        return next_order_id_.fetch_add(1, std::memory_order_relaxed);
    }

    // Creation arb: buy all 10 dorms, /create, sell UNDY
    // Returns true if the arb was attempted
    bool try_creation_arb(const ArbSnapshot& snap);

    // Redemption arb: buy UNDY, /redeem, sell all 10 dorms
    // Returns true if the arb was attempted
    bool try_redemption_arb(const ArbSnapshot& snap);

    // Compute max qty we can safely trade for creation arb
    // Limited by position limits, available depth, and UNDY bid size
    int32_t creation_qty(const ArbSnapshot& snap) const;

    // Compute max qty we can safely trade for redemption arb
    int32_t redemption_qty(const ArbSnapshot& snap) const;
};