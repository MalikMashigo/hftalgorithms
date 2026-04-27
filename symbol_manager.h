#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <cstdint>

#include "orderbook.h"
#include "messages.h"

// ── Symbol ID constants ───────────────────────────────────────────────────────

static constexpr uint32_t SYM_GOLD = 1;
static constexpr uint32_t SYM_BLUE = 2;
static constexpr uint32_t SYM_KNAN = 3;
static constexpr uint32_t SYM_STED = 4;
static constexpr uint32_t SYM_FISH = 5;
static constexpr uint32_t SYM_DILN = 6;
static constexpr uint32_t SYM_SORN = 7;
static constexpr uint32_t SYM_RYAN = 8;
static constexpr uint32_t SYM_LYON = 9;
static constexpr uint32_t SYM_WLSH = 10;
static constexpr uint32_t SYM_LEWI = 11;
static constexpr uint32_t SYM_BDIN = 12;
static constexpr uint32_t SYM_UNDY = 13;

static constexpr std::array<uint32_t, 10> DORM_IDS = {
    SYM_KNAN, SYM_STED, SYM_FISH, SYM_DILN, SYM_SORN,
    SYM_RYAN, SYM_LYON, SYM_WLSH, SYM_LEWI, SYM_BDIN
};

// ── ArbSnapshot ───────────────────────────────────────────────────────────────
// All data needed to evaluate one ETF arb opportunity, read in a single pass
// from the atomic caches. No locks taken. Individual reads are not
// guaranteed to be mutually consistent (prices can shift between reads),
// but the arb edge is large enough that this is acceptable in practice.
// The strategy must re-validate before firing orders anyway.
struct ArbSnapshot {
    struct DormData {
        int32_t  best_bid_price;
        int32_t  best_ask_price;
        uint32_t best_bid_qty;
        uint32_t best_ask_qty;
        int32_t  position;
    };
    std::array<DormData, 10> dorms;  // indexed 0–9, matching DORM_IDS order

    int32_t  undy_best_bid_price;
    int32_t  undy_best_ask_price;
    uint32_t undy_best_bid_qty;
    uint32_t undy_best_ask_qty;
    int32_t  undy_position;

    // Derived NAV values computed during snapshot
    int32_t nav_ask;               // sum of all 10 dorm best asks
    int32_t nav_bid;               // sum of all 10 dorm best bids
    bool    any_dorm_ask_missing;  // true if any dorm has no ask
    bool    any_dorm_bid_missing;  // true if any dorm has no bid
};

// ── SymbolManager ─────────────────────────────────────────────────────────────
//
// Owns one OrderBook and one atomic top-of-book cache per symbol (13 total).
//
// Threading model:
//   Market data thread — calls on_new_order / on_delete / on_modify / on_trade
//                        which update the full OrderBook, then write atomics.
//   Strategy thread   — reads atomics via snapshot() or direct getters.
//   Fill thread       — calls on_fill(), which updates atomic positions.
//
// No mutexes, no kernel calls. All shared state is accessed via
// std::atomic with relaxed or release/acquire ordering:
//   - Writers use memory_order_release  (all prior writes visible to reader)
//   - Readers use memory_order_acquire  (sees all writes before the release)
//
// Position limit is 9 (one below the exchange hard limit of 10).

class SymbolManager {
public:
    static constexpr int32_t POSITION_LIMIT  = 9;
    static constexpr double  PNL_WARN_LEVEL  = -4500.0;

    SymbolManager();

    // ── Market data thread ───────────────────────────────────────────────────
    // Update the full order book, then flush top-of-book into the atomics.
    // Only ever called from the market data thread — no sharing of OrderBook.

    void on_new_order   (uint32_t symbol_id, const new_order*    msg);
    void on_delete_order(uint32_t symbol_id, const delete_order* msg);
    void on_modify_order(uint32_t symbol_id, const modify_order* msg);
    void on_trade       (uint32_t symbol_id, const trade*        msg);

    // ── Fill callback ────────────────────────────────────────────────────────
    // Called when one of our orders is filled.
    // Updates atomic position and global PnL via a CAS loop (no kernel call).

    void on_fill(uint32_t symbol_id, SIDE side, uint32_t qty, int32_t price);

    // ── Strategy thread reads ────────────────────────────────────────────────
    // All read directly from atomics — zero kernel involvement.

    ArbSnapshot snapshot() const;

    int32_t  best_bid_price(uint32_t symbol_id) const;
    int32_t  best_ask_price(uint32_t symbol_id) const;
    uint32_t best_bid_qty  (uint32_t symbol_id) const;
    uint32_t best_ask_qty  (uint32_t symbol_id) const;
    int32_t  get_position  (uint32_t symbol_id) const;

    // Returns true if adding `qty` on `side` would exceed POSITION_LIMIT
    bool would_breach_limit(uint32_t symbol_id, SIDE side, int32_t qty) const;

    // ── PnL ──────────────────────────────────────────────────────────────────
    double get_total_pnl()  const;
    bool   pnl_near_limit() const;

private:
    // ── Per-symbol slot ───────────────────────────────────────────────────────
    // OrderBook is only ever written by the market data thread so it needs
    // no synchronisation. The four atomic top-of-book values are the only
    // shared state between the MD thread and the strategy thread.
    struct SymbolSlot {
        // Full order book — market data thread only
        OrderBook book;

        // Top-of-book cache — written by MD thread, read by strategy thread.
        // Stored as atomics so the strategy can read without any lock.
        // Prices of 0 mean the side is empty.
        std::atomic<int32_t>  best_bid_price{0};
        std::atomic<uint32_t> best_bid_qty{0};
        std::atomic<int32_t>  best_ask_price{0};
        std::atomic<uint32_t> best_ask_qty{0};

        // Our net position in this symbol.
        // Written by fill callback, read by strategy thread.
        std::atomic<int32_t>  position{0};

        explicit SymbolSlot(uint32_t id) : book(id) {}

        // Non-copyable, non-movable (atomics)
        SymbolSlot(const SymbolSlot&)            = delete;
        SymbolSlot& operator=(const SymbolSlot&) = delete;

        // Called by the MD thread after every book update to flush
        // the latest top-of-book into the atomics.
        void flush_top_of_book() {
            best_bid_price.store(book.get_best_bid_price(), std::memory_order_release);
            best_bid_qty  .store(book.get_best_bid_qty(),   std::memory_order_release);
            best_ask_price.store(book.get_best_ask_price(), std::memory_order_release);
            best_ask_qty  .store(book.get_best_ask_qty(),   std::memory_order_release);
        }
    };

    //std::unordered_map<uint32_t, std::unique_ptr<SymbolSlot>> slots_;
    std::array<std::unique_ptr<SymbolSlot>, 14> slots_; //index 0 unused

    // Global PnL stored as an atomic double.
    // Updated via CAS loop in on_fill() — no kernel call.
    std::atomic<double> total_pnl_{0.0};

    // Safe slot accessor
    SymbolSlot&       slot(uint32_t symbol_id);
    const SymbolSlot& slot(uint32_t symbol_id) const;
};