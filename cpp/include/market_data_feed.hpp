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
#include <mutex>
#include <map>

// Forward declarations for WebSocket
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

// JSON library
#include <nlohmann/json.hpp>

// Sodium removed for HFT optimization

namespace hft {

using MarketDataWebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

/**
 * Market data connection states
 */
enum class ConnectionState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    SUBSCRIBED = 3,
    ERROR = 4,
    RECONNECTING = 5,
    DISCONNECTING = 6
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
    
    // Real arrival timestamp (captured at WebSocket level)
    timestamp_t arrival_time;
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
    
    // Real arrival timestamp (captured at WebSocket level)
    timestamp_t arrival_time;
};

/**
 * Market data statistics
 */
struct MarketDataStats {
    uint64_t messages_processed;
    uint64_t trades_processed;
    uint64_t book_updates_processed;
    timestamp_t last_message_time;
    
    MarketDataStats() : messages_processed(0), trades_processed(0), 
                       book_updates_processed(0), last_message_time(now()) {}
};

/**
 * Configuration for market data feed
 */
struct MarketDataConfig {
    std::string coinbase_api_key;
    std::string coinbase_api_secret;
    std::string websocket_url = "wss://advanced-trade-ws.coinbase.com";
    std::string product_id = "BTC-USD";
    
    // Subscription options
    bool subscribe_to_level2 = true;
    bool subscribe_to_matches = true;
    
    // Performance settings
    uint32_t reconnect_delay_ms = 1000;  // Faster reconnection for HFT
    uint32_t message_queue_size = 1000;  // Message queue size for buffering
    
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
    
    // Subscribed products
    std::vector<std::string> subscribed_products_;
    mutable std::mutex products_mutex_;
    
    // Statistics and monitoring
    mutable std::mutex stats_mutex_;
    MarketDataStats statistics_;

    // Normalized local L2 book state (external market levels only)
    mutable std::mutex local_book_mutex_;
    std::map<price_t, quantity_t, std::greater<price_t>> local_bids_;
    std::map<price_t, quantity_t, std::less<price_t>> local_asks_;
    bool local_book_initialized_ = false;
    
    // Event callbacks
    ConnectionStateCallback connection_callback_;
    TradeMessageCallback trade_callback_;
    BookMessageCallback book_callback_;
    ErrorCallback error_callback_;
    
    // Typed WebSocket client owned by this feed.
    std::unique_ptr<MarketDataWebSocketClient> websocket_client_;
    
    // WebSocket connection handle for managing active connections
    websocketpp::connection_hdl connection_hdl_;
    
    // JWT authentication removed for HFT optimization
    
    // =========================================================================
    // INTERNAL IMPLEMENTATION FUNCTIONS
    // =========================================================================
    
    // Connection management
    bool establish_connection();
    void close_connection();
    void websocket_thread_main();
    
    // Subscription management
    void send_subscriptions(websocketpp::connection_hdl hdl);
    
    // Message processing
    void process_message_with_arrival_time(const std::string& raw_message, timestamp_t arrival_time);
    void handle_trade_message(const std::string& message);
    void handle_trade_message_with_arrival_time(const std::string& message, timestamp_t arrival_time);
    void handle_book_message(const std::string& message);
    void handle_book_message_with_arrival_time(const std::string& message, timestamp_t arrival_time);
    void handle_heartbeat_message(const std::string& message);
    void handle_error_message(const std::string& message);
    
    // Message parsing
    CoinbaseTradeMessage parse_trade_message(const std::string& message);
    CoinbaseBookMessage parse_book_message(const std::string& message);
    
    // Integration with OrderBookEngine
    void update_order_book_from_trade(const CoinbaseTradeMessage& trade);
    void update_order_book_from_snapshot(const CoinbaseBookMessage& book);
    void update_order_book_from_l2update(const CoinbaseBookMessage& book);
    void rebuild_local_book_from_snapshot(const CoinbaseBookMessage& book);
    void apply_local_book_changes(const CoinbaseBookMessage& book);
    void publish_local_book(timestamp_t book_time);
    void maybe_log_local_book();

    timestamp_t last_local_book_log_time_{};
    
    // Performance tracking
    void update_statistics(CoinbaseMessageType msg_type);
    
    // Reconnection logic
    void schedule_reconnection();
    void attempt_reconnection();
    
    // Error handling
    void notify_error(const std::string& error_message);
};

/**
 * Factory function to create market data feed from environment
 */
std::unique_ptr<MarketDataFeed> create_coinbase_feed(OrderBookEngine& order_book,
                                                    LatencyTracker& latency_tracker,
                                                    const std::string& product_id = "BTC-USD");

// =============================================================================
// BTC-USD SPECIFIC CONFIGURATION
// =============================================================================

/**
 * Create a configuration specifically for BTC-USD trade and orderbook data
 */
MarketDataConfig create_btcusd_config();

/**
 * Create a market data feed configured for BTC-USD only
 */
std::unique_ptr<MarketDataFeed> create_btcusd_feed(OrderBookEngine& order_book,
                                                   LatencyTracker& latency_tracker);

} // namespace hft 
