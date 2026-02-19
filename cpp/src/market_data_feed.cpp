#include "market_data_feed.hpp"
#include "log_control.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>

// WebSocket and JSON libraries
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

// Boost timer removed for HFT optimization

// Type definitions for WebSocket client
using WebSocketClient = hft::MarketDataWebSocketClient;
using WebSocketMessage = WebSocketClient::message_ptr;
using json = nlohmann::json;

namespace hft {

// =============================================================================
// HELPER FUNCTIONS (from websocket_test.cpp)
// =============================================================================

static inline void trim(std::string& s) {
    auto f = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), f));
    s.erase(std::find_if(s.rbegin(), s.rend(), f).base(), s.end());
}

static inline void strip_quotes(std::string& s) {
    if (s.size() > 1 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
}

void load_dotenv() {
    namespace fs = std::filesystem;
    for (fs::path p = fs::current_path();; p = p.parent_path()) {
        fs::path env = p / ".env";
        if (fs::exists(env)) {
            std::ifstream in(env);
            std::string ln;
            while (std::getline(in, ln)) {
                if (ln.empty() || ln[0] == '#') continue;
                auto eq = ln.find('=');
                if (eq == std::string::npos) continue;
                std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
                trim(k);
                trim(v);
                strip_quotes(v);
                if (!std::getenv(k.c_str())) setenv(k.c_str(), v.c_str(), 0);
            }
            break;
        }
        if (p == p.root_path()) break;
    }
}

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
    , auto_reconnect_enabled_(true) {
    
    std::cout << "[MARKET DATA] Initializing HFT feed for " << config_.product_id << std::endl;
    
    // Load environment variables
    load_dotenv();
    
    // Get API credentials
    const char* api_key = std::getenv("HFT_API_KEY");
    const char* secret_key = std::getenv("HFT_SECRET_KEY");
    
    if (!api_key || !secret_key) {
        std::cerr << "[MARKET DATA] HFT_API_KEY / HFT_SECRET_KEY not set" << std::endl;
        return;
    }
    
    config_.coinbase_api_key = api_key;
    config_.coinbase_api_secret = secret_key;
    
    // Initialize WebSocket client
    try {
        websocket_client_ = std::make_unique<WebSocketClient>();
        
        // Configure WebSocket client
        websocket_client_->clear_access_channels(websocketpp::log::alevel::all);
        websocket_client_->clear_error_channels(websocketpp::log::elevel::all);
        websocket_client_->set_access_channels(websocketpp::log::alevel::connect);
        websocket_client_->set_access_channels(websocketpp::log::alevel::disconnect);
        websocket_client_->set_access_channels(websocketpp::log::alevel::app);
        
        // Initialize ASIO
        websocket_client_->init_asio();
        
        // Set up TLS context
        websocket_client_->set_tls_init_handler([](websocketpp::connection_hdl) {
            return std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
        });
        
        websocket_client_->start_perpetual();
        
        // Set up event handlers
        websocket_client_->set_open_handler([this](websocketpp::connection_hdl hdl) {
            std::cout << "[MARKET DATA] WebSocket connected." << std::endl;
            connection_state_.store(ConnectionState::CONNECTED);
            connection_hdl_ = hdl;
            
            // Send subscriptions
            send_subscriptions(hdl);
        });

        websocket_client_->set_close_handler([this](websocketpp::connection_hdl /* hdl */) {
            std::cout << "[MARKET DATA] WebSocket disconnected." << std::endl;
            connection_state_.store(ConnectionState::DISCONNECTED);
            if (auto_reconnect_enabled_.load()) {
                schedule_reconnection();
            }
        });

        websocket_client_->set_message_handler([this](websocketpp::connection_hdl /* hdl */, WebSocketMessage msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::text) {
                // Capture arrival time immediately at WebSocket level
                auto arrival_time = now_monotonic_raw();
                process_message_with_arrival_time(msg->get_payload(), arrival_time);
            }
        });
        
        std::cout << "[MARKET DATA] WebSocket client initialized successfully" << std::endl;
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Failed to initialize WebSocket client: " << ex.what() << std::endl;
        websocket_client_.reset();
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
    
    // Stop threads with timeout
    if (websocket_thread_ && websocket_thread_->joinable()) {
        // Wait for thread to finish with timeout
        auto start_time = std::chrono::steady_clock::now();
        while (websocket_thread_->joinable() && 
               std::chrono::steady_clock::now() - start_time < std::chrono::seconds(3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (websocket_thread_->joinable()) {
            std::cout << "[MARKET DATA] WebSocket thread not finishing, detaching..." << std::endl;
            websocket_thread_->detach();
        } else {
            websocket_thread_->join();
        }
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
    
    if (should_stop_.load()) {
        std::cout << "[MARKET DATA] Skipping reconnection - system is shutting down" << std::endl;
        return;
    }
    
    auto current_state = connection_state_.load();
    if (current_state != ConnectionState::DISCONNECTED) {
        close_connection();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "[MARKET DATA] Attempting immediate reconnection" << std::endl;
    
    bool connected = establish_connection();
    
    if (connected) {
        std::cout << "[MARKET DATA] Manual reconnection successful" << std::endl;
    } else {
        std::cout << "[MARKET DATA] Manual reconnection failed" << std::endl;
        if (auto_reconnect_enabled_.load()) {
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
    
    auto it = std::find(subscribed_products_.begin(), subscribed_products_.end(), product_id);
    if (it != subscribed_products_.end()) {
        if (subscribed_products_.size() < 10) {
            std::cout << "[MARKET DATA] Already subscribed to " << product_id << std::endl;
        }
        return true;
    }
    
    subscribed_products_.push_back(product_id);
    
    if (is_connected()) {
        send_subscriptions(connection_hdl_);
    }
    
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
    
    const char* api_key = std::getenv("HFT_API_KEY");
    if (api_key) {
        config.coinbase_api_key = api_key;
        std::cout << "[MARKET DATA] Loaded API key from environment: " << std::string(api_key, 0, 10) << "..." << std::endl;
    } else {
        std::cout << "[MARKET DATA] HFT_API_KEY not found in environment" << std::endl;
    }
    
    const char* secret_key = std::getenv("HFT_SECRET_KEY");
    if (secret_key) {
        config.coinbase_api_secret = secret_key;
        std::cout << "[MARKET DATA] Loaded secret key from environment (length: " << strlen(secret_key) << ")" << std::endl;
    } else {
        std::cout << "[MARKET DATA] HFT_SECRET_KEY not found in environment" << std::endl;
    }
    
    const char* product_id = std::getenv("COINBASE_PRODUCT_ID");
    if (product_id) {
        config.product_id = product_id;
        std::cout << "[MARKET DATA] Using product: " << config.product_id << std::endl;
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
    std::cout << " HFT MARKET DATA FEED PERFORMANCE REPORT" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    std::cout << "\n MESSAGE STATISTICS:" << std::endl;
    std::cout << "  Messages Processed:   " << std::setw(10) << stats.messages_processed << std::endl;
    std::cout << "  Trades Processed:     " << std::setw(10) << stats.trades_processed << std::endl;
    std::cout << "  Book Updates:         " << std::setw(10) << stats.book_updates_processed << std::endl;
    
    std::cout << "\n CONNECTION STATISTICS:" << std::endl;
    std::cout << "  Connection State:     " << std::setw(10) << static_cast<int>(get_connection_state()) << std::endl;
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
}

double MarketDataFeed::get_avg_processing_latency_us() const {
    // Use LatencyTracker for performance metrics instead
    auto stats = latency_tracker_.get_statistics(LatencyType::MARKET_DATA_PROCESSING);
    return stats.mean_us;
}

// =============================================================================
// INTERNAL IMPLEMENTATION FUNCTIONS (PRIVATE)
// =============================================================================

bool MarketDataFeed::establish_connection() {
    std::cout << "[MARKET DATA] Establishing connection to Advanced Trade WebSocket" << std::endl;
    
    if (!websocket_client_) {
        std::cerr << "[MARKET DATA] WebSocket client not initialized" << std::endl;
        return false;
    }
    
    auto current_state = connection_state_.load();
    if (current_state == ConnectionState::CONNECTED || 
        current_state == ConnectionState::CONNECTING ||
        current_state == ConnectionState::SUBSCRIBED) {
        std::cout << "[MARKET DATA] Already connected or connecting (state: " << static_cast<int>(current_state) << ")" << std::endl;
        return true;
    }
    
    connection_state_.store(ConnectionState::CONNECTING);
    
    try {
        // Connect to Advanced Trade WebSocket URL
        websocketpp::lib::error_code ec;
        auto conn = websocket_client_->get_connection("wss://advanced-trade-ws.coinbase.com", ec);

        if (ec) {
            std::cerr << "[MARKET DATA] Failed to create connection: " << ec.message() << std::endl;
            connection_state_.store(ConnectionState::ERROR);
            return false;
        }
        
        websocket_client_->connect(conn);
        
        std::cout << "[MARKET DATA] Connection initiated successfully" << std::endl;
        
        // Wait for connection to establish
        const int timeout_ms = 5000;
        const int check_interval_ms = 100;
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
        } else {
            std::cout << "[MARKET DATA] Connection failed with state: " << static_cast<int>(final_state) << std::endl;
            return false;
        }
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Exception during connection: " << ex.what() << std::endl;
        connection_state_.store(ConnectionState::ERROR);
        return false;
    }
}

void MarketDataFeed::close_connection() {
    auto current_state = connection_state_.load();
    
    if (current_state == ConnectionState::DISCONNECTED) {
        std::cout << "[MARKET DATA] Connection already disconnected" << std::endl;
        return;
    }
    
    std::cout << "[MARKET DATA] Closing WebSocket connection" << std::endl;
    
    connection_state_.store(ConnectionState::DISCONNECTING);
    
    if (websocket_client_) {
        try {
            if (!connection_hdl_.expired()) {
                try {
                    websocketpp::lib::error_code ec;
                    websocket_client_->close(connection_hdl_, websocketpp::close::status::going_away, "Application shutting down", ec);
                    if (ec) {
                        std::cout << "[MARKET DATA] Error closing WebSocket connection: " << ec.message() << std::endl;
                    }
                } catch (const std::exception& close_ex) {
                    std::cout << "[MARKET DATA] Exception while closing connection: " << close_ex.what() << std::endl;
                }
            }

            websocket_client_->stop();
            
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] Error during connection cleanup: " << ex.what() << std::endl;
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    connection_state_.store(ConnectionState::DISCONNECTED);
}

void MarketDataFeed::websocket_thread_main() {
    std::cout << "[MARKET DATA] WebSocket thread started" << std::endl;
    
    if (websocket_client_) {
        try {
            // Run continuously until shutdown.
            while (!should_stop_.load()) {
                websocket_client_->run_one();  // Run one iteration instead of blocking run()
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            if (should_stop_.load()) {
                std::cout << "[MARKET DATA] WebSocket thread stopping due to shutdown signal" << std::endl;
            }
            
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] WebSocket thread error: " << ex.what() << std::endl;
        }
    }
    
    std::cout << "[MARKET DATA] WebSocket thread finished" << std::endl;
}

void MarketDataFeed::send_subscriptions(websocketpp::connection_hdl hdl) {
    std::cout << "[MARKET DATA] Sending subscriptions" << std::endl;
    
    // Get subscribed products
    std::vector<std::string> products;
    {
        std::lock_guard<std::mutex> lock(products_mutex_);
        products = subscribed_products_;
    }
    
    // Use the correct Coinbase Advanced Trade API subscription format
    auto sub = [&](const std::string& channel) {
        json msg = {
            {"type", "subscribe"},
            {"channel", channel},
            {"product_ids", products}
        };
        websocket_client_->send(hdl, msg.dump(), websocketpp::frame::opcode::text);
        std::cout << "[MARKET DATA] >>> Subscribing to " << channel << " for products: ";
        for (const auto& product : products) {
            std::cout << product << " ";
        }
        std::cout << std::endl;
        std::cout << "[MARKET DATA] >>> Message: " << msg.dump() << std::endl;
    };
    
    // Subscribe to the channels we need for market making
    sub("level2");
    sub("market_trades");
    
    connection_state_.store(ConnectionState::SUBSCRIBED);
    std::cout << "[MARKET DATA] Subscriptions sent successfully" << std::endl;
}

void MarketDataFeed::process_message_with_arrival_time(const std::string& raw_message, timestamp_t arrival_time) {
    ScopedCoutSilencer silence_hot_path(!kEnableHotPathLogging);
    
    // Update received message count
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_.messages_processed++;
    }
    
    try {
        auto json = nlohmann::json::parse(raw_message);
        
        // FIXED: Add more detailed logging to understand what messages we're receiving
        // Parsed JSON with " << json.size() << " fields"
        
        // Handle new Advanced Trade format
        if (json.contains("channel") && json.contains("events")) {
            std::string channel = json["channel"].get<std::string>();
            // Received " << channel << " message"
            
            if (channel == "market_trades") {
                handle_trade_message_with_arrival_time(raw_message, arrival_time);
            } else if (channel == "level2") {
                // Processing orderbook data
                handle_book_message_with_arrival_time(raw_message, arrival_time);
            } else if (channel == "ticker" || channel == "subscriptions") {
                // Non-book/trade channels are intentionally ignored.
            } else {
                // Unknown channel - log and ignore
                std::cout << "[MARKET DATA] Unknown channel: " << channel << std::endl;
            }
            
            update_statistics(CoinbaseMessageType::UNKNOWN);
            return;
        }

        // Strict Advanced Trade mode: ignore non-channel payloads.
        std::cout << "[MARKET DATA] Unsupported message format (missing channel/events), ignoring" << std::endl;
        update_statistics(CoinbaseMessageType::UNKNOWN);
        
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error processing message: " << ex.what() << std::endl;
        std::cerr << "[MARKET DATA] Raw message: " << raw_message.substr(0, 200) << "..." << std::endl;
    }
}

void MarketDataFeed::handle_trade_message_with_arrival_time(const std::string& message, timestamp_t arrival_time) {
    auto trade = parse_trade_message(message);
    trade.arrival_time = arrival_time;  // Store the real arrival time
    
    // Calculate processing latency from arrival to completion
    auto processing_end = now_monotonic_raw();
    auto total_processing_latency = time_diff_us(arrival_time, processing_end);
    
    // Skip latency tracking for the first few messages (connection setup)
    static int trade_message_count = 0;
    trade_message_count++;
    
    // Only track latency after the first 3 messages (connection and subscription setup)
    if (trade_message_count > 3) {
        // Track trade processing latency for realistic messages only
        latency_tracker_.add_latency_fast_path(LatencyType::MARKET_DATA_PROCESSING, to_microseconds(total_processing_latency));
    }
    
    // Trade latency: " << to_microseconds(total_processing_latency) << "us"
    
    update_order_book_from_trade(trade);
    
    if (trade_callback_) {
        trade_callback_(trade);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.trades_processed++;
    statistics_.messages_processed++;
}

void MarketDataFeed::handle_trade_message(const std::string& message) {
    auto trade = parse_trade_message(message);
    
    update_order_book_from_trade(trade);
    
    if (trade_callback_) {
        trade_callback_(trade);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.trades_processed++;
    statistics_.messages_processed++;
}

void MarketDataFeed::handle_book_message_with_arrival_time(const std::string& message, timestamp_t arrival_time) {
    auto book = parse_book_message(message);
    book.arrival_time = arrival_time;  // Store the real arrival time
    
    // Calculate processing latency from arrival to completion
    auto processing_end = now_monotonic_raw();
    auto total_processing_latency = time_diff_us(arrival_time, processing_end);
    
    // Skip latency tracking for the first few messages (connection setup)
    static int book_message_count = 0;
    book_message_count++;
    
    // Only track latency after the first 3 messages (connection and subscription setup)
    if (book_message_count > 3) {
        // Track book processing latency for realistic messages only
        latency_tracker_.add_latency_fast_path(LatencyType::MARKET_DATA_PROCESSING, to_microseconds(total_processing_latency));
    }
    
    // Book latency: " << to_microseconds(total_processing_latency) << "us"
    
    if (book.type == "snapshot") {
        update_order_book_from_snapshot(book);
    } else if (book.type == "l2update" || book.type == "update") {
        update_order_book_from_l2update(book);
    }
    
    if (book_callback_) {
        book_callback_(book);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.book_updates_processed++;
    statistics_.messages_processed++;
}

void MarketDataFeed::handle_book_message(const std::string& message) {
    auto book = parse_book_message(message);
    
    if (book.type == "snapshot") {
        update_order_book_from_snapshot(book);
    } else if (book.type == "l2update" || book.type == "update") {
        update_order_book_from_l2update(book);
    }
    
    if (book_callback_) {
        book_callback_(book);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.book_updates_processed++;
    statistics_.messages_processed++;
}

void MarketDataFeed::handle_heartbeat_message(const std::string& /* message */) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.last_message_time = now();
    statistics_.messages_processed++;
}

void MarketDataFeed::handle_error_message(const std::string& message) {
    std::cout << "[MARKET DATA] Error message received: " << message << std::endl;
    notify_error(message);
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.messages_processed++;
}

// =============================================================================
// MESSAGE PARSING FUNCTIONS
// =============================================================================

CoinbaseTradeMessage MarketDataFeed::parse_trade_message(const std::string& message) {
    CoinbaseTradeMessage trade;
    
    try {
        auto json = nlohmann::json::parse(message);
        
        // Handle new Advanced Trade format with nested events
        if (json.contains("events") && json["events"].is_array() && !json["events"].empty()) {
            auto& first_event = json["events"][0];
            
            // Check if this is a trade event
            if (first_event.contains("trades") && first_event["trades"].is_array() && !first_event["trades"].empty()) {
                auto& trade_data = first_event["trades"][0];
                
                trade.trade_id = trade_data["trade_id"].get<std::string>();
                trade.maker_order_id = trade_data.value("maker_order_id", "");
                trade.taker_order_id = trade_data.value("taker_order_id", "");
                trade.side = trade_data["side"].get<std::string>();
                trade.size = trade_data["size"].get<std::string>();
                trade.price = trade_data["price"].get<std::string>();
                trade.product_id = trade_data["product_id"].get<std::string>();
                trade.sequence = trade_data.value("sequence", "");
                trade.time = trade_data.value("time", "");
                
                trade.parsed_price = std::stod(trade.price);
                trade.parsed_size = std::stod(trade.size);
                trade.parsed_side = (trade.side == "buy") ? Side::BUY : Side::SELL;
                trade.parsed_time = now();
                
                return trade;
            }
        }
        
        std::cout << "[MARKET DATA] Unsupported trade payload in Advanced Trade mode" << std::endl;
        
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[MARKET DATA] JSON parse error for trade message: " << ex.what() << std::endl;
        std::cerr << "[MARKET DATA] Raw message: " << message << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error parsing trade message: " << ex.what() << std::endl;
        std::cerr << "[MARKET DATA] Raw message: " << message << std::endl;
        // Return trade with default/zero values
        trade.parsed_size = 0.0;
        trade.parsed_price = 0.0;
    }
    
    return trade;
}

CoinbaseBookMessage MarketDataFeed::parse_book_message(const std::string& message) {
    CoinbaseBookMessage book;
    
    try {
        auto json = nlohmann::json::parse(message);
        
        // Handle new Advanced Trade format with nested events
        if (json.contains("events") && json["events"].is_array() && !json["events"].empty()) {
            auto& first_event = json["events"][0];
            
            book.type = first_event["type"].get<std::string>();
            book.product_id = first_event["product_id"].get<std::string>();
            book.time = first_event.value("time", "");
            book.parsed_time = now();

            // Handle updates array for L2 data
            if (first_event.contains("updates") && first_event["updates"].is_array()) {
                auto& updates = first_event["updates"];
                for (const auto& update : updates) {
                    if (update.contains("side") && update.contains("price_level") && update.contains("new_quantity")) {
                        std::vector<std::string> change;
                        change.push_back(update["side"].get<std::string>());
                        change.push_back(update["price_level"].get<std::string>());
                        change.push_back(update["new_quantity"].get<std::string>());
                        book.changes.push_back(change);
                        
                        // Parse for internal use
                        std::string side_str = change[0];
                        double price = std::stod(change[1]);
                        double size = std::stod(change[2]);
                        Side side = (side_str == "bid") ? Side::BUY : Side::SELL;
                        
                        book.parsed_changes.emplace_back(side, price, size);
                    }
                }
            }
            
            return book;
        }
        
        std::cout << "[MARKET DATA] Unsupported book payload in Advanced Trade mode" << std::endl;
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

void MarketDataFeed::update_order_book_from_trade(const CoinbaseTradeMessage& trade) {
    std::cout << "[MARKET DATA] Processing trade: " << trade.product_id 
              << " " << trade.side << " " << trade.parsed_size 
              << " @ " << trade.parsed_price << std::endl;
    
    // Create trade execution for OrderBookEngine
    TradeExecution trade_exec;
    trade_exec.trade_id = 0;  // Will be assigned by OrderBookEngine
    trade_exec.price = trade.parsed_price;
    trade_exec.quantity = trade.parsed_size;
    trade_exec.aggressor_side = trade.parsed_side;  // Use aggressor_side instead of side
    trade_exec.timestamp = trade.parsed_time;
    
    // Process trade in OrderBookEngine
    order_book_.process_market_data_trade(trade_exec);
}

void MarketDataFeed::update_order_book_from_snapshot(const CoinbaseBookMessage& book) {
    std::cout << "[MARKET DATA] Processing book snapshot for " << book.product_id << std::endl;
    rebuild_local_book_from_snapshot(book);
    publish_local_book(book.parsed_time);
}

void MarketDataFeed::update_order_book_from_l2update(const CoinbaseBookMessage& book) {
    std::cout << "[MARKET DATA] Processing L2 update for " << book.product_id 
              << " with " << book.parsed_changes.size() << " changes" << std::endl;
    apply_local_book_changes(book);
    publish_local_book(book.parsed_time);
}

void MarketDataFeed::rebuild_local_book_from_snapshot(const CoinbaseBookMessage& book) {
    std::lock_guard<std::mutex> lock(local_book_mutex_);

    local_bids_.clear();
    local_asks_.clear();

    for (const auto& change : book.parsed_changes) {
        Side side = std::get<0>(change);
        price_t price = std::get<1>(change);
        quantity_t quantity = std::get<2>(change);

        if (price <= 0.0 || quantity <= 0.0) {
            continue;
        }

        if (side == Side::BUY) {
            local_bids_[price] = quantity;
        } else {
            local_asks_[price] = quantity;
        }
    }

    local_book_initialized_ = true;
}

void MarketDataFeed::apply_local_book_changes(const CoinbaseBookMessage& book) {
    std::lock_guard<std::mutex> lock(local_book_mutex_);

    if (!local_book_initialized_) {
        std::cout << "[MARKET DATA] WARNING: Received L2 update before snapshot; bootstrapping from incremental data." << std::endl;
        local_book_initialized_ = true;
    }

    for (const auto& change : book.parsed_changes) {
        Side side = std::get<0>(change);
        price_t price = std::get<1>(change);
        quantity_t quantity = std::get<2>(change);

        if (price <= 0.0) {
            continue;
        }

        if (side == Side::BUY) {
            if (quantity <= 0.0) {
                local_bids_.erase(price);
            } else {
                local_bids_[price] = quantity;
            }
        } else {
            if (quantity <= 0.0) {
                local_asks_.erase(price);
            } else {
                local_asks_[price] = quantity;
            }
        }
    }
}

void MarketDataFeed::publish_local_book(timestamp_t book_time) {
    MarketDepth depth;

    {
        std::lock_guard<std::mutex> lock(local_book_mutex_);
        if (!local_book_initialized_) {
            return;
        }

        const size_t level_count = local_bids_.size() + local_asks_.size();

        // Force snapshot semantics even when one side is temporarily empty.
        depth.depth_levels = static_cast<uint32_t>(level_count == 0 ? 1 : level_count);
        depth.timestamp = (book_time == timestamp_t{}) ? now() : book_time;

        depth.bids.reserve(local_bids_.size());
        depth.asks.reserve(local_asks_.size());

        for (const auto& [price, quantity] : local_bids_) {
            if (quantity > 0.0) {
                depth.bids.emplace_back(price, quantity);
            }
        }

        for (const auto& [price, quantity] : local_asks_) {
            if (quantity > 0.0) {
                depth.asks.emplace_back(price, quantity);
            }
        }
    }

    order_book_.apply_market_data_update(depth);
}

void MarketDataFeed::notify_error(const std::string& error_message) {
    if (error_callback_) {
        error_callback_(error_message);
    }
}

void MarketDataFeed::update_statistics(CoinbaseMessageType /* msg_type */) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.last_message_time = now();
}

void MarketDataFeed::attempt_reconnection() {
    std::cout << "[MARKET DATA] Attempting reconnection..." << std::endl;
    
    if (should_stop_.load()) {
        std::cout << "[MARKET DATA] Stopping reconnection attempts due to shutdown" << std::endl;
        return;
    }
    
    bool connected = establish_connection();
    
    if (connected) {
        std::cout << "[MARKET DATA] Reconnection successful" << std::endl;
    } else {
        std::cout << "[MARKET DATA] Reconnection failed, scheduling retry" << std::endl;
        if (auto_reconnect_enabled_.load() && !should_stop_.load()) {
            schedule_reconnection();
        }
    }
}

void MarketDataFeed::schedule_reconnection() {
    std::cout << "[MARKET DATA] Scheduling reconnection in " << config_.reconnect_delay_ms << "ms" << std::endl;
    
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.reconnect_delay_ms));
        
        if (auto_reconnect_enabled_.load() && !should_stop_.load() && 
            connection_state_.load() != ConnectionState::CONNECTED) {
            attempt_reconnection();
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
    
    const char* api_key = std::getenv("HFT_API_KEY");
    if (api_key) {
        config.coinbase_api_key = api_key;
    }
    
    const char* secret_key = std::getenv("HFT_SECRET_KEY");
    if (secret_key) {
        config.coinbase_api_secret = secret_key;
    }
    
    config.product_id = "BTC-USD";
    config.websocket_url = "wss://advanced-trade-ws.coinbase.com";
    
    config.subscribe_to_level2 = true;
    config.subscribe_to_matches = true;
    
    config.reconnect_delay_ms = 1000;  // Faster reconnection for HFT
    
    return config;
}

std::unique_ptr<MarketDataFeed> create_btcusd_feed(OrderBookEngine& order_book,
                                                   LatencyTracker& latency_tracker) {
    auto config = create_btcusd_config();
    return std::make_unique<MarketDataFeed>(order_book, latency_tracker, config);
}

} // namespace hft 
