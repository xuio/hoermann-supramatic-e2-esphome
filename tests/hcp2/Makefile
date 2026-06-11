CC ?= cc
CXX ?= c++

CORE_DIR := ../../components/hcp2bridge/core
BUILD_DIR := build
CORE_SRCS := $(CORE_DIR)/hcp2_crc.c $(CORE_DIR)/hcp2_frame.c $(CORE_DIR)/hcp2_engine.c
TEST_SRCS := test_hcp2_core.c $(CORE_SRCS)
FUZZ_SRCS := fuzz_hcp2_parser.c $(CORE_SRCS)
HOST_SRCS := host_responder.c $(CORE_SRCS)
TEST_BIN := $(BUILD_DIR)/test_hcp2_core
FUZZ_BIN := $(BUILD_DIR)/fuzz_hcp2_parser
HOST_BIN := $(BUILD_DIR)/host_responder
FUZZ_ITERATIONS ?= 10000
FUZZ_REPORT ?= $(BUILD_DIR)/fuzz-report.json

CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -g
CPPFLAGS += -I$(CORE_DIR)
LDFLAGS ?=
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -g

.PHONY: all test fuzz host_responder asan cxx clean
.NOTPARALLEL:

all: test fuzz host_responder cxx

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_BIN): $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_SRCS) -o $@ $(LDFLAGS)

$(FUZZ_BIN): $(FUZZ_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUZZ_SRCS) -o $@ $(LDFLAGS)

$(HOST_BIN): $(HOST_SRCS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HOST_SRCS) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	$(TEST_BIN)

fuzz: $(FUZZ_BIN)
	$(FUZZ_BIN) $(FUZZ_ITERATIONS) $(FUZZ_REPORT)

host_responder: $(HOST_BIN)

cxx: | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $(CORE_DIR)/hcp2_crc.c -o $(BUILD_DIR)/hcp2_crc.cxx.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $(CORE_DIR)/hcp2_frame.c -o $(BUILD_DIR)/hcp2_frame.cxx.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $(CORE_DIR)/hcp2_engine.c -o $(BUILD_DIR)/hcp2_engine.cxx.o

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean test fuzz host_responder cxx

clean:
	rm -rf $(BUILD_DIR)
