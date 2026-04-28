#include "symbol_manager.h"

#include <stdexcept>
#include <cmath>
#include <string>

// ── Construction ──────────────────────────────────────────────────────────────

SymbolManager::SymbolManager() : total_pnl_(0.0) {
    for (uint32_t id = 1; id <= 13; ++id) {
        //slots_.emplace(id, std::make_unique<SymbolSlot>(id));
        slots_[id] = std::make_unique<SymbolSlot>(id);
    }
}

// ── Safe accessor ─────────────────────────────────────────────────────────────

SymbolManager::SymbolSlot& SymbolManager::slot(uint32_t id) {
    // auto it = slots_.find(id);
    // if (it == slots_.end())
    //     throw std::out_of_range("SymbolManager: unknown symbol_id "
    //                             + std::to_string(id));
    // return *it->second;
    return *slots_[id];  // direct array access, no hash
}

const SymbolManager::SymbolSlot& SymbolManager::slot(uint32_t id) const {
    // auto it = slots_.find(id);
    // if (it == slots_.end())
    //     throw std::out_of_range("SymbolManager: unknown symbol_id "
    //                             + std::to_string(id));
    // return *it->second;
    return *slots_[id];
}

// ── Market data thread ────────────────────────────────────────────────────────
// Pattern for every handler:
//   1. Update the full OrderBook (market data thread only — no sync needed)
//   2. Flush updated top-of-book into atomics with memory_order_release
//      so the strategy thread sees a consistent view on next acquire-load.

void SymbolManager::on_new_order(uint32_t id, const new_order* msg) {
    auto& s = slot(id);
    s.book.handle_new_order(msg);
    s.flush_top_of_book();
}

void SymbolManager::on_delete_order(uint32_t id, const delete_order* msg) {
    auto& s = slot(id);
    s.book.handle_delete_order(msg);
    s.flush_top_of_book();
}

void SymbolManager::on_modify_order(uint32_t id, const modify_order* msg) {
    auto& s = slot(id);
    s.book.handle_modify_order(msg);
    s.flush_top_of_book();
}

void SymbolManager::on_trade(uint32_t id, const trade* msg) {
    auto& s = slot(id);
    s.book.handle_trade(msg);
    s.flush_top_of_book();
}

void SymbolManager::reset_book(uint32_t symbol_id) {
    auto& s = slot(symbol_id);
    s.book = OrderBook(symbol_id);   // replace with fresh book
    s.best_bid_price.store(0, std::memory_order_release);
    s.best_bid_qty  .store(0, std::memory_order_release);
    s.best_ask_price.store(0, std::memory_order_release);
    s.best_ask_qty  .store(0, std::memory_order_release);
}

// ── Fill callback ─────────────────────────────────────────────────────────────
// Updates atomic position and global PnL.
// Uses a CAS loop for the PnL double — spins in user space, no kernel call.

void SymbolManager::on_fill(uint32_t symbol_id, SIDE side,
                             uint32_t qty, int32_t price) {
    auto& s = slot(symbol_id);
    int32_t old_pos = s.position.load(std::memory_order_acquire);

    // Update position
    if (side == SIDE::BUY)
        s.position.fetch_add(static_cast<int32_t>(qty), std::memory_order_release);
    else
        s.position.fetch_sub(static_cast<int32_t>(qty), std::memory_order_release);

    // Update average entry price
    if (side == SIDE::BUY) {
        double old_val = avg_entry_price_[symbol_id] * std::max(old_pos, 0);
        double new_qty = old_pos + static_cast<int32_t>(qty);
        avg_entry_price_[symbol_id] = new_qty > 0
            ? (old_val + price * qty) / new_qty
            : price;
    }

    // Only book realized PnL when reducing position
    bool reducing = (side == SIDE::SELL && old_pos > 0) ||
                    (side == SIDE::BUY  && old_pos < 0);

    if (!reducing) return;

    uint32_t closing_qty = std::min(qty, static_cast<uint32_t>(std::abs(old_pos)));
    double avg_entry     = avg_entry_price_[symbol_id];
    double realized      = (side == SIDE::SELL)
        ? (price - avg_entry) * closing_qty
        : (avg_entry - price) * closing_qty;

    double expected = total_pnl_.load(std::memory_order_relaxed);
    while (!total_pnl_.compare_exchange_weak(
               expected, expected + realized,
               std::memory_order_release,
               std::memory_order_relaxed)) {}
}

// ── Per-symbol reads ──────────────────────────────────────────────────────────
// All memory_order_acquire: guarantees we see all writes the MD thread
// did before its memory_order_release store.

int32_t SymbolManager::best_bid_price(uint32_t id) const {
    return slot(id).best_bid_price.load(std::memory_order_acquire);
}

int32_t SymbolManager::best_ask_price(uint32_t id) const {
    return slot(id).best_ask_price.load(std::memory_order_acquire);
}

uint32_t SymbolManager::best_bid_qty(uint32_t id) const {
    return slot(id).best_bid_qty.load(std::memory_order_acquire);
}

uint32_t SymbolManager::best_ask_qty(uint32_t id) const {
    return slot(id).best_ask_qty.load(std::memory_order_acquire);
}

int32_t SymbolManager::get_position(uint32_t id) const {
    return slot(id).position.load(std::memory_order_acquire);
}

bool SymbolManager::would_breach_limit(uint32_t symbol_id,
                                        SIDE side, int32_t qty) const {
    int32_t pos = get_position(symbol_id);
    int32_t new_pos = (side == SIDE::BUY) ? pos + qty : pos - qty;
    return std::abs(new_pos) > POSITION_LIMIT;
}

// ── Snapshot ──────────────────────────────────────────────────────────────────
// Reads all 13 atomic caches in one pass. Individual reads are not
// atomically consistent with each other (no lock taken), but this is
// acceptable: the strategy re-validates before sending any order.
// On x86 the entire snapshot takes ~50 ns — far faster than a mutex.

ArbSnapshot SymbolManager::snapshot() const {
    ArbSnapshot snap{};
    snap.nav_ask              = 0;
    snap.nav_bid              = 0;
    snap.any_dorm_ask_missing = false;
    snap.any_dorm_bid_missing = false;

    for (size_t i = 0; i < DORM_IDS.size(); ++i) {
        const auto& s = slot(DORM_IDS[i]);

        int32_t  bid_px  = s.best_bid_price.load(std::memory_order_acquire);
        int32_t  ask_px  = s.best_ask_price.load(std::memory_order_acquire);
        uint32_t bid_qty = s.best_bid_qty  .load(std::memory_order_acquire);
        uint32_t ask_qty = s.best_ask_qty  .load(std::memory_order_acquire);
        int32_t  pos     = s.position      .load(std::memory_order_acquire);

        snap.dorms[i] = { bid_px, ask_px, bid_qty, ask_qty, pos };

        if (ask_px == 0) snap.any_dorm_ask_missing = true;
        else             snap.nav_ask += ask_px;

        if (bid_px == 0) snap.any_dorm_bid_missing = true;
        else             snap.nav_bid += bid_px;
    }

    const auto& undy = slot(SYM_UNDY);
    snap.undy_best_bid_price = undy.best_bid_price.load(std::memory_order_acquire);
    snap.undy_best_ask_price = undy.best_ask_price.load(std::memory_order_acquire);
    snap.undy_best_bid_qty   = undy.best_bid_qty  .load(std::memory_order_acquire);
    snap.undy_best_ask_qty   = undy.best_ask_qty  .load(std::memory_order_acquire);
    snap.undy_position       = undy.position      .load(std::memory_order_acquire);

    return snap;
}

// ── PnL ───────────────────────────────────────────────────────────────────────

double SymbolManager::get_total_pnl() const {
    return total_pnl_.load(std::memory_order_acquire);
}

bool SymbolManager::pnl_near_limit() const {
    return get_total_pnl() <= PNL_WARN_LEVEL;
}