#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "symbol_manager.h"
#include "etf_client.h"
#include "oe_client.h"

static constexpr int32_t MIN_EDGE = 10;

class ETFArb {
public:
    ETFArb(SymbolManager& sm, OEClient& oe, ETFClient& etf,
           std::atomic<bool>& shutdown);

    void run();
    void stop() { running_.store(false, std::memory_order_release); }

private:
    SymbolManager&     sm_;
    OEClient&          oe_;
    ETFClient&         etf_;
    std::atomic<bool>& global_shutdown_;
    std::atomic<bool>  running_{true};
    std::atomic<uint64_t> next_order_id_{1000};

    std::unordered_map<uint64_t, std::pair<uint32_t, SIDE>> order_map_;

    uint64_t next_id() {
        return next_order_id_.fetch_add(1, std::memory_order_relaxed);
    }

    bool    try_creation_arb   (const ArbSnapshot& snap);
    bool    try_redemption_arb (const ArbSnapshot& snap);
    int32_t creation_qty       (const ArbSnapshot& snap) const;
    int32_t redemption_qty     (const ArbSnapshot& snap) const;
    void    unwind_dorm_longs  ();
};