#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <map>
#include <fstream>
#include <queue>
#include "messages.h"
#include "orderbook.h"

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip);

struct BufferedMessage {
    char data[1500];
    size_t length;
};

int main(int, char *[])
{
    // Map from symbol ID to OrderBook
    std::map<uint32_t, OrderBook> books;
    
    // Track expected sequence numbers per symbol
    std::map<uint32_t, uint32_t> expected_seq_nums;
    
    // Buffer for live messages during catch-up
    std::queue<BufferedMessage> live_buffer;
    
    // File to record best bid/ask for homework checker
    std::ofstream outfile("bbo_data.csv");
    outfile << "seq_num,symbol,bid_price,bid_qty,ask_price,ask_qty" << std::endl;
    
    const char* live_addr = "239.0.0.1";
    const int live_port = 12345;
    const char* replay_addr = "239.0.0.2";
    const int replay_port = 12345;
    const char* local_ip = "192.168.13.16";

    int live_sock = create_multicast_socket(live_addr, live_port, local_ip);
    int replay_sock = create_multicast_socket(replay_addr, replay_port, local_ip);

    int epoll_fd = epoll_create(128);
    if (epoll_fd < 0) {
        std::cerr << "Failed to create epoll: " << strerror(errno) << std::endl;
        return 1;
    }

    // Add BOTH sockets from the start
    epoll_event event;
    event.data.fd = replay_sock;
    event.events = EPOLLIN;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, replay_sock, &event) < 0) {
        std::cerr << "Failed to add replay socket to epoll" << std::endl;
        return 1;
    }
    
    event.data.fd = live_sock;
    event.events = EPOLLIN;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, live_sock, &event) < 0) {
        std::cerr << "Failed to add live socket to epoll" << std::endl;
        return 1;
    }

    std::cout << "Starting to receive market data..." << std::endl;

    char buf[1500];
    bool caught_up = false;
    bool received_snapshot = false;
    uint32_t messages_processed = 0;
    int no_replay_count = 0;
    
    while (true) {
        epoll_event events[16];
        int timeout = caught_up ? -1 : 10;
        int nfds = epoll_wait(epoll_fd, events, 16, timeout);
        
        if (nfds < 0) {
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
            return 1;
        }
        
        // Check if we should switch to live-only mode
        if (nfds == 0 && !caught_up && received_snapshot) {
            no_replay_count++;
            if (no_replay_count > 100) {
                std::cout << "Caught up! Switching to live feed only..." << std::endl;
                std::cout << "Processing " << live_buffer.size() << " buffered messages..." << std::endl;
                caught_up = true;
                
                // Process buffered live messages
                while (!live_buffer.empty()) {
                    BufferedMessage& msg = live_buffer.front();
                    memcpy(buf, msg.data, msg.length);
                    
                    md_header* header = reinterpret_cast<md_header*>(buf);
                    
                    if (header->magic_number == MAGIC_NUMBER) {
                        switch (header->msg_type) {
                            case MSG_TYPE::NEW_ORDER: {
                                new_order* order_msg = reinterpret_cast<new_order*>(buf);
                                uint32_t symbol = order_msg->symbol;
                                uint32_t seq_num = order_msg->header.seq_num;
                                
                                if (books.find(symbol) != books.end()) {
                                    // Update expected sequence
                                    if (expected_seq_nums.find(symbol) != expected_seq_nums.end()) {
                                        // Skip if old
                                        if (seq_num < expected_seq_nums[symbol]) {
                                            live_buffer.pop();
                                            continue;
                                        }
                                        expected_seq_nums[symbol] = seq_num + 1;
                                    }
                                    
                                    books.at(symbol).handle_new_order(order_msg);
                                    
                                    outfile << seq_num << ","
                                            << symbol << ","
                                            << books.at(symbol).get_best_bid_price() << ","
                                            << books.at(symbol).get_best_bid_qty() << ","
                                            << books.at(symbol).get_best_ask_price() << ","
                                            << books.at(symbol).get_best_ask_qty() << std::endl;
                                }
                                break;
                            }
                            case MSG_TYPE::DELETE_ORDER: {
                                delete_order* del_msg = reinterpret_cast<delete_order*>(buf);
                                for (auto& pair : books) {
                                    pair.second.handle_delete_order(del_msg);
                                }
                                break;
                            }
                            case MSG_TYPE::MODIFY_ORDER: {
                                modify_order* mod_msg = reinterpret_cast<modify_order*>(buf);
                                for (auto& pair : books) {
                                    pair.second.handle_modify_order(mod_msg);
                                }
                                break;
                            }
                            case MSG_TYPE::TRADE: {
                                trade* trade_msg = reinterpret_cast<trade*>(buf);
                                for (auto& pair : books) {
                                    pair.second.handle_trade(trade_msg);
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    }
                    
                    live_buffer.pop();
                }
                
                std::cout << "Now processing live feed in real-time." << std::endl;
            }
            continue;
        }

        for (int i = 0; i < nfds; ++i) {
            ssize_t bytes = read(events[i].data.fd, buf, sizeof(buf));
            
            if (bytes <= 0) continue;
            
            // If this is from live feed and we haven't caught up, buffer it
            if (!caught_up && events[i].data.fd == live_sock) {
                BufferedMessage msg;
                memcpy(msg.data, buf, bytes);
                msg.length = bytes;
                live_buffer.push(msg);
                
                if (live_buffer.size() > 100000) {
                    std::cerr << "ERROR: Live buffer overflow!" << std::endl;
                    return 1;
                }
                continue;
            }

            md_header* header = reinterpret_cast<md_header*>(buf);
            
            // Handle snapshot messages
            if (header->magic_number == SNAPSHOT_MAGIC_NUMBER) {
                snapshot_info* snap = reinterpret_cast<snapshot_info*>(buf);
                
                uint32_t symbol = snap->symbol;
                uint32_t last_seq = snap->last_md_seq_num;
                
                std::cout << "Snapshot: symbol " << symbol << " at seq " << last_seq << std::endl;
                
                // Create or reset book
                if (books.find(symbol) == books.end()) {
                    books.emplace(symbol, OrderBook(symbol));
                } else {
                    // Clear existing book and rebuild from snapshot
                    books.erase(symbol);
                    books.emplace(symbol, OrderBook(symbol));
                }
                
                books.at(symbol).set_last_seq_num(last_seq);
                expected_seq_nums[symbol] = last_seq + 1;
                
                received_snapshot = true;
                no_replay_count = 0;
                
                continue;
            }
            
            // Handle normal market data
            if (header->magic_number != MAGIC_NUMBER) {
                continue;
            }
            
            uint32_t seq_num = header->seq_num;
            
            switch (header->msg_type) {
                case MSG_TYPE::NEW_ORDER: {
                    new_order* msg = reinterpret_cast<new_order*>(buf);
                    uint32_t symbol = msg->symbol;
                    
                    // Create book if doesn't exist
                    if (books.find(symbol) == books.end()) {
                        books.emplace(symbol, OrderBook(symbol));
                        expected_seq_nums[symbol] = seq_num;
                    }
                    
                    // During live mode, check sequence strictly
                    if (caught_up) {
                        if (expected_seq_nums.find(symbol) != expected_seq_nums.end()) {
                            if (seq_num != expected_seq_nums[symbol]) {
                                std::cerr << "FATAL: Live sequence gap for symbol " << symbol 
                                          << "! Expected " << expected_seq_nums[symbol]
                                          << " got " << seq_num << std::endl;
                                return 1;
                            }
                        }
                    }
                    
                    // Update expected sequence
                    expected_seq_nums[symbol] = seq_num + 1;
                    
                    books.at(symbol).handle_new_order(msg);
                    
                    // Record BBO
                    outfile << seq_num << ","
                            << symbol << ","
                            << books.at(symbol).get_best_bid_price() << ","
                            << books.at(symbol).get_best_bid_qty() << ","
                            << books.at(symbol).get_best_ask_price() << ","
                            << books.at(symbol).get_best_ask_qty() << std::endl;
                    
                    messages_processed++;
                    if (messages_processed % 1000 == 0) {
                        std::cout << "Processed " << messages_processed 
                                  << " messages (seq: " << seq_num << ")" << std::endl;
                    }
                    
                    no_replay_count = 0;
                    break;
                }
                case MSG_TYPE::DELETE_ORDER: {
                    delete_order* msg = reinterpret_cast<delete_order*>(buf);
                    for (auto& pair : books) {
                        pair.second.handle_delete_order(msg);
                    }
                    messages_processed++;
                    no_replay_count = 0;
                    break;
                }
                case MSG_TYPE::MODIFY_ORDER: {
                    modify_order* msg = reinterpret_cast<modify_order*>(buf);
                    for (auto& pair : books) {
                        pair.second.handle_modify_order(msg);
                    }
                    messages_processed++;
                    no_replay_count = 0;
                    break;
                }
                case MSG_TYPE::TRADE: {
                    trade* msg = reinterpret_cast<trade*>(buf);
                    for (auto& pair : books) {
                        pair.second.handle_trade(msg);
                    }
                    messages_processed++;
                    no_replay_count = 0;
                    break;
                }
                case MSG_TYPE::HEARTBEAT:
                    no_replay_count = 0;
                    break;
                default:
                    break;
            }
        }
        
        // Periodically print book status
        if (messages_processed % 5000 == 0 && messages_processed > 0 && caught_up) {
            for (const auto& pair : books) {
                std::cout << "Symbol " << pair.first 
                          << " - Bid: " << pair.second.get_best_bid_price() 
                          << "@" << pair.second.get_best_bid_qty()
                          << " Ask: " << pair.second.get_best_ask_price() 
                          << "@" << pair.second.get_best_ask_qty() << std::endl;
            }
        }
    }

    close(epoll_fd);
    close(live_sock);
    close(replay_sock);
    outfile.close();
    
    return 0;
}

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip) {
    int mcast_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mcast_fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        exit(1);
    }

    int reuse = 1;
    setsockopt(mcast_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(mcast_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    // Increase socket buffer size
    int rcvbuf = 8*1024*1024;
    setsockopt(mcast_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int flags = fcntl(mcast_fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "Could not get flags: " << strerror(errno) << std::endl;
        exit(1);
    }
    if (fcntl(mcast_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "Could not set socket to non-blocking: " << strerror(errno) << std::endl;
        exit(1);
    }

    struct sockaddr_in mcast_addr_struct;
    mcast_addr_struct.sin_family = AF_INET;
    mcast_addr_struct.sin_addr.s_addr = INADDR_ANY;
    mcast_addr_struct.sin_port = htons(port);
    
    if (bind(mcast_fd, (const sockaddr *)&mcast_addr_struct, sizeof(mcast_addr_struct)) < 0) {
        std::cerr << "Failed to bind to " << mcast_addr << ":" << port 
                  << " - " << strerror(errno) << std::endl;
        exit(1);
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
    mreq.imr_interface.s_addr = inet_addr(local_ip);
    
    if (setsockopt(mcast_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "Failed to join multicast group " << mcast_addr 
                  << " - " << strerror(errno) << std::endl;
        exit(1);
    }

    std::cout << "Joined multicast group " << mcast_addr << ":" << port << std::endl;
    return mcast_fd;
}