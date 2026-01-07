CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2
TARGET = tracker
SRC = tracker.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
