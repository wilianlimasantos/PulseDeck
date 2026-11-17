# Host tests for firmware-core (no board needed).  make test
CXX ?= g++
CORE_FLAGS := -std=c++17 -Wall -Wextra -fno-exceptions -fno-rtti -Iinclude
CXXFLAGS ?= $(CORE_FLAGS) -O1 -g -fsanitize=address,undefined
SRC := $(wildcard src/*.cpp)

build/test_core: $(SRC) tests/test_core.cpp tests/mem_store.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) tests/test_core.cpp -o $@

nostl:
	@! grep -nE '#include <(string|vector|map|deque|memory|iostream|algorithm|functional)>|std::' src/*.cpp include/pd/*.h || (echo 'STL in firmware-core'; exit 1)
	@echo 'firmware-core: no STL, no std::'

test: nostl build/test_core
	python3 ../tools/check_catalog.py
	./build/test_core

clean:
	rm -rf build
.PHONY: test clean nostl
