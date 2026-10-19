# Native MIR product entry points. CMake owns every compile/link/install rule.
.DEFAULT_GOAL := all
# An outer make -j must not multiply the fixed two-job compiler limit.
.NOTPARALLEL:

SRC_DIR ?= $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR ?= build-make
PREFIX ?= /usr/local
DESTDIR ?=
CMAKE ?= cmake
CTEST ?= ctest
CONFIG ?= Release
TEST_JOBS ?= 8
ASAN_TEST_JOBS ?= 4

NATIVE_BINARY_DIR ?= $(BUILD_DIR)/release
DEBUG_BINARY_DIR ?= $(BUILD_DIR)/debug
ASAN_BINARY_DIR ?= $(BUILD_DIR)/asan
TEST_BINARY_DIR ?= $(BUILD_DIR)/tests
REFERENCE_BINARY_DIR ?= $(BUILD_DIR)/reference
BENCHMARK_BINARY_DIR ?= $(BUILD_DIR)/benchmarks

PRODUCT_OPTIONS = -DBUILD_TESTING=OFF -DMIRNEXT_BUILD_C_REFERENCE=OFF -DMIRNEXT_BUILD_TOOLS=OFF -DMIRNEXT_BUILD_EXAMPLES=OFF -DMIRNEXT_BUILD_UTILITIES=ON -DMIRNEXT_BUILD_BENCHMARKS=OFF
TEST_OPTIONS = -DBUILD_TESTING=ON -DMIRNEXT_BUILD_TOOLS=OFF -DMIRNEXT_BUILD_EXAMPLES=ON -DMIRNEXT_BUILD_UTILITIES=ON -DMIRNEXT_BUILD_BENCHMARKS=OFF

.PHONY: all debug debug2 asan install uninstall clean test test-all check reference-test native-test-build
.PHONY: native-commands native-corpus-test mir-bin-run-test bench native-benchmarks native-classic-benchmark native-format-benchmark legacy-c2mir

all:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(NATIVE_BINARY_DIR)" $(PRODUCT_OPTIONS) -DCMAKE_BUILD_TYPE="$(CONFIG)"
	$(CMAKE) --build "$(NATIVE_BINARY_DIR)" --config "$(CONFIG)" --parallel 2

debug:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(DEBUG_BINARY_DIR)" $(PRODUCT_OPTIONS) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build "$(DEBUG_BINARY_DIR)" --config Debug --parallel 2

debug2: asan
asan:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(ASAN_BINARY_DIR)" $(TEST_OPTIONS) -DMIRNEXT_BUILD_C_REFERENCE=OFF -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer"
	$(CMAKE) -E env ASAN_OPTIONS=detect_leaks=0 $(CMAKE) --build "$(ASAN_BINARY_DIR)" --config Debug --parallel 2
	$(CMAKE) -E env ASAN_OPTIONS=detect_leaks=0 $(CTEST) --test-dir "$(ASAN_BINARY_DIR)" -C Debug --output-on-failure --parallel $(ASAN_TEST_JOBS)

install: all
	$(CMAKE) "-DBINARY_DIR=$(abspath $(NATIVE_BINARY_DIR))" "-DPREFIX=$(PREFIX)" "-DSTAGING_ROOT=$(DESTDIR)" "-DCONFIG=$(CONFIG)" -P "$(SRC_DIR)/cmake/Install.cmake"

# Uses the last successful installation receipt, including its staging root.
# Uninstallation never configures or builds anything.
uninstall:
	$(CMAKE) "-DRECEIPT=$(abspath $(NATIVE_BINARY_DIR))/mirnext-install-receipt.txt" -P "$(SRC_DIR)/cmake/Uninstall.cmake"

clean:
	$(CMAKE) "-DBINARY_DIR=$(abspath $(NATIVE_BINARY_DIR))" "-DCONFIG=$(CONFIG)" -P "$(SRC_DIR)/cmake/Clean.cmake"

native-test-build:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(TEST_BINARY_DIR)" $(TEST_OPTIONS) -DMIRNEXT_BUILD_C_REFERENCE=OFF -DCMAKE_BUILD_TYPE="$(CONFIG)"
	$(CMAKE) --build "$(TEST_BINARY_DIR)" --config "$(CONFIG)" --parallel 2

test: native-test-build
	$(CTEST) --test-dir "$(TEST_BINARY_DIR)" -C "$(CONFIG)" --output-on-failure --parallel $(TEST_JOBS)
test-all check: test

reference-test:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(REFERENCE_BINARY_DIR)" $(TEST_OPTIONS) -DMIRNEXT_BUILD_C_REFERENCE=ON -DCMAKE_BUILD_TYPE="$(CONFIG)"
	$(CMAKE) --build "$(REFERENCE_BINARY_DIR)" --config "$(CONFIG)" --parallel 2
	$(CTEST) --test-dir "$(REFERENCE_BINARY_DIR)" -C "$(CONFIG)" --output-on-failure --parallel $(TEST_JOBS)

native-commands: all
native-corpus-test: native-test-build
	$(CTEST) --test-dir "$(TEST_BINARY_DIR)" -C "$(CONFIG)" -R "^mirnext-unified-linker" --output-on-failure --parallel $(TEST_JOBS)
mir-bin-run-test: native-test-build
	$(CTEST) --test-dir "$(TEST_BINARY_DIR)" -C "$(CONFIG)" -R "^mirnext-native-(commands|embedded)$$" --output-on-failure --parallel $(TEST_JOBS)

# Historical MIR correctness aliases now exercise the owning execution suites.
CORPUS_ALIASES = simplify-test io-test scan-test hello-test readme-example-test gen-test-loop gen-test-sieve gen-issue219-test gen-test-get-thunk-addr
CORPUS_ALIASES += $(addprefix interp-test,1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)
CORPUS_ALIASES += $(addprefix gen-test,1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)
.PHONY: $(CORPUS_ALIASES)
$(CORPUS_ALIASES): native-corpus-test

native-benchmarks:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(BENCHMARK_BINARY_DIR)" $(PRODUCT_OPTIONS) -DMIRNEXT_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build "$(BENCHMARK_BINARY_DIR)" --config Release --target mirnext_classic_benchmark mirnext_format_io_benchmark --parallel 2
native-classic-benchmark native-format-benchmark: native-benchmarks

bench: native-benchmarks
	$(CMAKE) "-DBINARY_DIR=$(abspath $(BENCHMARK_BINARY_DIR))" -P "$(SRC_DIR)/cmake/RunBenchmarks.cmake"

# Explicitly out of scope. No product/default/install/test target depends on it.
legacy-c2mir:
	$(CMAKE) -S "$(SRC_DIR)" -B "$(BUILD_DIR)/c2mir" -DBUILD_TESTING=OFF -DMIRNEXT_BUILD_C_REFERENCE=OFF -DMIRNEXT_BUILD_TOOLS=ON -DMIRNEXT_BUILD_EXAMPLES=OFF -DMIRNEXT_BUILD_UTILITIES=OFF -DCMAKE_BUILD_TYPE="$(CONFIG)"
	$(CMAKE) --build "$(BUILD_DIR)/c2mir" --config "$(CONFIG)" --target c2m --parallel 2
