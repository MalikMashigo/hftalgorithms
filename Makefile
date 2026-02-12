CXX = g++
CXXFLAGS = -std=c++11 -O3 -Wall -Wextra -Wno-unused-parameter
TARGET = listener
SOURCES = listener.cpp orderbook.cpp
HEADERS = orderbook.h messages.h

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET) bbo_data.csv

run: $(TARGET)
	./$(TARGET)

.PHONY: clean run