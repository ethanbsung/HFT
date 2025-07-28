# HFT System Makefile
# High-performance C++ build configuration

# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic

# Performance optimizations for HFT
RELEASE_FLAGS = -O3 -DNDEBUG -march=native -mtune=native -ffast-math -funroll-loops -flto
RELEASE_FLAGS += -fno-rtti  # Disable RTTI for performance (keeping exceptions for WebSocket)

# Libraries to link for WebSocket support
WEBSOCKET_LIBS = -lboost_system -lssl -lcrypto

# Debug flags
DEBUG_FLAGS = -g -O0 -DDEBUG -fsanitize=address

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = bin
LIBDIR = lib

# Source files
SOURCES = $(wildcard $(SRCDIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
HEADERS = $(wildcard $(INCDIR)/*.hpp)

# Library and executable names
LIBRARY = $(LIBDIR)/libhft_core.a
TEST_EXEC = $(BINDIR)/hft_test
BENCHMARK_EXEC = $(BINDIR)/hft_benchmark

# Include paths
INCLUDES = -I$(INCDIR)

# Libraries to link
LIBS = -pthread $(WEBSOCKET_LIBS) -lsodium

# Test configuration
TESTDIR = tests
TEST_SOURCES = $(wildcard $(TESTDIR)/*.cpp)
TEST_OBJECTS = $(TEST_SOURCES:$(TESTDIR)/%.cpp=$(OBJDIR)/test_%.o)
GTEST_LIBS = -lgtest -lgtest_main

# Default target
.PHONY: all clean debug release test benchmark install

all: release

# Release build (optimized for performance)
release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(LIBRARY)

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: $(LIBRARY)

# Create directories
$(OBJDIR):
	mkdir -p $(OBJDIR)

$(LIBDIR):
	mkdir -p $(LIBDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# Compile source files
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(HEADERS) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Create static library
$(LIBRARY): $(OBJECTS) | $(LIBDIR)
	ar rcs $@ $^
	ranlib $@

# Test objects compilation
$(OBJDIR)/test_%.o: $(TESTDIR)/test_%.cpp $(HEADERS) | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Test executables
test: test_data_feed test_orderbook test_latency test_signal_engine

# Data feed tests
test_data_feed: $(BINDIR)/test_data_feed
$(BINDIR)/test_data_feed: $(OBJDIR)/test_data_feed.o $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) $(GTEST_LIBS) -o $@

# OrderBook tests
test_orderbook: $(BINDIR)/test_orderbook_engine
$(BINDIR)/test_orderbook_engine: $(OBJDIR)/test_orderbook_engine.o $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) $(GTEST_LIBS) -o $@

# Latency tracker tests
test_latency: $(BINDIR)/test_latency_tracker
$(BINDIR)/test_latency_tracker: $(OBJDIR)/test_latency_tracker.o $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) $(GTEST_LIBS) -o $@

# Signal engine tests
test_signal_engine: $(BINDIR)/test_signal_engine
$(BINDIR)/test_signal_engine: $(OBJDIR)/test_signal_engine.o $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) $(GTEST_LIBS) -o $@

# Benchmark executable
benchmark: $(BENCHMARK_EXEC)

$(BENCHMARK_EXEC): benchmark/benchmark_main.cpp $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) -o $@

# Install (simple local install)
install: release
	mkdir -p /usr/local/include/hft
	mkdir -p /usr/local/lib
	cp -r $(INCDIR)/* /usr/local/include/hft/
	cp $(LIBRARY) /usr/local/lib/

# Clean build artifacts
clean:
	rm -rf $(OBJDIR) $(LIBDIR) $(BINDIR)

# Performance profiling build
profile: CXXFLAGS += $(RELEASE_FLAGS) -pg
profile: $(LIBRARY)

# Memory debugging build
memcheck: CXXFLAGS += $(DEBUG_FLAGS) -fsanitize=leak
memcheck: $(LIBRARY)

# Static analysis
analyze:
	clang-tidy $(SOURCES) -- $(INCLUDES)

# Code formatting
format:
	clang-format -i $(SOURCES) $(HEADERS)

# Show build information
info:
	@echo "HFT System Build Configuration"
	@echo "=============================="
	@echo "Compiler: $(CXX)"
	@echo "C++ Standard: C++17"
	@echo "Include Dir: $(INCDIR)"
	@echo "Source Dir: $(SRCDIR)"
	@echo "Object Dir: $(OBJDIR)"
	@echo "Library Dir: $(LIBDIR)"
	@echo "Binary Dir: $(BINDIR)"
	@echo ""
	@echo "Available targets:"
	@echo "  all       - Build release version (default)"
	@echo "  release   - Build optimized version"
	@echo "  debug     - Build debug version"
	@echo "  test      - Build test executable"
	@echo "  benchmark - Build benchmark executable"
	@echo "  profile   - Build with profiling support"
	@echo "  memcheck  - Build with memory debugging"
	@echo "  clean     - Remove build artifacts"
	@echo "  format    - Format source code"
	@echo "  analyze   - Run static analysis"
	@echo "  install   - Install to system"

# Dependencies
-include $(OBJECTS:.o=.d)

# Generate dependency files
$(OBJDIR)/%.d: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MM -MT $(@:.d=.o) $< > $@

# Build statistics
stats: $(OBJECTS)
	@echo "Build Statistics:"
	@echo "================"
	@echo "Source files: $(words $(SOURCES))"
	@echo "Object files: $(words $(OBJECTS))"
	@echo "Header files: $(words $(HEADERS))"
	@wc -l $(SOURCES) $(HEADERS)

# Quick test runner (when test files exist)
check: test
	@echo "Running HFT system tests..."
	@echo "Running market data feed tests..."
	@if [ -f $(BINDIR)/test_data_feed ]; then \
		$(BINDIR)/test_data_feed; \
	else \
		echo "Data feed test executable not found."; \
	fi
	@echo "Running orderbook engine tests..."
	@if [ -f $(BINDIR)/test_orderbook_engine ]; then \
		$(BINDIR)/test_orderbook_engine; \
	else \
		echo "OrderBook test executable not found."; \
	fi
	@echo "Running latency tracker tests..."
	@if [ -f $(BINDIR)/test_latency_tracker ]; then \
		$(BINDIR)/test_latency_tracker; \
	else \
		echo "Latency tracker test executable not found."; \
	fi
	@echo "Running signal engine tests..."
	@if [ -f $(BINDIR)/test_signal_engine ]; then \
		$(BINDIR)/test_signal_engine; \
	else \
		echo "Signal engine test executable not found."; \
	fi

# Benchmark runner
bench: benchmark
	@echo "Running HFT system benchmarks..."
	@if [ -f $(BENCHMARK_EXEC) ]; then \
		$(BENCHMARK_EXEC); \
	else \
		echo "Benchmark executable not found. Run 'make benchmark' first."; \
	fi

# Development convenience targets
dev: debug test
	@echo "Development build complete"

prod: release
	@echo "Production build complete"

# Help target
help: info
