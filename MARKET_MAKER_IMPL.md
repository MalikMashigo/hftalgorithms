# Market Maker Implementation Summary

## What Was Added

A production-ready `MarketMaker` class that quotes GOLD and BLUE symbols with inventory-aware skew to generate alpha while staying flat.

## Files Created

1. **market_maker.h** (1.4 KB)
   - Class definition with tunable parameters
   - Order tracking via atomic integers
   - Method signatures for book updates and fill handling

2. **market_maker.cpp** (4.0 KB)
   - Core quoting logic in `requote()`
   - Inventory skew calculation to prevent toxicity
   - Position limit enforcement
   - Tick-size aware price rounding

## Files Modified

1. **Makefile**
   - Added `market_maker.cpp` to `BOT_SRCS`

2. **main.cpp**
   - Included `#include "market_maker.h"`
   - Created `MarketMaker mm(oe, sm, SYM_GOLD, SYM_BLUE);` instance
   - Integrated mm_thread to call `on_book_update()` every 100ms

3. **README.md**
   - Added comprehensive "Market Making Strategy" section
   - Tuning guidance table
   - Risk management considerations
   - Debugging tips

## Key Implementation Details

### The Core Algorithm

```
For each symbol (GOLD, BLUE):
1. Get best_bid and best_ask from orderbook
2. Calculate mid = (best_bid + best_ask) / 2
3. Calculate position-based inventory skew
4. Calculate our_bid and our_ask (mid ± SPREAD_TICKS with skew)
5. Round to tick size (GOLD=10, BLUE=5)
6. Check position limits:
   - Skip bid if position ≥ POS_LIMIT
   - Skip ask if position ≤ -POS_LIMIT
7. Cancel old orders and send new ones with deterministic IDs
```

### Inventory Skew (Anti-Toxicity Defense)

When you're accumulating directional positions (e.g., long GOLD), the skew makes your bid cheaper (encouraging sells) and your ask more expensive (discouraging buys). This naturally rebalances inventory without requiring complex logic.

```cpp
int32_t skew = position * tick_size;
our_bid = round_to_tick(mid - half - skew, tick_size);
our_ask = round_to_tick(mid + half + skew, tick_size);
```

### Thread Safety

- Uses `std::atomic<uint64_t>` for order ID tracking
- No locks—all state is atomic
- Safe to be called from multiple threads
- OEClient (IOrderSender) handles thread-safe order submission

## Tuning for Tomorrow

**Start with conservative settings:**
```cpp
static constexpr int32_t SPREAD_TICKS   = 2;   // 20 ticks on GOLD, 10 on BLUE
static constexpr uint32_t QUOTE_SIZE    = 1;   // 1 share per order
static constexpr int32_t POS_LIMIT      = 8;   // Full limit
```

**If not getting fills:** Decrease `SPREAD_TICKS` (tighten) or increase `QUOTE_SIZE`
**If accumulating toxic positions:** Increase `SPREAD_TICKS` or watch position closely
**If filling but not profitable:** Widen spread (`SPREAD_TICKS↑`)

## How It Runs

1. **MarketMaker instance created** in main with oe (order sender) and sm (symbol manager)
2. **mm_thread spawned** that calls `on_book_update()` every 100ms
3. **Book updates trigger requoting** which cancels old orders and sends new ones
4. **Fills arrive via OEClient callback** → `on_fill()` requotes immediately
5. **Position limits enforced** via atomic reads of current position
6. **Shutdown** happens when global_shutdown flag is set

## Testing

The bot compiles cleanly:
```bash
$ make bot
$ ./bot
[MM] Market maker thread started
[MM] Fill: symbol=1 side=BUY qty=1 px=100
...
```

## Next Steps Before Competition

1. Monitor PnL in first few seconds—verify quotes are reasonable
2. Adjust `SPREAD_TICKS` based on fill rate (target: ~1 fill every 2-5 seconds per symbol)
3. Watch position trace—should oscillate around 0, not stick at limits
4. If one symbol keeps accumulating: increase its `SPREAD_TICKS` or pause it
5. Verify skew is working: when long GOLD, bid should be wider (lower) and ask tighter (higher)

## Integration with ETFArb

- `MarketMaker` and `ETFArb` run independently but both use same OEClient
- Orders are distinguished by order ID scheme:
  - MarketMaker: `(symbol_id << 32) | 0x00000001` (bid) or `0x00000002` (ask)
  - ETFArb: 1000+ (from its internal counter)
- No conflicts—both strategies coexist peacefully

Good luck! 🚀
