# HFT Algorithms: ETF Arbitrage Trading Bot

A high-frequency trading (HFT) system designed to execute ETF arbitrage strategies with sophisticated risk management. This project implements a market maker and order routing system that trades between an exchange and an ETF pricing service.

## Project Overview

This is a multi-threaded C++17 trading bot that:
- **Monitors orderbooks** for two symbols (GOLD and BLUE) from an exchange
- **Executes ETF arbitrage strategies** by pricing and trading against an external ETF service
- **Manages risk** through position limits, exposure tracking, and PnL monitoring
- **Routes orders** between an order entry (OE) client and exchange connections
- **Tracks fills** and maintains real-time position/PnL updates

## Architecture

### Core Components

#### Networking & Order Entry
- **OEClient** (`oe_client.{h,cpp}`): Socket-based connection to the exchange order entry system. Handles login, order submission, and fill callbacks.
- **ETFClient** (`etf_client.{h,cpp}`): HTTP client for retrieving ETF prices and net asset value from the pricing service.
- **Listener** (`listener.{h,cpp}`): Market data listener that connects to the exchange and streams orderbook updates.

#### Orderbook Management
- **OrderBook** (`orderbook.{h,cpp}`): In-memory orderbook structure that maintains bids/asks and handles new orders, deletions, modifications, and trades.
- **SymbolManager** (`symbol_manager.{h,cpp}`): Manages multiple orderbooks and provides symbol lookup/access.

#### Trading Strategy
- **ETFArb** (`etf_arb.{h,cpp}`): Core arbitrage strategy engine that:
  - Monitors GOLD and BLUE symbols from the exchange
  - Fetches ETF prices for arbitrage opportunities
  - Identifies bid-ask spreads and edges
  - Submits orders when profitable opportunities exist
  - Tracks its own order fills

#### Risk Management
- **RiskManager** (`risk_manager.{h,cpp}`): Central risk control system that enforces:
  - Per-order quantity limits
  - Per-side quantity limits
  - Total exposure limits
  - Absolute position limits
  - Order rate limits (orders/second)
  - Order rate per market data sequence number
  - Maximum unacknowledged orders
  - PnL floors (shutdown if losses exceed threshold)

- **PositionTracker** (`position_tracker.{h,cpp}`): Tracks net position (long/short).
- **ExposureTracker** (`exposure_tracker.{h,cpp}`): Tracks outstanding orders and total market exposure.
- **PnLTracker** (`pnl_tracker.{h,cpp}`): Computes realized and unrealized P&L.

#### Message Processing
- **messages.h**: Common message structures (SIDE enum, order types, etc.)
- **oe_messages.h**: Order entry specific message protocols.
- **iorder_sender.h**: Interface for order submission.

### Configuration

Key trading parameters in [main.cpp](main.cpp):
```cpp
EXCHANGE_HOST = "192.168.13.100"
EXCHANGE_PORT = 1234
ETF_URL = "http://129.74.160.245:5000"
TEAM_NAME = "group8"
CLIENT_ID = 8

GOLD_TICK = 10          // price tick for GOLD symbol
BLUE_TICK = 5           // price tick for BLUE symbol
MM_POSITION_LIMIT = 8   // position limit for market making
MIN_EDGE = 10           // minimum profit edge required for arbitrage
```

## Building

```bash
# Build all targets
make all

# Build specific targets
make bot              # Main trading bot
make listener         # Market data listener
make oe_client        # Order entry client standalone
make tests            # Unit tests for risk management

# Clean build artifacts
make clean
```

## Running

### Start the Market Data Listener
```bash
make run_listener
# or
./listener
```
This connects to the exchange and streams orderbook data.

### Run Unit Tests
```bash
make run_tests
# or
./tests
```
Tests risk management logic (position limits, exposure, PnL).

### Start the Trading Bot
```bash
make run_bot
# or
./bot
```
Starts the main arbitrage bot with:
- OE client connection to the exchange
- ETF pricing client for valuation
- Market data listener in a separate thread
- ETF arbitrage strategy engine
- Risk manager for trade approval

### Run Order Entry Client (Standalone)
```bash
make run_oe
# or
./oe_client
```

## File Structure

```
├── main.cpp                    # Bot entry point and threading orchestration
├── bot                         # Compiled bot executable
│
├── Networking
├── oe_client.{h,cpp}          # Order entry socket client
├── etf_client.{h,cpp}         # HTTP ETF pricing client
├── listener.{h,cpp}           # Market data listener
├── oe_messages.h              # Order entry protocol
│
├── Market Data & Orderbooks
├── orderbook.{h,cpp}          # In-memory orderbook
├── symbol_manager.{h,cpp}     # Multi-symbol management
├── messages.h                 # Common message types
│
├── Strategy
├── etf_arb.{h,cpp}            # ETF arbitrage engine
├── iorder_sender.h            # Order sending interface
│
├── Risk Management
├── risk_manager.{h,cpp}       # Central risk control
├── position_tracker.{h,cpp}   # Position tracking
├── exposure_tracker.{h,cpp}   # Exposure tracking
├── pnl_tracker.{h,cpp}        # P&L tracking
│
├── Testing
├── test_risk.cpp              # Risk manager unit tests
├── test_symbol_manager.cpp    # Symbol manager tests
├── test_etf_client.cpp        # ETF client tests
│
├── Build & Logs
├── Makefile                   # Build configuration
├── bot_log.txt               # Bot execution log
├── oe_log.txt                # Order entry log
│
└── Executables (generated)
    ├── listener              # Market data listener
    ├── oe_client             # Order entry client
    └── tests                 # Risk management tests
```

## Key Design Patterns

### Thread Safety
- Uses `std::atomic` for lock-free synchronization of shared state
- Each component can run in its own thread with atomic flags for shutdown
- Order fill callbacks used for inter-component communication

### Risk-First Design
- All order submissions go through `RiskManager::check_order()`
- Multi-layered checks: price validity → quantity limits → exposure → position → rate limits
- Automatic shutdown on catastrophic loss

### Callback Architecture
- OE client registers fill callbacks with the strategy
- Strategy processes fills and updates risk tracking
- Enables decoupled, event-driven architecture

## Example Workflow

1. **Startup**: Bot connects to exchange (OE), ETF service, and market data
2. **Market Data**: Listener thread streams orderbook updates
3. **Strategy**: ETFArb periodically checks:
   - Latest GOLD/BLUE bids/asks from orderbook
   - Latest ETF pricing
   - Profitability of arbitrage opportunities
4. **Risk Check**: RiskManager validates proposed order against all limits
5. **Execution**: Order sent to exchange via OEClient
6. **Fill Processing**: Fill callback updates position, exposure, and PnL
7. **Shutdown**: Graceful exit on global shutdown flag or excessive losses

## Performance Considerations

- **C++17 with optimizations** (`-O2` flag)
- **Lock-free atomics** for frequently-accessed shared state
- **Orderbook indexed** for fast best bid/ask lookups
- **Thread-per-component** for parallelism
- **Pre-allocated vectors** in orderbook to prevent reallocations

## Testing

Unit tests verify:
- Position tracking (long/short, limits)
- Exposure calculation (orders + positions)
- PnL computation (realized/unrealized)
- Risk rule enforcement (order approval/rejection)

Run with `make run_tests` or `./tests`

## Dependencies

- **C++17 Standard Library** (std::thread, std::atomic, std::unordered_map, etc.)
- **POSIX sockets** for exchange connectivity
- **libcurl** (implied for HTTP ETF client)
- **Standard math libraries** for PnL calculations

Compiled with `g++ -std=c++17 -O2 -pthread`

## Notes

- This is an educational/competition trading system (group8 team)
- Designed for a specific exchange protocol and ETF pricing service
- Not production-ready without additional monitoring, logging, and safety features
- All trading parameters should be tuned based on market conditions and risk tolerance

## Market Making Strategy

### Overview

The `MarketMaker` class implements a simple but effective market-making strategy for GOLD and BLUE symbols. It quotes bid/ask prices around the market midpoint, captures the spread, and remains inventory-neutral.

### How It Works

**Core Logic:**
- Subscribes to orderbook updates via `on_book_update()`
- Calculates the market midpoint: `mid = (best_bid + best_ask) / 2`
- Quotes symmetrically around mid: 
  - `your_bid = mid - (SPREAD_TICKS × tick_size)`
  - `your_ask = mid + (SPREAD_TICKS × tick_size)`
- Respects symbol tick sizes (GOLD=10, BLUE=5)
- Cancels and replaces quotes to stay in the market as mid moves

**Inventory Skew (Anti-Toxicity):**
- Applies position-based skew to defend against directional picking
- When position is long, quotes become tighter on bid (encourages selling) and wider on ask (discourages buying)
- When position is short, reverses this to encourage buying
- Formula: `skew = position × tick_size`

**Position Limits:**
- Stops quoting bids when at `+POS_LIMIT` (prevents excessive longs)
- Stops quoting asks when at `-POS_LIMIT` (prevents excessive shorts)
- `POS_LIMIT = 8` matches the global market maker position limit

### Tuning Parameters

Edit these in [market_maker.h](market_maker.h) lines 32-34:

```cpp
static constexpr int32_t SPREAD_TICKS   = 2;   // quote 2 ticks each side
static constexpr uint32_t QUOTE_SIZE    = 1;   // shares per side
static constexpr int32_t POS_LIMIT      = 8;   // absolute position limit
```

**Guidance:**

| Parameter | Conservative | Aggressive | Notes |
|-----------|---|---|---|
| `SPREAD_TICKS` | 3 | 1 | Wider = safer, less fills. Tighter = more fills, higher risk. |
| `QUOTE_SIZE` | 1 | 3 | Smaller = lower inventory accumulation. Larger = more profit per fill but higher risk. |
| `POS_LIMIT` | 5 | 8 | Lower = flatter, reduces exposure. Higher = allows larger positions. |

**Tuning Strategy:**
1. Start conservative (SPREAD_TICKS=3, QUOTE_SIZE=1)
2. If not getting fills: widen quotes (SPREAD_TICKS↓) or increase size (QUOTE_SIZE↑)
3. If accumulating toxic positions: increase skew effect or widen spread
4. If filling constantly but not profitable: widen spread (capture more)

### Integration

**In main.cpp:**
```cpp
// Create MarketMaker (passes OEClient which implements IOrderSender)
MarketMaker mm(oe, sm, SYM_GOLD, SYM_BLUE);

// In market maker thread:
std::thread mm_thread([&]() {
    std::cout << "[MM] Market maker thread started\n";
    while (!global_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        mm.on_book_update(SYM_GOLD);
        mm.on_book_update(SYM_BLUE);
    }
});
```

### Risk Management

The MarketMaker respects these guardrails:

1. **Position Limits**: Enforced by `POS_LIMIT` check before sending orders
2. **Tick Size Compliance**: All prices rounded to valid multiples
3. **Inventory Skew**: Position-aware pricing prevents toxic accumulation
4. **Graceful Cancellation**: Orders cancelled if position limits reached
5. **Atomic Order IDs**: Thread-safe tracking of resting orders

### Monitoring & Debugging

**Check MarketMaker activity:**
```bash
# Watch stdout for [MM] messages
./bot | grep "\[MM\]"
```

**Signals of problems:**
- Position stuck at +8 or -8 (not oscillating) → Someone picking you off, widen spread
- No fills for 10+ seconds → Quotes too wide, tighten spread or increase size
- Position whipsaw (long→short→long repeatedly) → Normal MM behavior, but watch PnL
- Fills only on one side → Market is trending, use skew more aggressively

### Files

- [market_maker.h](market_maker.h): Header with class definition and tunable parameters
- [market_maker.cpp](market_maker.cpp): Implementation of quoting logic and skew calculation
- [main.cpp](main.cpp): Integration point (lines ~118-127 show mm_thread setup)
