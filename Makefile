
CXX = g++
CFLAGS = -g -Wall -std=c++11 -O1 -rdynamic
SHARED = -fPIC --shared
INCLUDE_DIR ?= skynet/skynet-src
TARGET = mprof.so

all: $(TARGET)

$(TARGET): service_mprof.cpp
	$(CXX) $(CFLAGS) -I$(INCLUDE_DIR) $(SHARED)  $^ -o $@

clean:
	rm -f $(TARGET)