CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

RISK_SRCS = position_tracker.cpp exposure_tracker.cpp \
            pnl_tracker.cpp risk_manager.cpp

all: listener oe_client tests

listener: listener.cpp orderbook.cpp orderbook.h messages.h
	$(CXX) $(CXXFLAGS) -o listener listener.cpp orderbook.cpp

oe_client: oe_client.cpp oe_messages.h oe_client.h iorder_sender.h
	$(CXX) $(CXXFLAGS) -o oe_client oe_client.cpp

tests: test_risk.cpp $(RISK_SRCS)
	$(CXX) $(CXXFLAGS) -o tests test_risk.cpp $(RISK_SRCS)

run_tests: tests
	./tests

clean:
	rm -f listener oe_client tests bbo_data.csv oe_log.txt risk_log.txt risk_demo_log.txt

run_listener: listener
	./listener

run_oe: oe_client
	./oe_client

.PHONY: all clean run_listener run_oe run_tests
