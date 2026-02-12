#include "orderbook.h"
#include <iostream>
#include <stdexcept>

void OrderBook::handle_new_order(const new_order* msg) {
    if (msg->header.msg_type != MSG_TYPE::NEW_ORDER) return;
    
    // Check for duplicate order
    if (orders_.find(msg->order_id) != orders_.end()) {
        std::cerr << "ERROR: Duplicate order ID " << msg->order_id << std::endl;
        throw std::runtime_error("Duplicate order ID");
    }
    
    // Validate price and quantity
    if (msg->quantity == 0) {
        std::cerr << "ERROR: Zero quantity order" << std::endl;
        throw std::runtime_error("Zero quantity");
    }
    
    if (msg->price < 0) {
        std::cerr << "ERROR: Negative price" << std::endl;
        throw std::runtime_error("Negative price");
    }
    
    // Store order info
    OrderInfo info;
    info.price = msg->price;
    info.quantity = msg->quantity;
    info.side = msg->side;
    info.symbol = msg->symbol;
    
    orders_[msg->order_id] = info;
    add_to_price_level(msg->side, msg->price, msg->quantity);
    
    // Update last sequence number
    last_seq_num_ = msg->header.seq_num;
    
    // Check for crossed book
    if (is_crossed()) {
        std::cerr << "ERROR: Book is crossed after new order!" << std::endl;
        print_book();
        throw std::runtime_error("Crossed book");
    }
}

void OrderBook::handle_delete_order(const delete_order* msg) {
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        // Order not found - might have been fully traded
        return;
    }
    
    const OrderInfo& info = it->second;
    
    // Only process if this order belongs to our symbol
    if (info.symbol != symbol_) {
        return;
    }
    
    remove_from_price_level(info.side, info.price, info.quantity);
    orders_.erase(it);
    
    last_seq_num_ = msg->header.seq_num;
}

void OrderBook::handle_modify_order(const modify_order* msg) {
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        std::cerr << "WARNING: Modifying non-existent order " << msg->order_id << std::endl;
        return;
    }
    
    OrderInfo& info = it->second;
    
    // Only process if this order belongs to our symbol
    if (info.symbol != symbol_) {
        return;
    }
    
    // Remove old quantity from price level
    remove_from_price_level(info.side, info.price, info.quantity);
    
    // Update order info
    info.price = msg->price;
    info.quantity = msg->quantity;
    info.side = msg->side;
    
    // Add new quantity to (possibly new) price level
    add_to_price_level(info.side, info.price, info.quantity);
    
    last_seq_num_ = msg->header.seq_num;
    
    if (is_crossed()) {
        std::cerr << "ERROR: Book crossed after modify!" << std::endl;
        throw std::runtime_error("Crossed book");
    }
}

void OrderBook::handle_trade(const trade* msg) {
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        // Order might have been deleted or fully traded already
        return;
    }
    
    OrderInfo& info = it->second;
    
    // Only process if this order belongs to our symbol
    if (info.symbol != symbol_) {
        return;
    }
    
    if (msg->quantity > info.quantity) {
        std::cerr << "ERROR: Trade quantity " << msg->quantity 
                  << " exceeds order quantity " << info.quantity << "!" << std::endl;
        throw std::runtime_error("Invalid trade quantity");
    }
    
    // Reduce quantity at price level
    remove_from_price_level(info.side, info.price, msg->quantity);
    info.quantity -= msg->quantity;
    
    // If fully executed, remove order
    if (info.quantity == 0) {
        orders_.erase(it);
    }
    
    last_seq_num_ = msg->header.seq_num;
}

int32_t OrderBook::get_best_bid_price() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

uint32_t OrderBook::get_best_bid_qty() const {
    return bids_.empty() ? 0 : bids_.begin()->second;
}

int32_t OrderBook::get_best_ask_price() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

uint32_t OrderBook::get_best_ask_qty() const {
    return asks_.empty() ? 0 : asks_.begin()->second;
}

void OrderBook::add_to_price_level(SIDE side, int32_t price, uint32_t quantity) {
    if (side == SIDE::BUY) {
        bids_[price] += quantity;
    } else {
        asks_[price] += quantity;
    }
}

void OrderBook::remove_from_price_level(SIDE side, int32_t price, uint32_t quantity) {
    if (side == SIDE::BUY) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            if (it->second < quantity) {
                std::cerr << "ERROR: Removing more quantity (" << quantity 
                          << ") than exists (" << it->second << ") at bid price level " 
                          << price << std::endl;
                throw std::runtime_error("Invalid quantity removal");
            }
            it->second -= quantity;
            if (it->second == 0) {
                bids_.erase(it);
            }
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            if (it->second < quantity) {
                std::cerr << "ERROR: Removing more quantity (" << quantity 
                          << ") than exists (" << it->second << ") at ask price level " 
                          << price << std::endl;
                throw std::runtime_error("Invalid quantity removal");
            }
            it->second -= quantity;
            if (it->second == 0) {
                asks_.erase(it);
            }
        }
    }
}

bool OrderBook::is_crossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return bids_.begin()->first >= asks_.begin()->first;
}

void OrderBook::print_book() const {
    std::cout << "\n=== Order Book (Symbol " << symbol_ << ") ===" << std::endl;
    std::cout << "ASKS:" << std::endl;
    for (auto it = asks_.rbegin(); it != asks_.rend(); ++it) {
        std::cout << "  " << it->first << " @ " << it->second << std::endl;
    }
    std::cout << "---" << std::endl;
    for (const auto& bid : bids_) {
        std::cout << "  " << bid.first << " @ " << bid.second << std::endl;
    }
    std::cout << "BIDS:" << std::endl;
    std::cout << "Best Bid: " << get_best_bid_price() << " @ " << get_best_bid_qty() << std::endl;
    std::cout << "Best Ask: " << get_best_ask_price() << " @ " << get_best_ask_qty() << std::endl;
}