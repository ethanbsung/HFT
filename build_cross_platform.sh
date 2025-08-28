#!/bin/bash

# Cross-Platform HFT System Build Script
# Supports Linux and macOS with architecture detection

set -e  # Exit on any error

echo "🚀 HFT System Cross-Platform Build"
echo "=================================="

# Detect platform and architecture
OS=$(uname -s)
ARCH=$(uname -m)
COMPILER=""

echo "📋 Platform Detection:"
echo "  OS: $OS"
echo "  Architecture: $ARCH"
echo ""

# Set compiler based on platform
case $OS in
    "Darwin")
        echo "🍎 macOS detected"
        # Check for available compilers
        if command -v clang++ &> /dev/null; then
            COMPILER="clang++"
            echo "  Using Clang++ compiler"
        elif command -v g++ &> /dev/null; then
            COMPILER="g++"
            echo "  Using GCC compiler"
        else
            echo "❌ No suitable C++ compiler found"
            echo "   Install Xcode Command Line Tools: xcode-select --install"
            exit 1
        fi
        ;;
    "Linux")
        echo "🐧 Linux detected"
        if command -v g++ &> /dev/null; then
            COMPILER="g++"
            echo "  Using GCC compiler"
        elif command -v clang++ &> /dev/null; then
            COMPILER="clang++"
            echo "  Using Clang++ compiler"
        else
            echo "❌ No suitable C++ compiler found"
            echo "   Install GCC: sudo apt-get install build-essential"
            exit 1
        fi
        ;;
    *)
        echo "❌ Unsupported operating system: $OS"
        exit 1
        ;;
esac

# Check architecture compatibility
case $ARCH in
    "x86_64"|"amd64")
        echo "  Architecture: x86_64 (Intel/AMD)"
        echo "  ✅ Full optimization support"
        ;;
    "aarch64"|"arm64")
        echo "  Architecture: ARM64 (Apple Silicon/ARM)"
        echo "  ✅ Full optimization support"
        ;;
    *)
        echo "  Architecture: $ARCH"
        echo "  ⚠️  Limited optimization support"
        ;;
esac

echo ""
echo "🔧 Build Configuration:"
echo "  Compiler: $COMPILER"
echo "  Build Type: Release (optimized)"
echo "  C++ Standard: C++17"
echo ""

# Check dependencies
echo "📦 Checking Dependencies..."

# Check for required libraries
MISSING_DEPS=()

# Check for OpenSSL
if ! pkg-config --exists openssl 2>/dev/null; then
    case $OS in
        "Darwin")
            echo "  ⚠️  OpenSSL not found - install with: brew install openssl"
            ;;
        "Linux")
            echo "  ⚠️  OpenSSL not found - install with: sudo apt-get install libssl-dev"
            ;;
    esac
    MISSING_DEPS+=("openssl")
fi

# Check for Google Test
if ! pkg-config --exists gtest 2>/dev/null; then
    case $OS in
        "Darwin")
            echo "  ⚠️  Google Test not found - install with: brew install googletest"
            ;;
        "Linux")
            echo "  ⚠️  Google Test not found - install with: sudo apt-get install libgtest-dev"
            ;;
    esac
    MISSING_DEPS+=("gtest")
fi

# Check for Boost (optional)
if ! pkg-config --exists boost_system 2>/dev/null; then
    case $OS in
        "Darwin")
            echo "  ℹ️  Boost not found - install with: brew install boost"
            ;;
        "Linux")
            echo "  ℹ️  Boost not found - install with: sudo apt-get install libboost-all-dev"
            ;;
    esac
    echo "  ℹ️  Boost is optional - WebSocket features may be limited"
fi

echo ""

# Build the system
echo "🔨 Building HFT System..."

cd cpp

# Use CMake for cross-platform compatibility
if command -v cmake &> /dev/null; then
    echo "  Using CMake build system"
    
    # Create build directory
    mkdir -p build
    cd build
    
    # Configure with CMake
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=$COMPILER
    
    # Build
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    
    echo ""
    echo "✅ CMake build completed successfully!"
    
else
    echo "  Using Makefile build system"
    
    # Set compiler for Makefile
    export CXX=$COMPILER
    
    # Build with Makefile
    make clean
    make release
    
    echo ""
    echo "✅ Makefile build completed successfully!"
fi

cd ..

echo ""
echo "🎯 Build Summary:"
echo "  Platform: $OS ($ARCH)"
echo "  Compiler: $COMPILER"
echo "  Build System: $(if command -v cmake &> /dev/null; then echo "CMake"; else echo "Makefile"; fi)"
echo ""

# Check if executables were created
if [ -f "cpp/bin/hft_system" ] || [ -f "cpp/build/bin/hft_system" ]; then
    echo "✅ HFT system executable created successfully!"
    echo ""
    echo "🚀 Ready to run:"
    echo "  ./run_hft_system.sh"
    echo ""
else
    echo "⚠️  HFT system executable not found"
    echo "   Check build output for errors"
fi

echo "📊 Performance Note:"
echo "  - Build optimized for $ARCH architecture"
echo "  - Performance may vary between different ISAs"
echo "  - For consistent results, use same architecture on both platforms"
echo ""
