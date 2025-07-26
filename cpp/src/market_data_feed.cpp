#include "market_data_feed.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <chrono>

// WebSocket and JSON libraries
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

// SSL support
#include <websocketpp/config/asio_no_tls_client.hpp>

// OpenSSL for HMAC
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

// Type definitions for WebSocket client
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;
using WebSocketMessage = WebSocketClient::message_ptr;
using WebSocketConnection = WebSocketClient::connection_ptr;

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
    
    // Initialize WebSocket client
    try {
        auto ws_client = std::make_unique<WebSocketClient>();
        
        // Configure WebSocket client
        ws_client->clear_access_channels(websocketpp::log::alevel::all);
        ws_client->clear_error_channels(websocketpp::log::elevel::all);
        ws_client->set_access_channels(websocketpp::log::alevel::connect);
        ws_client->set_access_channels(websocketpp::log::alevel::disconnect);
        ws_client->set_access_channels(websocketpp::log::alevel::app);
        
        // Initialize ASIO
        ws_client->init_asio();
        
        // Set up TLS context for WSS connections
        ws_client->set_tls_init_handler([](websocketpp::connection_hdl) {
            auto ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::sslv23);
            try {
                ctx->set_options(boost::asio::ssl::context::default_workarounds |
                               boost::asio::ssl::context::no_sslv2 |
                               boost::asio::ssl::context::no_sslv3 |
                               boost::asio::ssl::context::single_dh_use);
                ctx->set_verify_mode(boost::asio::ssl::verify_none);
            } catch (std::exception& e) {
                std::cout << "[MARKET DATA] TLS context error: " << e.what() << std::endl;
            }
            return ctx;
        });
        
        ws_client->start_perpetual();
        
        // Store the WebSocket client
        websocket_handle_ = static_cast<void*>(ws_client.release());
        
        // Set up event handlers
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        
        client->set_open_handler([this](websocketpp::connection_hdl /* hdl */) {
            std::cout << "[MARKET DATA] WebSocket connection opened." << std::endl;
            connection_state_.store(ConnectionState::CONNECTED);
            notify_connection_state_change(ConnectionState::CONNECTED, "WebSocket connection opened");
        });

        client->set_close_handler([this](websocketpp::connection_hdl /* hdl */) {
            std::cout << "[MARKET DATA] WebSocket connection closed." << std::endl;
            connection_state_.store(ConnectionState::DISCONNECTED);
            notify_connection_state_change(ConnectionState::DISCONNECTED, "WebSocket connection closed");
            if (auto_reconnect_enabled_.load()) {
                schedule_reconnection();
            }
        });

        client->set_fail_handler([this](websocketpp::connection_hdl /* hdl */) {
            std::cout << "[MARKET DATA] WebSocket connection failed." << std::endl;
            connection_state_.store(ConnectionState::ERROR);
            notify_connection_state_change(ConnectionState::ERROR, "WebSocket connection failed");
            if (auto_reconnect_enabled_.load()) {
                schedule_reconnection();
            }
        });

        client->set_message_handler([this](websocketpp::connection_hdl /* hdl */, WebSocketMessage msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::text) {
                std::string message = msg->get_payload();
                std::cout << "[MARKET DATA] Received message: " << message.substr(0, 200) << (message.length() > 200 ? "..." : "") << std::endl;
                
                // Queue message for processing
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (message_queue_.size() < config_.message_queue_size) {
                    message_queue_.push(message);
                    queue_cv_.notify_one();
                } else {
                    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                    statistics_.messages_dropped++;
                }
            }
        });
        
        std::cout << "[MARKET DATA] WebSocket client initialized successfully" << std::endl;
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Failed to initialize WebSocket client: " << ex.what() << std::endl;
        websocket_handle_ = nullptr;
    }

    // Initialize statistics
    reset_statistics();
    
    // Add initial product to subscription list
    subscribed_products_.push_back(config_.product_id);
    
    std::cout << "[MARKET DATA] Initialized successfully" << std::endl;
}

MarketDataFeed::~MarketDataFeed() {
    std::cout << "[MARKET DATA] Shutting down market data feed" << std::endl;
    
    // Stop all operations
    stop();
    
    // Clean up WebSocket client
    if (websocket_handle_) {
        try {
            auto* client = static_cast<WebSocketClient*>(websocket_handle_);
            client->stop();
            delete client;
            websocket_handle_ = nullptr;
            std::cout << "[MARKET DATA] WebSocket client cleaned up" << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] Error cleaning up WebSocket client: " << ex.what() << std::endl;
        }
    }
    
    // Print final statistics
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
    
    // Start WebSocket thread
    websocket_thread_ = std::make_unique<std::thread>(&MarketDataFeed::websocket_thread_main, this);
    
    // Start message processor thread
    message_processor_thread_ = std::make_unique<std::thread>(&MarketDataFeed::message_processor_thread_main, this);
    
    // Establish connection
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
    
    // Close WebSocket connection
    close_connection();
    
    // Stop threads
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
    
    // Check if we should attempt reconnection
    if (should_stop_.load()) {
        std::cout << "[MARKET DATA] Skipping reconnection - system is shutting down" << std::endl;
        return;
    }
    
    // Close current connection if it exists
    auto current_state = connection_state_.load();
    if (current_state != ConnectionState::DISCONNECTED) {
        close_connection();
        
        // Wait a moment for clean disconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Attempt immediate reconnection (bypass scheduling)
    std::cout << "[MARKET DATA] Attempting immediate reconnection" << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_.reconnection_count++;
    }
    
    bool connected = establish_connection();
    
    if (connected) {
        std::cout << "[MARKET DATA] Manual reconnection successful" << std::endl;
        // Send subscription if we reconnected successfully
        if (is_connected()) {
            send_subscription_message();
        }
    } else {
        std::cout << "[MARKET DATA] Manual reconnection failed" << std::endl;
        // Only schedule automatic retry if auto-reconnect is enabled
        if (auto_reconnect_enabled_.load() && !should_stop_.load()) {
            schedule_reconnection();
        }
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
        // Only log in debug mode or for small numbers of subscriptions
        if (subscribed_products_.size() < 10) {
            std::cout << "[MARKET DATA] Already subscribed to " << product_id << std::endl;
        }
        return true;
    }
    
    subscribed_products_.push_back(product_id);
    
    // Send subscription message if connected
    if (is_connected()) {
        send_subscription_message();
    }
    
    // Only log for small numbers of subscriptions to avoid spam
    if (subscribed_products_.size() <= 10) {
        std::cout << "[MARKET DATA] Subscribed to " << product_id << std::endl;
    } else if (subscribed_products_.size() % 100 == 0) {
        std::cout << "[MARKET DATA] Total subscriptions: " << subscribed_products_.size() << std::endl;
    }
    
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
    
    // Send unsubscription message if connected
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
        std::cout << "[MARKET DATA] Loaded API key from environment: " << std::string(api_key, 0, 10) << "..." << std::endl;
    } else {
        std::cout << "[MARKET DATA] COINBASE_API_KEY not found in environment" << std::endl;
    }
    
    const char* api_secret = std::getenv("COINBASE_API_SECRET");
    if (api_secret) {
        config.coinbase_api_secret = api_secret;
        std::cout << "[MARKET DATA] Loaded API secret from environment (length: " << strlen(api_secret) << ")" << std::endl;
    } else {
        std::cout << "[MARKET DATA] COINBASE_API_SECRET not found in environment" << std::endl;
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
    
    if (!websocket_handle_) {
        std::cerr << "[MARKET DATA] WebSocket client not initialized" << std::endl;
        return false;
    }
    
    // Check if we're already connected or connecting
    auto current_state = connection_state_.load();
    if (current_state == ConnectionState::CONNECTED || 
        current_state == ConnectionState::CONNECTING ||
        current_state == ConnectionState::SUBSCRIBED) {
        std::cout << "[MARKET DATA] Already connected or connecting (state: " << static_cast<int>(current_state) << ")" << std::endl;
        return true;
    }
    
    connection_state_.store(ConnectionState::CONNECTING);
    
    try {
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        
        // Connect to Coinbase WebSocket URL
        websocketpp::lib::error_code ec;
        auto conn = client->get_connection(config_.websocket_url, ec);

        if (ec) {
            std::cerr << "[MARKET DATA] Failed to create connection: " << ec.message() << std::endl;
            connection_state_.store(ConnectionState::ERROR);
            notify_connection_state_change(ConnectionState::ERROR, "Failed to create WebSocket connection: " + ec.message());
            return false;
        }

        // Store connection handle for later use
        connection_hdl_ = conn->get_handle();
        
        // Connect asynchronously
        client->connect(conn);
        
        std::cout << "[MARKET DATA] Connection initiated successfully" << std::endl;
        
        // Wait a short time for connection to establish (with timeout)
        const int timeout_ms = 10000;  // Increased timeout
        const int check_interval_ms = 200;  // Increased check interval
        int elapsed_ms = 0;
        
        while (elapsed_ms < timeout_ms && 
               connection_state_.load() == ConnectionState::CONNECTING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
            elapsed_ms += check_interval_ms;
        }
        
        auto final_state = connection_state_.load();
        if (final_state == ConnectionState::CONNECTED || final_state == ConnectionState::SUBSCRIBED) {
            std::cout << "[MARKET DATA] Connection established successfully" << std::endl;
            return true;
        } else if (elapsed_ms >= timeout_ms) {
            std::cout << "[MARKET DATA] Connection timeout after " << timeout_ms << "ms" << std::endl;
            connection_state_.store(ConnectionState::ERROR);
            notify_connection_state_change(ConnectionState::ERROR, "Connection timeout");
            return false;
        } else {
            std::cout << "[MARKET DATA] Connection failed with state: " << static_cast<int>(final_state) << std::endl;
            return false;
        }
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Exception during connection: " << ex.what() << std::endl;
        connection_state_.store(ConnectionState::ERROR);
        notify_connection_state_change(ConnectionState::ERROR, "Exception during connection: " + std::string(ex.what()));
        return false;
    }
}

void MarketDataFeed::close_connection() {
    auto current_state = connection_state_.load();
    
    // Only attempt to close if we have an active connection
    if (current_state == ConnectionState::DISCONNECTED) {
        std::cout << "[MARKET DATA] Connection already disconnected" << std::endl;
        return;
    }
    
    std::cout << "[MARKET DATA] Closing WebSocket connection (current state: " << static_cast<int>(current_state) << ")" << std::endl;
    
    // Set state to disconnecting to prevent multiple close attempts
    connection_state_.store(ConnectionState::DISCONNECTING);
    
    if (websocket_handle_) {
        try {
            auto* client = static_cast<WebSocketClient*>(websocket_handle_);
            
            // Close WebSocket connection if we have a valid handle
            if (!connection_hdl_.expired()) {
                try {
                    websocketpp::lib::error_code ec;
                    client->close(connection_hdl_, websocketpp::close::status::going_away, "Application shutting down", ec);
                    if (ec) {
                        std::cout << "[MARKET DATA] Error closing WebSocket connection: " << ec.message() << std::endl;
                    } else {
                        std::cout << "[MARKET DATA] WebSocket connection close initiated" << std::endl;
                    }
                } catch (const std::exception& close_ex) {
                    std::cout << "[MARKET DATA] Exception while closing connection: " << close_ex.what() << std::endl;
                }
            } else {
                std::cout << "[MARKET DATA] WebSocket connection handle expired" << std::endl;
            }

            // Stop the client
            client->stop();
            
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] Error during connection cleanup: " << ex.what() << std::endl;
        }
    }
    
    // Wait a moment for cleanup to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    connection_state_.store(ConnectionState::DISCONNECTED);
    notify_connection_state_change(ConnectionState::DISCONNECTED, "Connection closed");
}

void MarketDataFeed::websocket_thread_main() {
    std::cout << "[MARKET DATA] WebSocket thread started" << std::endl;
    
    if (websocket_handle_) {
        try {
            auto* client = static_cast<WebSocketClient*>(websocket_handle_);
            // Main WebSocket event loop
            client->run();
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] WebSocket thread error: " << ex.what() << std::endl;
        }
    }
    
    std::cout << "[MARKET DATA] WebSocket thread finished" << std::endl;
}

void MarketDataFeed::handle_connection_error(const std::string& error) {
    std::cout << "[MARKET DATA] Connection error: " << error << std::endl;
    
    connection_state_.store(ConnectionState::ERROR);
    notify_error(error);
    
    // Attempt reconnection if enabled
    if (auto_reconnect_enabled_.load()) {
        schedule_reconnection();
    }
}

void MarketDataFeed::send_subscription_message() {
    std::cout << "[MARKET DATA] Sending subscription message" << std::endl;
    
    // Create and send subscription JSON
    std::string subscription_json = create_subscription_json();
    
    if (send_websocket_message(subscription_json)) {
        connection_state_.store(ConnectionState::SUBSCRIBED);
        notify_connection_state_change(ConnectionState::SUBSCRIBED, "Subscribed to channels");
    }
}

void MarketDataFeed::send_unsubscription_message(const std::string& product_id) {
    std::cout << "[MARKET DATA] Sending unsubscription for " << product_id << std::endl;
    
    // Create and send unsubscription JSON
    // TODO: Send via WebSocket
}

bool MarketDataFeed::send_websocket_message(const std::string& message) {
    if (!websocket_handle_ || connection_hdl_.expired()) {
        std::cerr << "[MARKET DATA] No active WebSocket connection" << std::endl;
        return false;
    }
    
    try {
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        websocketpp::lib::error_code ec;
        client->send(connection_hdl_, message, websocketpp::frame::opcode::text, ec);
        
        if (ec) {
            std::cerr << "[MARKET DATA] Error sending WebSocket message: " << ec.message() << std::endl;
            return false;
        }
        
        std::cout << "[MARKET DATA] Sent subscription message:" << std::endl;
        std::cout << message << std::endl;
        std::cout << "[MARKET DATA] Message length: " << message.length() << " bytes" << std::endl;
        std::cout << "[MARKET DATA] Waiting for server response..." << std::endl;
        return true;
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Exception sending message: " << ex.what() << std::endl;
        return false;
    }
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
    MEASURE_MARKET_DATA_LATENCY_FAST(latency_tracker_);
    
    // Parse message type and route to appropriate handler
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
    // Parse trade message and update order book
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
    // Parse book message and update order book
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

void MarketDataFeed::handle_heartbeat_message(const std::string& /* message */) {
    // Update last heartbeat time
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
    // Parse JSON and extract message type
    // Example: {"type": "match", ...}
    
    try {
        auto json = nlohmann::json::parse(message);
        std::string type_str = json["type"].get<std::string>();

        if (type_str == "match") {
            return CoinbaseMessageType::MATCH;
        } else if (type_str == "snapshot") {
            return CoinbaseMessageType::SNAPSHOT;
        } else if (type_str == "l2update") {
            return CoinbaseMessageType::L2UPDATE;
        } else if (type_str == "heartbeat") {
            return CoinbaseMessageType::HEARTBEAT;
        }
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[MARKET DATA] JSON parse error: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error parsing message: " << ex.what() << std::endl;
    }
    
    return CoinbaseMessageType::UNKNOWN;
}

CoinbaseTradeMessage MarketDataFeed::parse_trade_message(const std::string& message) {
    CoinbaseTradeMessage trade;
    
    // Parse JSON message into trade structure
    // Example JSON: {"type":"match","trade_id":"12345","maker_order_id":"...","taker_order_id":"...","side":"buy","size":"0.01","price":"50000.00","product_id":"BTC-USD","sequence":"123456789","time":"2024-01-01T12:00:00.000000Z"}
    
    try {
        auto json = nlohmann::json::parse(message);
        trade.trade_id = json["trade_id"].get<std::string>();
        trade.maker_order_id = json["maker_order_id"].get<std::string>();
        trade.taker_order_id = json["taker_order_id"].get<std::string>();
        trade.side = json["side"].get<std::string>();
        trade.size = json["size"].get<std::string>();
        trade.price = json["price"].get<std::string>();
        trade.product_id = json["product_id"].get<std::string>();
        trade.sequence = json["sequence"].get<std::string>();
        trade.time = json["time"].get<std::string>();
        
        // Parse to typed values
        trade.parsed_price = std::stod(trade.price);
        trade.parsed_size = std::stod(trade.size);
        trade.parsed_side = (trade.side == "buy") ? Side::BUY : Side::SELL;
        trade.parsed_time = now(); // TODO: Parse ISO8601 timestamp properly
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[MARKET DATA] JSON parse error for trade message: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error parsing trade message: " << ex.what() << std::endl;
    }
    
    return trade;
}

CoinbaseBookMessage MarketDataFeed::parse_book_message(const std::string& message) {
    CoinbaseBookMessage book;
    
    // Parse JSON message into book structure
    // Example JSON: {"type":"l2update","product_id":"BTC-USD","changes":[["buy","50000.00","0.5"],["sell","50100.00","0.3"]],"time":"2024-01-01T12:00:00.000000Z"}
    
    try {
        auto json = nlohmann::json::parse(message);
        book.type = json["type"].get<std::string>();
        book.product_id = json["product_id"].get<std::string>();
        book.time = json["time"].get<std::string>();
        book.parsed_time = now(); // TODO: Parse ISO8601 timestamp properly

        if (json.contains("changes")) {
            book.changes = json["changes"].get<std::vector<std::vector<std::string>>>();
            
            // Parse changes into typed format
            for (const auto& change : book.changes) {
                if (change.size() >= 3) {
                    std::string side_str = change[0];
                    double price = std::stod(change[1]);
                    double size = std::stod(change[2]);
                    Side side = (side_str == "buy") ? Side::BUY : Side::SELL;
                    
                    book.parsed_changes.emplace_back(side, price, size);
                }
            }
        }
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[MARKET DATA] JSON parse error for book message: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error parsing book message: " << ex.what() << std::endl;
    }
    
    return book;
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

std::string MarketDataFeed::create_subscription_json() const {
    // Create proper JSON subscription message using nlohmann::json
    // For authenticated channels, we need to include signature, key, passphrase, and timestamp
    
    try {
        nlohmann::json subscription;
        subscription["type"] = "subscribe";
        subscription["channels"] = nlohmann::json::array();
        
        std::vector<std::string> product_ids;
        {
            std::lock_guard<std::mutex> lock(products_mutex_);
            product_ids = subscribed_products_;
        }
        
        // Check if we need authentication (level2 and matches channels require auth for most products)
        bool needs_auth = config_.subscribe_to_level2 || config_.subscribe_to_matches;
        
        if (needs_auth && !config_.coinbase_api_key.empty() && !config_.coinbase_api_secret.empty()) {
            // Generate timestamp
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::string timestamp = std::to_string(now);
            
            // Create signature for authentication
            // Message to sign: timestamp + 'GET' + '/users/self/verify'
            std::string message = timestamp + "GET" + "/users/self/verify";
            
            // Create HMAC SHA256 signature
            std::string signature = create_hmac_signature(message, config_.coinbase_api_secret);
            
            // Add authentication fields
            subscription["signature"] = signature;
            subscription["key"] = config_.coinbase_api_key;
            subscription["timestamp"] = timestamp;
            
            std::cout << "[MARKET DATA] Using authentication:" << std::endl;
            std::cout << "  API Key: " << config_.coinbase_api_key.substr(0, 20) << "..." << std::endl;
            std::cout << "  Timestamp: " << timestamp << std::endl;
            std::cout << "  Message to sign: " << message << std::endl;
            std::cout << "  Generated signature: " << signature << std::endl;
        }
        
        if (config_.subscribe_to_level2) {
            nlohmann::json level2_channel;
            level2_channel["name"] = "level2";
            level2_channel["product_ids"] = product_ids;
            subscription["channels"].push_back(level2_channel);
        }
        
        if (config_.subscribe_to_matches) {
            nlohmann::json matches_channel;
            matches_channel["name"] = "matches";
            matches_channel["product_ids"] = product_ids;
            subscription["channels"].push_back(matches_channel);
        }
        
        if (config_.subscribe_to_heartbeat) {
            nlohmann::json heartbeat_channel;
            heartbeat_channel["name"] = "heartbeat";
            heartbeat_channel["product_ids"] = product_ids;
            subscription["channels"].push_back(heartbeat_channel);
        }
        
        return subscription.dump();
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error creating subscription JSON: " << ex.what() << std::endl;
        return "{}";
    }
}

void MarketDataFeed::update_order_book_from_trade(const CoinbaseTradeMessage& trade) {
    // Update order book with trade execution
    // This typically doesn't modify the book directly but can trigger callbacks
    std::cout << "[MARKET DATA] Processing trade: " << trade.product_id 
              << " " << trade.side << " " << trade.parsed_size 
              << " @ " << trade.parsed_price << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
}

std::string MarketDataFeed::create_hmac_signature(const std::string& message, const std::string& secret) const {
    // Create HMAC SHA256 signature for Coinbase authentication
    std::cout << "[MARKET DATA] Creating HMAC signature for message: " << message << std::endl;
    
    try {
        // Decode the base64 secret
        std::string decoded_secret;
        BIO* bio = BIO_new_mem_buf(secret.c_str(), secret.length());
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);
        
        char buffer[512];
        int decoded_length = BIO_read(bio, buffer, sizeof(buffer));
        BIO_free_all(bio);
        
        if (decoded_length <= 0) {
            std::cerr << "[MARKET DATA] Failed to decode base64 secret" << std::endl;
            return "";
        }
        
        decoded_secret.assign(buffer, decoded_length);
        
        // Create HMAC SHA256 hash
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len;
        
        HMAC(EVP_sha256(),
             decoded_secret.c_str(), decoded_secret.length(),
             reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
             hash, &hash_len);
        
        // Base64 encode the result
        BIO* bio_out = BIO_new(BIO_s_mem());
        BIO* b64_out = BIO_new(BIO_f_base64());
        BIO_set_flags(b64_out, BIO_FLAGS_BASE64_NO_NL);
        bio_out = BIO_push(b64_out, bio_out);
        
        BIO_write(bio_out, hash, hash_len);
        BIO_flush(bio_out);
        
        BUF_MEM* buffer_ptr;
        BIO_get_mem_ptr(bio_out, &buffer_ptr);
        
        std::string signature(buffer_ptr->data, buffer_ptr->length);
        BIO_free_all(bio_out);
        
        std::cout << "[MARKET DATA] Generated HMAC signature: " << signature << std::endl;
        return signature;
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error creating HMAC signature: " << ex.what() << std::endl;
        return "";
    }
}

void MarketDataFeed::update_order_book_from_snapshot(const CoinbaseBookMessage& book) {
    // Replace order book with snapshot data
    std::cout << "[MARKET DATA] Processing book snapshot for " << book.product_id << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
    // order_book_.apply_snapshot(book.parsed_changes);
}

void MarketDataFeed::update_order_book_from_l2update(const CoinbaseBookMessage& book) {
    // Apply incremental updates to order book
    std::cout << "[MARKET DATA] Processing L2 update for " << book.product_id 
              << " with " << book.parsed_changes.size() << " changes" << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
    // order_book_.apply_l2_update(book.parsed_changes);
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

void MarketDataFeed::update_statistics(CoinbaseMessageType /* msg_type */) {
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
    
    // Check if we should stop attempting reconnections
    if (should_stop_.load()) {
        std::cout << "[MARKET DATA] Stopping reconnection attempts due to shutdown" << std::endl;
        return;
    }
    
    // Check maximum retry limit
    const int max_retries = 10;
    if (statistics_.reconnection_count > max_retries) {
        std::cout << "[MARKET DATA] Maximum reconnection attempts (" << max_retries << ") exceeded" << std::endl;
        connection_state_.store(ConnectionState::ERROR);
        notify_connection_state_change(ConnectionState::ERROR, "Maximum reconnection attempts exceeded");
        return;
    }
    
    // Try to establish a new connection
    bool connected = establish_connection();
    
    if (connected) {
        std::cout << "[MARKET DATA] Reconnection successful" << std::endl;
        // Send subscription if we reconnected successfully
        if (is_connected()) {
            send_subscription_message();
        }
    } else {
        std::cout << "[MARKET DATA] Reconnection failed, scheduling retry" << std::endl;
        // Only schedule another attempt if auto-reconnect is still enabled
        if (auto_reconnect_enabled_.load() && !should_stop_.load()) {
            schedule_reconnection();
        }
    }
}

void MarketDataFeed::schedule_reconnection() {
    // Schedule reconnection attempt after delay using a separate thread
    std::cout << "[MARKET DATA] Scheduling reconnection in " << config_.reconnect_delay_ms << "ms" << std::endl;
    
    // Use a detached thread to avoid blocking and prevent infinite recursion
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.reconnect_delay_ms));
        
        // Check if we should still attempt reconnection
        if (auto_reconnect_enabled_.load() && !should_stop_.load() && 
            connection_state_.load() != ConnectionState::CONNECTED) {
            attempt_reconnection();
        } else {
            std::cout << "[MARKET DATA] Skipping scheduled reconnection (conditions changed)" << std::endl;
        }
    }).detach();
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

// =============================================================================
// BTC-USD SPECIFIC CONFIGURATION
// =============================================================================

MarketDataConfig create_btcusd_config() {
    MarketDataConfig config;
    
    // Load environment variables
    const char* api_key = std::getenv("COINBASE_API_KEY");
    if (api_key) {
        config.coinbase_api_key = api_key;
    }
    
    const char* api_secret = std::getenv("COINBASE_API_SECRET");
    if (api_secret) {
        config.coinbase_api_secret = api_secret;
    }
    
    // Configure for BTC-USD only
    config.product_id = "BTC-USD";
    config.websocket_url = "wss://ws-feed.exchange.coinbase.com";
    
    // Enable only trade and orderbook data
    config.subscribe_to_level2 = true;      // Orderbook data (snapshots and updates)
    config.subscribe_to_matches = true;     // Trade data (matches)
    config.subscribe_to_heartbeat = false;  // Disable heartbeat to reduce noise
    config.subscribe_to_ticker = false;     // Disable ticker data
    
    // Performance settings
    config.message_queue_size = 10000;
    config.reconnect_delay_ms = 5000;
    config.heartbeat_timeout_ms = 30000;
    
    return config;
}

std::unique_ptr<MarketDataFeed> create_btcusd_feed(OrderBookEngine& order_book,
                                                   LatencyTracker& latency_tracker) {
    auto config = create_btcusd_config();
    return std::make_unique<MarketDataFeed>(order_book, latency_tracker, config);
}

} // namespace hft 