#include "market_data_feed.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <chrono>

// TODO: Add these includes when implementing WebSocket
// #include <websocketpp/config/asio_no_tls.hpp>
// #include <websocketpp/client.hpp>
// #include <nlohmann/json.hpp>

namespace hft {

// =============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// =============================================================================

MarketDataFeed::MarketDataFeed(OrderBookEngine& order_book,
                              LatencyTracker& latency_tracker,
                              const MarketDataConfig& config)
    : order_book_(order_book)
    , latency_tracker_(latency_tracker)
    , config_(config)
    , connection_state_(ConnectionState::DISCONNECTED)
    , should_stop_(false)
    , auto_reconnect_enabled_(true)
    , websocket_handle_(nullptr) {
    
    std::cout << "[MARKET DATA] Initializing feed for " << config_.product_id << std::endl;
    
    // TODO: Initialize WebSocket client
    // TODO: Set up message queue limits
    // TODO: Initialize statistics
    
    // Add initial product to subscription list
    subscribed_products_.push_back(config_.product_id);
    
    std::cout << "[MARKET DATA] Initialized successfully" << std::endl;
}

MarketDataFeed::~MarketDataFeed() {
    std::cout << "[MARKET DATA] Shutting down market data feed" << std::endl;
    
    // Stop all operations
    stop();
    
    // TODO: Print final statistics
    auto stats = get_statistics();
    std::cout << "[MARKET DATA] Final stats - Messages: " << stats.messages_processed 
              << ", Trades: " << stats.trades_processed << std::endl;
    
    std::cout << "[MARKET DATA] Shutdown complete" << std::endl;
}

// =============================================================================
// CONNECTION MANAGEMENT
// =============================================================================

bool MarketDataFeed::start() {
    std::cout << "[MARKET DATA] Starting market data feed..." << std::endl;
    
    if (connection_state_.load() != ConnectionState::DISCONNECTED) {
        std::cout << "[MARKET DATA] Already connected or connecting" << std::endl;
        return false;
    }
    
    should_stop_.store(false);
    
    // TODO: Start WebSocket thread
    // websocket_thread_ = std::make_unique<std::thread>(&MarketDataFeed::websocket_thread_main, this);
    
    // TODO: Start message processor thread
    // message_processor_thread_ = std::make_unique<std::thread>(&MarketDataFeed::message_processor_thread_main, this);
    
    // TODO: Establish connection
    bool connected = establish_connection();
    
    if (connected) {
        std::cout << "[MARKET DATA] Successfully started market data feed" << std::endl;
    } else {
        std::cout << "[MARKET DATA] Failed to start market data feed" << std::endl;
    }
    
    return connected;
}

void MarketDataFeed::stop() {
    std::cout << "[MARKET DATA] Stopping market data feed..." << std::endl;
    
    should_stop_.store(true);
    
    // TODO: Close WebSocket connection
    close_connection();
    
    // TODO: Stop threads
    queue_cv_.notify_all();  // Wake up message processor
    
    if (websocket_thread_ && websocket_thread_->joinable()) {
        websocket_thread_->join();
    }
    
    if (message_processor_thread_ && message_processor_thread_->joinable()) {
        message_processor_thread_->join();
    }
    
    connection_state_.store(ConnectionState::DISCONNECTED);
    
    std::cout << "[MARKET DATA] Market data feed stopped" << std::endl;
}

bool MarketDataFeed::is_connected() const {
    auto state = connection_state_.load();
    return state == ConnectionState::CONNECTED || state == ConnectionState::SUBSCRIBED;
}

void MarketDataFeed::reconnect() {
    std::cout << "[MARKET DATA] Manual reconnection requested" << std::endl;
    
    // TODO: Close current connection
    close_connection();
    
    // TODO: Attempt reconnection
    if (auto_reconnect_enabled_.load()) {
        attempt_reconnection();
    }
}

ConnectionState MarketDataFeed::get_connection_state() const {
    return connection_state_.load();
}

// =============================================================================
// SUBSCRIPTION MANAGEMENT
// =============================================================================

bool MarketDataFeed::subscribe_to_product(const std::string& product_id) {
    std::lock_guard<std::mutex> lock(products_mutex_);
    
    // Check if already subscribed
    auto it = std::find(subscribed_products_.begin(), subscribed_products_.end(), product_id);
    if (it != subscribed_products_.end()) {
        std::cout << "[MARKET DATA] Already subscribed to " << product_id << std::endl;
        return true;
    }
    
    subscribed_products_.push_back(product_id);
    
    // TODO: Send subscription message if connected
    if (is_connected()) {
        send_subscription_message();
    }
    
    std::cout << "[MARKET DATA] Subscribed to " << product_id << std::endl;
    return true;
}

bool MarketDataFeed::unsubscribe_from_product(const std::string& product_id) {
    std::lock_guard<std::mutex> lock(products_mutex_);
    
    auto it = std::find(subscribed_products_.begin(), subscribed_products_.end(), product_id);
    if (it == subscribed_products_.end()) {
        std::cout << "[MARKET DATA] Not subscribed to " << product_id << std::endl;
        return false;
    }
    
    subscribed_products_.erase(it);
    
    // TODO: Send unsubscription message if connected
    if (is_connected()) {
        send_unsubscription_message(product_id);
    }
    
    std::cout << "[MARKET DATA] Unsubscribed from " << product_id << std::endl;
    return true;
}

std::vector<std::string> MarketDataFeed::get_subscribed_products() const {
    std::lock_guard<std::mutex> lock(products_mutex_);
    return subscribed_products_;
}

// =============================================================================
// CONFIGURATION AND CONTROL
// =============================================================================

void MarketDataFeed::update_config(const MarketDataConfig& config) {
    config_ = config;
    std::cout << "[MARKET DATA] Configuration updated (restart required)" << std::endl;
}

MarketDataConfig MarketDataFeed::load_config_from_env() {
    MarketDataConfig config;
    
    // Load from environment variables
    const char* api_key = std::getenv("COINBASE_API_KEY");
    if (api_key) {
        config.coinbase_api_key = api_key;
        std::cout << "[MARKET DATA] Loaded API key from environment" << std::endl;
    }
    
    const char* api_secret = std::getenv("COINBASE_API_SECRET");
    if (api_secret) {
        config.coinbase_api_secret = api_secret;
        std::cout << "[MARKET DATA] Loaded API secret from environment" << std::endl;
    }
    
    const char* product_id = std::getenv("COINBASE_PRODUCT_ID");
    if (product_id) {
        config.product_id = product_id;
        std::cout << "[MARKET DATA] Using product: " << config.product_id << std::endl;
    }
    
    const char* websocket_url = std::getenv("COINBASE_WEBSOCKET_URL");
    if (websocket_url) {
        config.websocket_url = websocket_url;
    }
    
    return config;
}

void MarketDataFeed::set_auto_reconnect(bool enabled) {
    auto_reconnect_enabled_.store(enabled);
    std::cout << "[MARKET DATA] Auto-reconnect " << (enabled ? "enabled" : "disabled") << std::endl;
}

// =============================================================================
// CALLBACKS AND EVENTS
// =============================================================================

void MarketDataFeed::set_connection_state_callback(ConnectionStateCallback callback) {
    connection_callback_ = callback;
}

void MarketDataFeed::set_trade_message_callback(TradeMessageCallback callback) {
    trade_callback_ = callback;
}

void MarketDataFeed::set_book_message_callback(BookMessageCallback callback) {
    book_callback_ = callback;
}

void MarketDataFeed::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

// =============================================================================
// MONITORING AND STATISTICS
// =============================================================================

MarketDataStats MarketDataFeed::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return statistics_;
}

void MarketDataFeed::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_ = MarketDataStats();
    std::cout << "[MARKET DATA] Statistics reset" << std::endl;
}

void MarketDataFeed::print_performance_report() const {
    auto stats = get_statistics();
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "📡 MARKET DATA FEED PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    std::cout << "\n📊 MESSAGE STATISTICS:" << std::endl;
    std::cout << "  Messages Received:    " << std::setw(10) << stats.messages_received << std::endl;
    std::cout << "  Messages Processed:   " << std::setw(10) << stats.messages_processed << std::endl;
    std::cout << "  Messages Dropped:     " << std::setw(10) << stats.messages_dropped << std::endl;
    std::cout << "  Trades Processed:     " << std::setw(10) << stats.trades_processed << std::endl;
    std::cout << "  Book Updates:         " << std::setw(10) << stats.book_updates_processed << std::endl;
    
    std::cout << "\n🔌 CONNECTION STATISTICS:" << std::endl;
    std::cout << "  Reconnection Count:   " << std::setw(10) << stats.reconnection_count << std::endl;
    std::cout << "  Connection State:     " << std::setw(10) << static_cast<int>(get_connection_state()) << std::endl;
    
    std::cout << "\n⚡ PERFORMANCE METRICS:" << std::endl;
    std::cout << "  Avg Processing Latency: " << std::fixed << std::setprecision(2) 
              << std::setw(8) << stats.avg_latency_us << " μs" << std::endl;
    
    // Calculate uptime
    auto uptime_us = time_diff_us(stats.connection_start_time, now());
    double uptime_seconds = to_microseconds(uptime_us) / 1000000.0;
    std::cout << "  Connection Uptime:    " << std::fixed << std::setprecision(1) 
              << uptime_seconds << " seconds" << std::endl;
    
    if (stats.messages_processed > 0) {
        double processing_rate = stats.messages_processed / uptime_seconds;
        std::cout << "  Message Rate:         " << std::fixed << std::setprecision(1) 
                  << processing_rate << " msg/sec" << std::endl;
    }
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
}

double MarketDataFeed::get_avg_processing_latency_us() const {
    auto stats = get_statistics();
    return stats.avg_latency_us;
}

// =============================================================================
// INTERNAL IMPLEMENTATION FUNCTIONS (PRIVATE)
// =============================================================================

bool MarketDataFeed::establish_connection() {
    std::cout << "[MARKET DATA] Establishing connection to " << config_.websocket_url << std::endl;
    
    connection_state_.store(ConnectionState::CONNECTING);
    
    // TODO: Implement WebSocket connection
    // Example pseudocode:
    // 1. Create WebSocket client
    // 2. Set up event handlers
    // 3. Connect to Coinbase WebSocket URL
    // 4. Wait for connection confirmation
    // 5. Send subscription message
    
    // PLACEHOLDER: Simulate successful connection
    connection_state_.store(ConnectionState::CONNECTED);
    notify_connection_state_change(ConnectionState::CONNECTED, "Connected to Coinbase");
    
    // TODO: Send subscription message
    send_subscription_message();
    
    return true; // TODO: Return actual connection result
}

void MarketDataFeed::close_connection() {
    if (connection_state_.load() == ConnectionState::DISCONNECTED) {
        return;
    }
    
    std::cout << "[MARKET DATA] Closing WebSocket connection" << std::endl;
    
    // TODO: Close WebSocket connection
    // TODO: Clean up resources
    
    connection_state_.store(ConnectionState::DISCONNECTED);
    notify_connection_state_change(ConnectionState::DISCONNECTED, "Connection closed");
}

void MarketDataFeed::websocket_thread_main() {
    std::cout << "[MARKET DATA] WebSocket thread started" << std::endl;
    
    // TODO: Main WebSocket event loop
    // Example structure:
    // while (!should_stop_.load()) {
    //     // Handle WebSocket events
    //     // Receive messages
    //     // Queue messages for processing
    //     // Handle connection errors
    // }
    
    std::cout << "[MARKET DATA] WebSocket thread finished" << std::endl;
}

void MarketDataFeed::handle_connection_error(const std::string& error) {
    std::cout << "[MARKET DATA] Connection error: " << error << std::endl;
    
    connection_state_.store(ConnectionState::ERROR);
    notify_error(error);
    
    // TODO: Attempt reconnection if enabled
    if (auto_reconnect_enabled_.load()) {
        schedule_reconnection();
    }
}

void MarketDataFeed::send_subscription_message() {
    std::cout << "[MARKET DATA] Sending subscription message" << std::endl;
    
    // TODO: Create and send subscription JSON
    std::string subscription_json = create_subscription_json();
    
    if (send_websocket_message(subscription_json)) {
        connection_state_.store(ConnectionState::SUBSCRIBED);
        notify_connection_state_change(ConnectionState::SUBSCRIBED, "Subscribed to channels");
    }
}

void MarketDataFeed::send_unsubscription_message(const std::string& product_id) {
    std::cout << "[MARKET DATA] Sending unsubscription for " << product_id << std::endl;
    
    // TODO: Create and send unsubscription JSON
    // TODO: Send via WebSocket
}

bool MarketDataFeed::send_websocket_message(const std::string& message) {
    // TODO: Send message via WebSocket
    std::cout << "[MARKET DATA] Sending: " << message.substr(0, 100) << "..." << std::endl;
    return true; // TODO: Return actual send result
}

void MarketDataFeed::message_processor_thread_main() {
    std::cout << "[MARKET DATA] Message processor thread started" << std::endl;
    
    while (!should_stop_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        
        // Wait for messages or stop signal
        queue_cv_.wait(lock, [this] {
            return !message_queue_.empty() || should_stop_.load();
        });
        
        // Process all queued messages
        while (!message_queue_.empty()) {
            std::string message = message_queue_.front();
            message_queue_.pop();
            lock.unlock();
            
            // Process message outside of lock
            process_message(message);
            
            lock.lock();
        }
    }
    
    std::cout << "[MARKET DATA] Message processor thread finished" << std::endl;
}

void MarketDataFeed::process_message(const std::string& raw_message) {
    MEASURE_LATENCY(latency_tracker_, LatencyType::MARKET_DATA_PROCESSING);
    
    // TODO: Parse message type and route to appropriate handler
    auto msg_type = parse_message_type(raw_message);
    
    switch (msg_type) {
        case CoinbaseMessageType::MATCH:
            handle_trade_message(raw_message);
            break;
        case CoinbaseMessageType::SNAPSHOT:
        case CoinbaseMessageType::L2UPDATE:
            handle_book_message(raw_message);
            break;
        case CoinbaseMessageType::HEARTBEAT:
            handle_heartbeat_message(raw_message);
            break;
        case CoinbaseMessageType::ERROR_MSG:
            handle_error_message(raw_message);
            break;
        default:
            // Unknown message type - log and ignore
            break;
    }
    
    update_statistics(msg_type);
}

void MarketDataFeed::handle_trade_message(const std::string& message) {
    // TODO: Parse trade message and update order book
    auto trade = parse_trade_message(message);
    
    // Update order book with trade
    update_order_book_from_trade(trade);
    
    // Notify callbacks
    if (trade_callback_) {
        trade_callback_(trade);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.trades_processed++;
}

void MarketDataFeed::handle_book_message(const std::string& message) {
    // TODO: Parse book message and update order book
    auto book = parse_book_message(message);
    
    if (book.type == "snapshot") {
        update_order_book_from_snapshot(book);
    } else if (book.type == "l2update") {
        update_order_book_from_l2update(book);
    }
    
    // Notify callbacks
    if (book_callback_) {
        book_callback_(book);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.book_updates_processed++;
}

void MarketDataFeed::handle_heartbeat_message(const std::string& message) {
    // TODO: Update last heartbeat time
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.last_message_time = now();
}

void MarketDataFeed::handle_error_message(const std::string& message) {
    std::cout << "[MARKET DATA] Error message received: " << message << std::endl;
    notify_error(message);
}

// =============================================================================
// MESSAGE PARSING FUNCTIONS
// =============================================================================

CoinbaseMessageType MarketDataFeed::parse_message_type(const std::string& message) {
    // TODO: Parse JSON and extract message type
    // Example: {"type": "match", ...}
    
    if (message.find("\"type\":\"match\"") != std::string::npos) {
        return CoinbaseMessageType::MATCH;
    } else if (message.find("\"type\":\"snapshot\"") != std::string::npos) {
        return CoinbaseMessageType::SNAPSHOT;
    } else if (message.find("\"type\":\"l2update\"") != std::string::npos) {
        return CoinbaseMessageType::L2UPDATE;
    } else if (message.find("\"type\":\"heartbeat\"") != std::string::npos) {
        return CoinbaseMessageType::HEARTBEAT;
    }
    
    return CoinbaseMessageType::UNKNOWN;
}

CoinbaseTradeMessage MarketDataFeed::parse_trade_message(const std::string& message) {
    CoinbaseTradeMessage trade;
    
    // TODO: Parse JSON message into trade structure
    // Example JSON: {"type":"match","trade_id":"12345","maker_order_id":"...","taker_order_id":"...","side":"buy","size":"0.01","price":"50000.00","product_id":"BTC-USD","sequence":"123456789","time":"2024-01-01T12:00:00.000000Z"}
    
    // PLACEHOLDER: Parse basic fields
    trade.parsed_price = 50000.0;
    trade.parsed_size = 0.01;
    trade.parsed_side = Side::BUY;
    trade.parsed_time = now();
    
    return trade;
}

CoinbaseBookMessage MarketDataFeed::parse_book_message(const std::string& message) {
    CoinbaseBookMessage book;
    
    // TODO: Parse JSON message into book structure
    // Example JSON: {"type":"l2update","product_id":"BTC-USD","changes":[["buy","50000.00","0.5"],["sell","50100.00","0.3"]],"time":"2024-01-01T12:00:00.000000Z"}
    
    book.parsed_time = now();
    
    return book;
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

std::string MarketDataFeed::create_subscription_json() const {
    // TODO: Create proper JSON subscription message
    // Example: {"type":"subscribe","channels":[{"name":"level2","product_ids":["BTC-USD"]},{"name":"matches","product_ids":["BTC-USD"]}]}
    
    std::ostringstream json;
    json << "{\"type\":\"subscribe\",\"channels\":[";
    
    if (config_.subscribe_to_level2) {
        json << "{\"name\":\"level2\",\"product_ids\":[\"" << config_.product_id << "\"]}";
    }
    
    if (config_.subscribe_to_matches) {
        if (config_.subscribe_to_level2) json << ",";
        json << "{\"name\":\"matches\",\"product_ids\":[\"" << config_.product_id << "\"]}";
    }
    
    json << "]}";
    
    return json.str();
}

void MarketDataFeed::update_order_book_from_trade(const CoinbaseTradeMessage& trade) {
    // TODO: Update order book with trade execution
    // This might not directly modify the book, but could trigger callbacks
}

void MarketDataFeed::update_order_book_from_snapshot(const CoinbaseBookMessage& book) {
    // TODO: Replace order book with snapshot data
    std::cout << "[MARKET DATA] Processing book snapshot" << std::endl;
}

void MarketDataFeed::update_order_book_from_l2update(const CoinbaseBookMessage& book) {
    // TODO: Apply incremental updates to order book
    std::cout << "[MARKET DATA] Processing L2 update" << std::endl;
}

void MarketDataFeed::notify_error(const std::string& error_message) {
    if (error_callback_) {
        error_callback_(error_message);
    }
}

void MarketDataFeed::notify_connection_state_change(ConnectionState new_state, const std::string& message) {
    if (connection_callback_) {
        connection_callback_(new_state, message);
    }
}

void MarketDataFeed::update_statistics(CoinbaseMessageType msg_type) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.messages_processed++;
    statistics_.last_message_time = now();
}

void MarketDataFeed::attempt_reconnection() {
    std::cout << "[MARKET DATA] Attempting reconnection..." << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_.reconnection_count++;
    }
    
    // TODO: Implement reconnection logic
    // 1. Wait for reconnect delay
    // 2. Attempt to establish connection
    // 3. If successful, re-subscribe
    // 4. If failed, schedule another attempt
}

void MarketDataFeed::schedule_reconnection() {
    // TODO: Schedule reconnection attempt after delay
    std::cout << "[MARKET DATA] Scheduling reconnection in " << config_.reconnect_delay_ms << "ms" << std::endl;
}

// =============================================================================
// FACTORY FUNCTION
// =============================================================================

std::unique_ptr<MarketDataFeed> create_coinbase_feed(OrderBookEngine& order_book,
                                                    LatencyTracker& latency_tracker,
                                                    const std::string& product_id) {
    auto config = MarketDataFeed::load_config_from_env();
    config.product_id = product_id;
    
    return std::make_unique<MarketDataFeed>(order_book, latency_tracker, config);
}

} // namespace hft 