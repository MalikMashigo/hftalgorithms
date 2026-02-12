#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <map>
#include <unordered_map>
#include <cstdint>
#include <iostream>
#include "messages.h"

struct OrderInfo {
    int32_t price;
    uint32_t quantity;
    SIDE side;
    uint32_t symbol;
};

class OrderBook {
public:
    OrderBook(uint32_t symbol_id) : symbol_(symbol_id), last_seq_num_(0) {}
    
    void handle_new_order(const new_order* msg);
    void handle_delete_order(const delete_order* msg);
    void handle_modify_order(const modify_order* msg);
    void handle_trade(const trade* msg);
    
    int32_t get_best_bid_price() const;
    uint32_t get_best_bid_qty() const;
    int32_t get_best_ask_price() const;
    uint32_t get_best_ask_qty() const;
    
    void print_book() const;
    bool is_crossed() const;
    
    uint32_t get_symbol() const { return symbol_; }
    uint32_t get_last_seq_num() const { return last_seq_num_; }
    void set_last_seq_num(uint32_t seq) { last_seq_num_ = seq; }

private:
    uint32_t symbol_;
    uint32_t last_seq_num_;
    
    std::unordered_map<uint64_t, OrderInfo> orders_;
    std::map<int32_t, uint32_t, std::greater<int32_t>> bids_;
    std::map<int32_t, uint32_t> asks_;
    
    void add_to_price_level(SIDE side, int32_t price, uint32_t quantity);
    void remove_from_price_level(SIDE side, int32_t price, uint32_t quantity);
};

#endif