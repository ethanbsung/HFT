#pragma once

#include "types.hpp"
#include "orderbook_engine.hpp"
#include "latency_tracker.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace hft {

/**
 * Market data connection states
 */
enum class ConnectionState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    SUBSCRIBED = 3,
    ERROR = 4,
    RECONNECTING = 5
};

/**
 * Coinbase message types
 */
enum class CoinbaseMessageType : uint8_t {
    HEARTBEAT = 0,
    TICKER = 1,
    SNAPSHOT = 2,
    L2UPDATE = 3,
    MATCH = 4,
    SUBSCRIPTIONS = 5,
    ERROR_MSG = 6,
    UNKNOWN = 7
};

/**
 * Coinbase trade message
 */
struct CoinbaseTradeMessage {
    std::string trade_id;
    std::string maker_order_id;
    std::string taker_order_id;
    std::string side;           // "buy" or "sell"
    std::string size;
    std::string price;
    std::string product_id;
    std::string sequence;
    std::string time;
    
    // Parsed values
    price_t parsed_price;
    quantity_t parsed_size;
    Side parsed_side;
    timestamp_t parsed_time;
};

/**
 * Coinbase order book update message
 */
struct CoinbaseBookMessage {
    std::string type;           // "snapshot" or "l2update"
    std::string product_id;
    std::vector<std::vector<std::string>> changes;  // [side, price, size]
    std::string time;
    
    // Parsed values
    timestamp_t parsed_time;
    std::vector<std::tuple<Side, price_t, quantity_t>> parsed_changes;
};

/**
 * Market data statistics
 */
struct MarketDataStats {
    uint64_t messages_received;
    uint64_t messages_processed;
    uint64_t messages_dropped;
    uint64_t reconnection_count;
    uint64_t trades_processed;
    uint64_t book_updates_processed;
    double avg_latency_us;
    timestamp_t last_message_time;
    timestamp_t connection_start_time;
    
    MarketDataStats() : messages_received(0), messages_processed(0), 
                       messages_dropped(0), reconnection_count(0),
                       trades_processed(0), book_updates_processed(0),
                       avg_latency_us(0.0), last_message_time(now()),
                       connection_start_time(now()) {}
};

/**
 * Configuration for market data feed
 */
struct MarketDataConfig {
    std::string coinbase_api_key;
    std::string coinbase_api_secret;
    std::string websocket_url = "wss://ws-feed.exchange.coinbase.com";
    std::string product_id = "BTC-USD";
    
    // Subscription options
    bool subscribe_to_heartbeat = true;
    bool subscribe_to_ticker = false;
    bool subscribe_to_level2 = true;
    bool subscribe_to_matches = true;
    
    // Performance settings
    uint32_t message_queue_size = 10000;
    uint32_t reconnect_delay_ms = 5000;
    uint32_t heartbeat_timeout_ms = 30000;
    bool enable_message_compression = true;
    
    MarketDataConfig() = default;
};

/**
 * Callback function types for market data events
 */
using ConnectionStateCallback = std::function<void(ConnectionState, const std::string&)>;
using TradeMessageCallback = std::function<void(const CoinbaseTradeMessage&)>;
using BookMessageCallback = std::function<void(const CoinbaseBookMessage&)>;
using ErrorCallback = std::function<void(const std::string&)>;

/**
 * High-performance Market Data Feed for Coinbase integration
 * 
 * Design Goals:
 * - Robust WebSocket connection management with auto-reconnection
 * - Low-latency message processing (< 100 microseconds)
 * - Thread-safe integration with OrderBookEngine
 * - Comprehensive error handling and recovery
 * - Real-time performance monitoring
 * - Support for multiple product subscriptions
 */
class MarketDataFeed {
public:
    explicit MarketDataFeed(OrderBookEngine& order_book,
                           LatencyTracker& latency_tracker,
                           const MarketDataConfig& config);
    
    ~MarketDataFeed();
    
    // Non-copyable, non-movable for safety
    MarketDataFeed(const MarketDataFeed&) = delete;
    MarketDataFeed& operator=(const MarketDataFeed&) = delete;
    MarketDataFeed(MarketDataFeed&&) = delete;
    MarketDataFeed& operator=(MarketDataFeed&&) = delete;
    
    // =========================================================================
    // CONNECTION MANAGEMENT
    // =========================================================================
    
    /**
     * Start market data feed connection
     */
    bool start();
    
    /**
     * Stop market data feed connection
     */
    void stop();
    
    /**
     * Check if currently connected
     */
    bool is_connected() const;
    
    /**
     * Force reconnection
     */
    void reconnect();
    
    /**
     * Get current connection state
     */
    ConnectionState get_connection_state() const;
    
    // =========================================================================
    // SUBSCRIPTION MANAGEMENT
    // =========================================================================
    
    /**
     * Subscribe to product feeds
     */
    bool subscribe_to_product(const std::string& product_id);
    
    /**
     * Unsubscribe from product feeds
     */
    bool unsubscribe_from_product(const std::string& product_id);
    
    /**
     * Get list of subscribed products
     */
    std::vector<std::string> get_subscribed_products() const;
    
    // =========================================================================
    // CONFIGURATION AND CONTROL
    // =========================================================================
    
    /**
     * Update configuration (requires restart)
     */
    void update_config(const MarketDataConfig& config);
    
    /**
     * Load configuration from environment variables
     */
    static MarketDataConfig load_config_from_env();
    
    /**
     * Enable/disable automatic reconnection
     */
    void set_auto_reconnect(bool enabled);
    
    // =========================================================================
    // CALLBACKS AND EVENTS
    // =========================================================================
    
    /**
     * Set event callbacks
     */
    void set_connection_state_callback(ConnectionStateCallback callback);
    void set_trade_message_callback(TradeMessageCallback callback);
    void set_book_message_callback(BookMessageCallback callback);
    void set_error_callback(ErrorCallback callback);
    
    // =========================================================================
    // MONITORING AND STATISTICS
    // =========================================================================
    
    /**
     * Get market data statistics
     */
    MarketDataStats get_statistics() const;
    
    /**
     * Reset statistics counters
     */
    void reset_statistics();
    
    /**
     * Print performance report
     */
    void print_performance_report() const;
    
    /**
     * Get message processing latency
     */
    double get_avg_processing_latency_us() const;
    
private:
    // =========================================================================
    // INTERNAL STATE AND CONFIGURATION
    // =========================================================================
    
    // External dependencies
    OrderBookEngine& order_book_;
    LatencyTracker& latency_tracker_;
    MarketDataConfig config_;
    
    // Connection state
    std::atomic<ConnectionState> connection_state_;
    std::atomic<bool> should_stop_;
    std::atomic<bool> auto_reconnect_enabled_;
    
    // Threading
    std::unique_ptr<std::thread> websocket_thread_;
    std::unique_ptr<std::thread> message_processor_thread_;
    
    // Message queue for decoupling network I/O from processing
    std::queue<std::string> message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // Subscribed products
    std::vector<std::string> subscribed_products_;
    std::mutex products_mutex_;
    
    // Statistics and monitoring
    mutable std::mutex stats_mutex_;
    MarketDataStats statistics_;
    
    // Event callbacks
    ConnectionStateCallback connection_callback_;
    TradeMessageCallback trade_callback_;
    BookMessageCallback book_callback_;
    ErrorCallback error_callback_;
    
    // WebSocket connection handle (implementation-specific)
    void* websocket_handle_;  // Will be cast to actual WebSocket library type
    
    // =========================================================================
    // INTERNAL IMPLEMENTATION FUNCTIONS
    // =========================================================================
    
    // Connection management
    bool establish_connection();
    void close_connection();
    void websocket_thread_main();
    void handle_connection_error(const std::string& error);
    
    // Subscription management
    void send_subscription_message();
    void send_unsubscription_message(const std::string& product_id);
    bool send_websocket_message(const std::string& message);
    
    // Message processing
    void message_processor_thread_main();
    void process_message(const std::string& raw_message);
    void handle_trade_message(const std::string& message);
    void handle_book_message(const std::string& message);
    void handle_heartbeat_message(const std::string& message);
    void handle_error_message(const std::string& message);
    
    // Message parsing
    CoinbaseMessageType parse_message_type(const std::string& message);
    CoinbaseTradeMessage parse_trade_message(const std::string& message);
    CoinbaseBookMessage parse_book_message(const std::string& message);
    
    // Data conversion
    price_t parse_price(const std::string& price_str);
    quantity_t parse_quantity(const std::string& qty_str);
    Side parse_side(const std::string& side_str);
    timestamp_t parse_timestamp(const std::string& time_str);
    
    // Integration with OrderBookEngine
    void update_order_book_from_trade(const CoinbaseTradeMessage& trade);
    void update_order_book_from_snapshot(const CoinbaseBookMessage& book);
    void update_order_book_from_l2update(const CoinbaseBookMessage& book);
    
    // Performance tracking
    void track_message_latency(timestamp_t message_time);
    void update_statistics(CoinbaseMessageType msg_type);
    
    // Reconnection logic
    void schedule_reconnection();
    void attempt_reconnection();
    
    // Error handling
    void notify_error(const std::string& error_message);
    void notify_connection_state_change(ConnectionState new_state, const std::string& message = "");
    
    // Utility functions
    std::string create_subscription_json() const;
    std::string create_auth_signature(const std::string& message) const;
    bool validate_message(const std::string& message) const;
    void log_message(const std::string& level, const std::string& message) const;
};

/**
 * Factory function to create market data feed from environment
 */
std::unique_ptr<MarketDataFeed> create_coinbase_feed(OrderBookEngine& order_book,
                                                    LatencyTracker& latency_tracker,
                                                    const std::string& product_id = "BTC-USD");

} // namespace hft 