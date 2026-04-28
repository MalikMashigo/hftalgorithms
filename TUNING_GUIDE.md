# Quick Reference: MarketMaker Tuning

## Adjust These 3 Parameters (in market_maker.h)

```cpp
// Line 32: How far away to quote from mid (in ticks)
static constexpr int32_t SPREAD_TICKS   = 2;

// Line 33: Order size (shares per quote)
static constexpr uint32_t QUOTE_SIZE    = 1;

// Line 34: Position limit where quoting stops (absolute value)
static constexpr int32_t POS_LIMIT      = 8;
```

## Decision Tree

**Not getting fills?**
- Decrease SPREAD_TICKS (quote closer to mid)
- Increase QUOTE_SIZE (post larger orders)

**Getting filled but not profitable?**
- Increase SPREAD_TICKS (capture bigger spread)
- Watch if someone is systematically picking you off

**Position accumulating in one direction?**
- Increase SPREAD_TICKS (widen quotes, invite rebalancing)
- Check if market is trending (compare to ETFArb positions)
- Look at fill log to see which side is getting picked

**Position oscillating crazily?**
- Normal behavior—means market is active
- Check that skew is working (position should influence price)
- Verify fills are happening on right side when long/short

## One-Line Tests

```bash
# Check it compiles
make clean && make bot

# Run the bot (will connect to exchange)
./bot

# Monitor fills in real-time
./bot | grep "\[MM\]"

# Watch positions (in another terminal)
while sleep 1; do grep "\[PnL\]" oe_log.txt | tail -1; done
```

## Implementation Details

- **File**: market_maker.{h,cpp}
- **Lines**: 43 header lines + 104 implementation lines
- **Thread**: Integrated in main.cpp mm_thread, updates every 100ms
- **Algorithm**: Mid-based bidder with position skew
- **Tick compliance**: GOLD=10, BLUE=5 (enforced via round_to_tick)
- **Safety**: Position limits, atomic order IDs, no locks needed

## What It Actually Does

1. Every 100ms, checks GOLD and BLUE orderbooks
2. Calculates mid = (best_bid + best_ask) / 2
3. Applies position skew: skew = position × tick_size
4. Quotes: bid = mid - 2×tick - skew, ask = mid + 2×tick + skew
5. Cancels old orders, sends new ones
6. On fill: requotes immediately to stay active

## Expected Behavior

- Quotes refresh every 100-200ms (faster if fills happen)
- Should see fills every few seconds on both sides
- Position should bounce between -8 and +8
- PnL should grow steadily (collect spreads)
- Occasional whipsaws are normal (shows market is trading)
