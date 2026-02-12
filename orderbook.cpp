#include "orderbook.h"
#include <iostream>
#include <stdexcept>

OrderBook::OrderBook() {
    // Reserve space to avoid frequent reallocations
    orders_.reserve(100000);
}

void OrderBook::handle_new_order(const new_order* msg) {
    // Check if order already exists (shouldn't happen, but good to validate)
    if (orders_.find(msg->order_id) != orders_.end()) {
        std::cerr << "ERROR: Duplicate order_id " << msg->order_id << std::endl;
        return;
    }
    
    // Validate price and quantity
    if (msg->price <= 0) {
        std::cerr << "ERROR: Invalid price " << msg->price << " for order " << msg->order_id << std::endl;
        return;
    }
    if (msg->quantity == 0) {
        std::cerr << "ERROR: Zero quantity for order " << msg->order_id << std::endl;
        return;
    }
    
    // Store order info
    OrderInfo info;
    info.price = msg->price;
    info.quantity = msg->quantity;
    info.side = msg->side;
    info.symbol = msg->symbol;
    orders_[msg->order_id] = info;
    
    // Add to appropriate price level
    add_to_price_level(msg->side, msg->price, msg->quantity);
}

void OrderBook::handle_delete_order(const delete_order* msg) {
    // Find the order
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        std::cerr << "ERROR: Order " << msg->order_id << " not found for delete" << std::endl;
        return;
    }
    
    // Remove from price level
    remove_from_price_level(it->second.side, it->second.price, it->second.quantity);
    
    // Remove from order tracking
    orders_.erase(it);
}

void OrderBook::handle_modify_order(const modify_order* msg) {
    // Find the order
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        std::cerr << "ERROR: Order " << msg->order_id << " not found for modify" << std::endl;
        return;
    }
    
    // Validate new quantity and price
    if (msg->quantity == 0) {
        std::cerr << "ERROR: Modified quantity is zero for order " << msg->order_id << std::endl;
        return;
    }
    if (msg->price <= 0) {
        std::cerr << "ERROR: Invalid modified price " << msg->price << std::endl;
        return;
    }
    
    OrderInfo& order = it->second;
    
    // If price changed, need to move to different price level
    if (order.price != msg->price) {
        // Remove from old price level
        remove_from_price_level(order.side, order.price, order.quantity);
        // Add to new price level
        add_to_price_level(msg->side, msg->price, msg->quantity);
        order.price = msg->price;
    } else {
        // Price same, just update quantity at this level
        uint32_t qty_diff;
        if (msg->quantity > order.quantity) {
            qty_diff = msg->quantity - order.quantity;
            add_to_price_level(order.side, order.price, qty_diff);
        } else {
            qty_diff = order.quantity - msg->quantity;
            remove_from_price_level(order.side, order.price, qty_diff);
        }
    }
    
    order.quantity = msg->quantity;
    order.side = msg->side;
}

void OrderBook::handle_trade(const trade* msg) {
    // Find the order
    auto it = orders_.find(msg->order_id);
    if (it == orders_.end()) {
        std::cerr << "ERROR: Order " << msg->order_id << " not found for trade" << std::endl;
        return;
    }
    
    OrderInfo& order = it->second;
    
    // Validate trade quantity
    if (msg->quantity > order.quantity) {
        std::cerr << "ERROR: Trade quantity " << msg->quantity 
                  << " exceeds order quantity " << order.quantity << std::endl;
        return;
    }
    
    // Reduce quantity at price level
    remove_from_price_level(order.side, order.price, msg->quantity);
    
    // Update order quantity
    order.quantity -= msg->quantity;
    
    // If fully filled, remove order
    if (order.quantity == 0) {
        orders_.erase(it);
    }
}

int32_t OrderBook::get_best_bid_price() const {
    if (bids_.empty()) return 0;
    return bids_.begin()->first;
}

uint32_t OrderBook::get_best_bid_qty() const {
    if (bids_.empty()) return 0;
    return bids_.begin()->second;
}

int32_t OrderBook::get_best_ask_price() const {
    if (asks_.empty()) return 0;
    return asks_.begin()->first;
}

uint32_t OrderBook::get_best_ask_qty() const {
    if (asks_.empty()) return 0;
    return asks_.begin()->second;
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
            if (it->second <= quantity) {
                // Remove price level entirely if quantity goes to 0
                bids_.erase(it);
            } else {
                it->second -= quantity;
            }
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            if (it->second <= quantity) {
                asks_.erase(it);
            } else {
                it->second -= quantity;
            }
        }
    }
}

bool OrderBook::is_crossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return get_best_bid_price() >= get_best_ask_price();
}

void OrderBook::print_book() const {
    std::cout << "\n=== Order Book ===" << std::endl;
    std::cout << "Best Bid: " << get_best_bid_price() << " @ " << get_best_bid_qty() << std::endl;
    std::cout << "Best Ask: " << get_best_ask_price() << " @ " << get_best_ask_qty() << std::endl;
    std::cout << "Total Orders: " << orders_.size() << std::endl;
    if (is_crossed()) {
        std::cout << "WARNING: Book is crossed!" << std::endl;
    }
}