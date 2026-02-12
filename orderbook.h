#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <map>
#include <unordered_map>
#include <cstdint>
#include "messages.h"

//Stores information about a single order
struct OrderInfo {
    int32_t price;
    uint32_t quantity;
    SIDE side;
    uint32_t symbol;  // Track which symbol this order is for
};

// OrderBook manages buy/sell orders at different price levels
// Uses price-time priority (FIFO at each price level)
class OrderBook {
public:
    OrderBook();
    
    // Process different message types
    void handle_new_order(const new_order* msg);
    void handle_delete_order(const delete_order* msg);
    void handle_modify_order(const modify_order* msg);
    void handle_trade(const trade* msg);
    
    // Get best bid/ask prices and quantities
    int32_t get_best_bid_price() const;
    uint32_t get_best_bid_qty() const;
    int32_t get_best_ask_price() const;
    uint32_t get_best_ask_qty() const;
    
    // Utility for debugging/validation
    void print_book() const;
    bool is_crossed() const;  // Check if bid >= ask (error condition)

private:
    // Order lookup
    std::unordered_map<uint64_t, OrderInfo> orders_;
    
    // Price levels: Buy side sorted descending, Sell side sorted ascending
    // Maps price → total quantity at that price level
    std::map<int32_t, uint32_t, std::greater<int32_t>> bids_;  // Highest bid first
    std::map<int32_t, uint32_t> asks_;  // Lowest ask first
    
    // Helper methods
    void add_to_price_level(SIDE side, int32_t price, uint32_t quantity);
    void remove_from_price_level(SIDE side, int32_t price, uint32_t quantity);
};

#endif