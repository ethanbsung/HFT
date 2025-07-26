#!/bin/bash

# Load environment variables from .env file if it exists
if [ -f "../.env" ]; then
    echo "Loading environment variables from .env file..."
    # Use a safer method to load multi-line environment variables
    set -a  # automatically export all variables
    source ../.env
    set +a  # turn off automatic export
    echo "Loaded environment variables:"
    echo "  COINBASE_API_KEY: ${COINBASE_API_KEY:0:10}..."
    echo "  COINBASE_API_SECRET: ${COINBASE_API_SECRET:+[PRESENT]}${COINBASE_API_SECRET:-[MISSING]}"
    echo "  COINBASE_API_KEY length: ${#COINBASE_API_KEY}"
    echo "  COINBASE_API_SECRET length: ${#COINBASE_API_SECRET}"
    echo "  All environment variables:"
    env | grep COINBASE
    
    # Export environment variables for child processes
    export COINBASE_API_KEY
    export COINBASE_API_SECRET
    export COINBASE_PRODUCT_ID
    export COINBASE_WEBSOCKET_URL
else
    echo "No .env file found at ../.env"
    echo "Make sure you have COINBASE_API_KEY and COINBASE_API_SECRET set"
fi

# Compile and run tests
echo "Compiling tests..."
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -O0 -g \
    tests/test_data_feed.cpp \
    src/market_data_feed.cpp \
    src/order_manager.cpp \
    src/orderbook_engine.cpp \
    src/memory_pool.cpp \
    src/latency_tracker.cpp \
    -lgtest -lgtest_main -pthread -lssl -lcrypto \
    -o test_data_feed

if [ $? -eq 0 ]; then
    echo "Compilation successful. Running tests..."
    ./test_data_feed
else
    echo "Compilation failed!"
    exit 1
fi 