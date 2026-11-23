# BruceLink host test build. SPDX-License-Identifier: MIT
# The shared core is compiled with strict flags (-Wconversion etc).
# Test translation units are compiled strict MINUS -Wconversion, since
# the tests intentionally pass int return codes into size_t parameters.
# Both are built and linked under AddressSanitizer + UBSan by default.

CORE_DIR := ../../shared/brucelink_core/src
CC ?= cc
CSTD ?= -std=c99
WARN_CORE := -Wall -Wextra -Werror -Wshadow -Wconversion -Wpointer-arith -Wvla
WARN_TEST := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wvla
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
OPT ?= -O1 -g
INC := -I$(CORE_DIR) -I.

CORE_SRC := $(wildcard $(CORE_DIR)/*.c)
TEST_SRC := $(wildcard *.c)
CORE_OBJ := $(patsubst $(CORE_DIR)/%.c,obj/core_%.o,$(CORE_SRC))
TEST_OBJ := $(patsubst %.c,obj/test_%.o,$(TEST_SRC))
HDRS := $(wildcard $(CORE_DIR)/*.h) $(wildcard *.h)

BIN := bl_tests

.PHONY: all test clean cppcheck
all: test

obj:
	@mkdir -p obj

obj/core_%.o: $(CORE_DIR)/%.c $(HDRS) | obj
	$(CC) $(CSTD) $(WARN_CORE) $(OPT) $(SAN) $(INC) -c $< -o $@

obj/test_%.o: %.c $(HDRS) | obj
	$(CC) $(CSTD) $(WARN_TEST) $(OPT) $(SAN) $(INC) -c $< -o $@

$(BIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(SAN) $(OPT) $(CORE_OBJ) $(TEST_OBJ) -o $(BIN)

test: $(BIN)
	./$(BIN)

# Static analysis over the core (no sanitizers); fails on findings.
cppcheck:
	cppcheck --std=c99 --enable=warning,style,performance,portability \
	  --error-exitcode=1 --inline-suppr --quiet \
	  --suppress=missingIncludeSystem --suppress=unusedFunction $(CORE_DIR)

clean:
	rm -rf obj $(BIN)
