# HFT System Makefile
# High-performance C++ build configuration

# Compiler settings
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic

# Performance optimizations for HFT
RELEASE_FLAGS = -O3 -DNDEBUG -march=native -mtune=native -ffast-math -funroll-loops -flto
RELEASE_FLAGS += -fno-exceptions -fno-rtti  # Disable for maximum performance

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
LIBS = -pthread -lssl -lcrypto

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

# Test executable
test: $(TEST_EXEC)

$(TEST_EXEC): test/test_main.cpp $(LIBRARY) | $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -L$(LIBDIR) -lhft_core $(LIBS) -o $@

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
	@if [ -f $(TEST_EXEC) ]; then \
		$(TEST_EXEC); \
	else \
		echo "Test executable not found. Run 'make test' first."; \
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
