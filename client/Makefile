CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2
LDFLAGS = -lssl -lcrypto
TARGET = client
SRC = client.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
