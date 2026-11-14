# Builds the library, the command-line tool, the benchmark and the tests into build/.
#
#   make                  library, CLI and benchmark (optimized)
#   make test             unit and round-trip tests (plus the Apple interoperability tests on macOS)
#   make test-sanitize    the tests under AddressSanitizer and UndefinedBehaviorSanitizer
#   make test-alloc       allocation-count tests
#   make fuzz             fuzz targets (needs clang with libFuzzer), run with FUZZ_SECONDS each
#   make format-check     check formatting with clang-format
#
# Set CPU_FLAGS to tune for a chip, for example: make CPU_FLAGS=-mcpu=apple-m1
# Set DEFINES=-DLZFSE_ZAZZCAT_NO_THREADS to build without pthreads (chunked encoding then runs on one thread).

CC ?= cc
BUILD := build
CPU_FLAGS ?=
DEFINES ?=
# C11 plus POSIX: under -std=c11, glibc hides POSIX functions and types (lstat, mode_t, popen, clock_gettime)
# unless _DEFAULT_SOURCE is defined before any system header. macOS shows them anyway and ignores the macro.
STANDARD := -std=c11 -D_DEFAULT_SOURCE
WARNINGS := $(STANDARD) -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
CFLAGS ?= -O3
ALL_CFLAGS := $(WARNINGS) $(CFLAGS) $(CPU_FLAGS) $(DEFINES) -Iinclude -Isrc -pthread
LDFLAGS ?=
LIBS := -pthread
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
APPLE_LIBS := -lcompression
endif

LIB_SOURCES := $(wildcard src/*.c)
LIB_HEADERS := $(wildcard src/*.h) include/lzfse_zazzcat.h
LIB_OBJECTS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SOURCES))
LIBRARY := $(BUILD)/liblzfse_zazzcat.a

all: $(LIBRARY) $(BUILD)/lzfse-zazzcat $(BUILD)/lzfse-bench

$(BUILD)/obj/%.o: src/%.c $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(LIBRARY): $(LIB_OBJECTS)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/lzfse-zazzcat: tools/cli.c tools/file_io.h $(LIBRARY)
	$(CC) $(ALL_CFLAGS) -Itools -o $@ tools/cli.c $(LIBRARY) $(LDFLAGS) $(LIBS)

$(BUILD)/lzfse-bench: bench/bench.c tools/file_io.h $(LIBRARY)
	$(CC) $(ALL_CFLAGS) -Itools -o $@ bench/bench.c $(LIBRARY) $(LDFLAGS) $(LIBS) $(APPLE_LIBS)

# Tests compile the library sources directly (with assertions and debug info) so they can reach internal functions.
TEST_FLAGS := $(WARNINGS) $(DEFINES) -O2 -g -Iinclude -Isrc -Itools -Itests -pthread

$(BUILD)/unit-tests: tests/*.c tests/*.h $(LIB_SOURCES) $(LIB_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(TEST_FLAGS) $(TEST_EXTRA) -o $@ tests/unit_tests.c $(LIB_SOURCES) $(LIBS) $(APPLE_LIBS)

test: $(BUILD)/unit-tests
	$(BUILD)/unit-tests

test-sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize TEST_EXTRA="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" test

$(BUILD)/alloc-tests: tests/alloc_tests.c tests/test_support.h $(LIB_SOURCES) $(LIB_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(TEST_FLAGS) -DLZFSE_ZAZZCAT_COUNT_ALLOCATIONS -o $@ tests/alloc_tests.c $(LIB_SOURCES) $(LIBS)

test-alloc: $(BUILD)/alloc-tests
	$(BUILD)/alloc-tests

FUZZ_TARGETS := decode round_trip lzvn_decode decmpfs
FUZZ_SECONDS ?= 60
FUZZ_FLAGS := $(STANDARD) -O1 -g -Iinclude -Isrc -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all

fuzz: $(patsubst %,$(BUILD)/fuzz/%,$(FUZZ_TARGETS))
	@for target in $(FUZZ_TARGETS); do \
		mkdir -p $(BUILD)/fuzz/corpus-$$target; \
		$(BUILD)/fuzz/$$target -max_total_time=$(FUZZ_SECONDS) -max_len=70000 $(BUILD)/fuzz/corpus-$$target || exit 1; \
	done

$(BUILD)/fuzz/%: fuzz/%.c $(LIB_SOURCES) $(LIB_HEADERS)
	@mkdir -p $(dir $@)
	clang $(FUZZ_FLAGS) -o $@ $< $(LIB_SOURCES) -pthread

FORMATTED := src/*.c src/*.h include/*.h tools/*.c tools/*.h bench/*.c tests/*.c tests/*.h fuzz/*.c

format-check:
	clang-format --dry-run --Werror $(FORMATTED)

format:
	clang-format -i $(FORMATTED)

clean:
	rm -rf $(BUILD)

.PHONY: all test test-sanitize test-alloc fuzz format format-check clean
