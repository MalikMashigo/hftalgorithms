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
#include "messages.h"
#include "orderbook.h"

const char* msg_type_to_string(MSG_TYPE type);

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip);

int main(int argc, char *argv[])
{
    OrderBook book;

    //Network configuration for NDFEX exchange
    const char* live_addr = "239.0.0.1";
    const int live_port = 12345;
    const char* replay_addr = "239.0.0.2";
    const int replay_port = 12345;
    const char* local_ip = "192.168.13.16";

    //Create both multicast sockets
    std::vector<int> mcast_socks;
    mcast_socks.push_back(create_multicast_socket(live_addr, live_port, local_ip));
    mcast_socks.push_back(create_multicast_socket(replay_addr, replay_port, local_ip));


    //Create epoll instance to monitor both sockets
    int epoll_fd = epoll_create(128);
    if (epoll_fd < 0) {
        std::cerr << "Failed to create epoll: " << strerror(errno) << std::endl;
        return 1;
    }

    //Register each multicast socket with epoll
    for (int sock : mcast_socks) {
        epoll_event event;
        event.data.fd = sock;
        event.events = EPOLLIN;
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &event) < 0) {
            std::cerr << "Failed to add socket to epoll: " << strerror(errno) << std::endl;
            return 1;
        }
    }

    std::cout << "Starting to receive messages..." << std::endl;

    char buf[1500];
    
    while (true) {
        epoll_event events[16]; 
        
        //Wait for events on any of the registered sockets
        // -1 means block indefinitely until data arrives
        int nfds = epoll_wait(epoll_fd, events, 16, -1);
        
        if (nfds < 0) {
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
            return 1;
        }

        //Process each socket that has data ready
        for (int i = 0; i < nfds; ++i) {
            //Read data from the socket
            ssize_t bytes = read(events[i].data.fd, buf, sizeof(buf));
            
            //Handle read errors (except EAGAIN which means no data available)
            if (bytes < 0 && errno != EAGAIN) {
                std::cerr << "Error on read: " << strerror(errno) << std::endl;
                return 1;
            }

            //Process the message if we received data
            if (bytes > 0) {
                //Cast buffer to message header to check type
                md_header* header = reinterpret_cast<md_header*>(buf);
                
                //Check magic number to determine message source
                if (header->magic_number == MAGIC_NUMBER) {
                    switch (header->msg_type) {
                        case MSG_TYPE::NEW_ORDER:
                            book.handle_new_order(reinterpret_cast<const new_order*>(buf));
                            break;
                        case MSG_TYPE::DELETE_ORDER:
                            book.handle_delete_order(reinterpret_cast<const delete_order*>(buf));
                            break;
                        case MSG_TYPE::MODIFY_ORDER:
                            book.handle_modify_order(reinterpret_cast<const modify_order*>(buf));
                            break;
                        case MSG_TYPE::TRADE:
                            book.handle_trade(reinterpret_cast<const trade*>(buf));
                            break;
                        case MSG_TYPE::HEARTBEAT:
                            break;
                        case MSG_TYPE::TRADE_SUMMARY:
        
                            break;
                        case MSG_TYPE::SNAPSHOT_INFO:
                            break;
                }

                // After each message, can check best bid/ask
                std::cout << "Best Bid: " << book.get_best_bid_price() 
                  << " @ " << book.get_best_bid_qty() << std::endl;
    }
            }
        }
    }

    //Cleanup
    close(epoll_fd);
    for (int sock : mcast_socks) {
        close(sock);
    }
    
    return 0;
}

const char* msg_type_to_string(MSG_TYPE type) {
    switch(type) {
        case MSG_TYPE::HEARTBEAT: return "HEARTBEAT";
        case MSG_TYPE::NEW_ORDER: return "NEW_ORDER";
        case MSG_TYPE::DELETE_ORDER: return "DELETE_ORDER";
        case MSG_TYPE::MODIFY_ORDER: return "MODIFY_ORDER";
        case MSG_TYPE::TRADE: return "TRADE";
        case MSG_TYPE::TRADE_SUMMARY: return "TRADE_SUMMARY";
        case MSG_TYPE::SNAPSHOT_INFO: return "SNAPSHOT_INFO";
        default: return "UNKNOWN";
    }
}

int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip) {
    //Create UDP socket
    int mcast_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (mcast_fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        exit(1);
    }

    int reuse = 1;
    setsockopt(mcast_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(mcast_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    //Make socket non-blocking so we can use epoll efficiently
    int flags = fcntl(mcast_fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "Could not get flags: " << strerror(errno) << std::endl;
        exit(1);
    }
    if (fcntl(mcast_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "Could not set socket to non-blocking: " << strerror(errno) << std::endl;
        exit(1);
    }

    //Bind to the multicast address and port
    struct sockaddr_in mcast_addr_struct;
    mcast_addr_struct.sin_family = AF_INET;
    mcast_addr_struct.sin_addr.s_addr = inet_addr(mcast_addr);
    mcast_addr_struct.sin_port = htons(port);
    
    if (bind(mcast_fd, (const sockaddr *)&mcast_addr_struct, sizeof(mcast_addr_struct)) < 0) {
        std::cerr << "Failed to bind to " << mcast_addr << ":" << port 
                  << " - " << strerror(errno) << std::endl;
        exit(1);
    }

    //Join the multicast group on the specified network interface
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
