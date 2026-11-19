CXXFLAGS ?= -O2 -Wall -Wextra -pedantic
CXXFLAGS += -std=c++17
SRC = main.cpp snap.cpp config.cpp nokill.cpp watchgroups.cpp daemon.cpp agent.cpp client.cpp

btrfs-snap-cpp: $(SRC) $(wildcard *.hpp)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ -lmount -lsystemd -pthread
