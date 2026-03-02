CXX = g++
CXXFLAGS = -std=c++11 -O3 -Wall -Wextra -Wno-unused-parameter

all: listener oe_client

listener: listener.cpp orderbook.cpp orderbook.h messages.h
	$(CXX) $(CXXFLAGS) -o listener listener.cpp orderbook.cpp

oe_client: oe_client.cpp oe_messages.h oe_client.h
	$(CXX) $(CXXFLAGS) -o oe_client oe_client.cpp

clean:
	rm -f listener oe_client bbo_data.csv oe_log.txt

run_listener: listener
	./listener

run_oe: oe_client
	./oe_client

.PHONY: all clean run_listener run_oe