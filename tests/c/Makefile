# BruceLink host test build. SPDX-License-Identifier: MIT
# The shared core is compiled with strict flags (-Wconversion etc).
# Test translation units are compiled strict MINUS -Wconversion, since
# the tests intentionally pass int return codes into size_t parameters.
# The ESP32 bridge core (portable C++) is compiled with the host C++
# compiler and tested by bl_bridge_tests. Everything is built and linked
# under AddressSanitizer + UBSan by default.

CORE_DIR := ../../shared/brucelink_core/src
BRIDGE_DIR := ../../esp32/lib/BruceLink
CC ?= cc
CXX ?= c++
CSTD ?= -std=c99
CXXSTD ?= -std=c++17
WARN_CORE := -Wall -Wextra -Werror -Wshadow -Wconversion -Wpointer-arith -Wvla
WARN_TEST := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wvla
WARN_CXX := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wnon-virtual-dtor
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
OPT ?= -O1 -g
INC := -I$(CORE_DIR) -I.

CORE_SRC := $(wildcard $(CORE_DIR)/*.c)
TEST_SRC := $(wildcard *.c)
CORE_OBJ := $(patsubst $(CORE_DIR)/%.c,obj/core_%.o,$(CORE_SRC))
TEST_OBJ := $(patsubst %.c,obj/test_%.o,$(TEST_SRC))
HDRS := $(wildcard $(CORE_DIR)/*.h) $(wildcard *.h)

# Bridge core only: NimBleCentralTransport needs NimBLE and is built by
# esp32/test_compile instead.
BRIDGE_SRC := $(BRIDGE_DIR)/BruceLinkBridge.cpp $(BRIDGE_DIR)/BruceLinkFormat.cpp \
              $(BRIDGE_DIR)/BruceLinkPeerGuard.cpp
BRIDGE_OBJ := $(patsubst $(BRIDGE_DIR)/%.cpp,obj/bridge_%.o,$(BRIDGE_SRC))
BRIDGE_HDRS := $(BRIDGE_DIR)/BruceLinkBridge.h $(BRIDGE_DIR)/BruceLinkFormat.h \
               $(BRIDGE_DIR)/BruceLinkPeerGuard.h

BIN := bl_tests
BRIDGE_BIN := bl_bridge_tests

.PHONY: all test clean cppcheck
all: test

obj:
	@mkdir -p obj

obj/core_%.o: $(CORE_DIR)/%.c $(HDRS) | obj
	$(CC) $(CSTD) $(WARN_CORE) $(OPT) $(SAN) $(INC) -c $< -o $@

obj/test_%.o: %.c $(HDRS) | obj
	$(CC) $(CSTD) $(WARN_TEST) $(OPT) $(SAN) $(INC) -c $< -o $@

obj/bridge_%.o: $(BRIDGE_DIR)/%.cpp $(HDRS) $(BRIDGE_HDRS) | obj
	$(CXX) $(CXXSTD) $(WARN_CXX) $(OPT) $(SAN) $(INC) -I$(BRIDGE_DIR) -c $< -o $@

obj/cxxtest_bridge.o: test_bridge.cpp $(HDRS) $(BRIDGE_HDRS) | obj
	$(CXX) $(CXXSTD) $(WARN_CXX) $(OPT) $(SAN) $(INC) -I$(BRIDGE_DIR) -c $< -o $@

$(BIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(SAN) $(OPT) $(CORE_OBJ) $(TEST_OBJ) -o $(BIN)

$(BRIDGE_BIN): $(CORE_OBJ) $(BRIDGE_OBJ) obj/cxxtest_bridge.o
	$(CXX) $(SAN) $(OPT) $^ -o $(BRIDGE_BIN)

test: $(BIN) $(BRIDGE_BIN)
	./$(BIN)
	./$(BRIDGE_BIN)

# Static analysis over the core (C) and the portable bridge core (C++);
# fails on findings.
cppcheck:
	cppcheck --std=c99 --enable=warning,style,performance,portability \
	  --error-exitcode=1 --inline-suppr --quiet \
	  --suppress=missingIncludeSystem --suppress=unusedFunction $(CORE_DIR)
	cppcheck --language=c++ --std=c++17 --enable=warning,performance,portability \
	  --error-exitcode=1 --inline-suppr --quiet \
	  --suppress=missingIncludeSystem -I$(CORE_DIR) $(BRIDGE_SRC)

clean:
	rm -rf obj $(BIN) $(BRIDGE_BIN)
