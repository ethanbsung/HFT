#include "market_data_feed.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

// WebSocket and JSON libraries
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

// SSL support
#include <websocketpp/config/asio_no_tls_client.hpp>

// OpenSSL for base64
#include <openssl/bio.h>
#include <openssl/buffer.h>

// Sodium for Ed25519 JWT
#include <sodium.h>

// Boost timer for JWT refresh
#include <boost/asio/steady_timer.hpp>

// Type definitions for WebSocket client
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;
using WebSocketMessage = WebSocketClient::message_ptr;
using WebSocketConnection = WebSocketClient::connection_ptr;
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

std::vector<unsigned char> b64_decode(const std::string& b64) {
    BIO* b = BIO_new_mem_buf(b64.data(), b64.size());
    BIO* f = BIO_new(BIO_f_base64());
    BIO_set_flags(f, BIO_FLAGS_BASE64_NO_NL);
    b = BIO_push(f, b);
    std::vector<unsigned char> out(b64.size());
    int n = BIO_read(b, out.data(), out.size());
    BIO_free_all(b);
    out.resize(n > 0 ? n : 0);
    return out;
}

std::string b64url(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_write(b64, data, len);
    BIO_flush(b64);
    BUF_MEM* p;
    BIO_get_mem_ptr(mem, &p);
    std::string s(p->data, p->length);
    BIO_free_all(b64);
    for (char& c : s) if (c == '+') c = '-'; else if (c == '/') c = '_';
    s.erase(std::find(s.begin(), s.end(), '='), s.end());
    return s;
}

std::string b64url(const std::string& s) {
    return b64url((const unsigned char*)s.data(), s.size());
}

std::string rand_hex16() {
    unsigned char buf[16];
    randombytes_buf(buf, sizeof buf);
    static const char* h = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = h[buf[i] >> 4];
        out[2 * i + 1] = h[buf[i] & 0xf];
    }
    return out;
}

std::string build_jwt(const std::string& kid, const unsigned char sk[crypto_sign_SECRETKEYBYTES]) {
    long now = std::time(nullptr);
    json hdr = {{"alg", "EdDSA"}, {"typ", "JWT"}, {"kid", kid}, {"nonce", rand_hex16()}};
    json pay = {{"iss", "cdp"}, {"sub", kid}, {"nbf", now}, {"exp", now + 120}};
    std::string msg = b64url(hdr.dump()) + "." + b64url(pay.dump());
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig, nullptr, (const unsigned char*)msg.data(), msg.size(), sk);
    return msg + "." + b64url(sig, sizeof sig);
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
    , auto_reconnect_enabled_(true)
    , websocket_handle_(nullptr) {
    
    std::cout << "[MARKET DATA] Initializing Advanced Trade feed for " << config_.product_id << std::endl;
    
    // Initialize libsodium
    if (sodium_init() < 0) {
        std::cerr << "[MARKET DATA] libsodium init failed" << std::endl;
        return;
    }
    
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
    
    // Derive secret key bytes
    auto raw = b64_decode(secret_key);
    if (raw.size() == crypto_sign_SEEDBYTES) {
        crypto_sign_seed_keypair(public_key_, secret_key_, raw.data());
    } else if (raw.size() == crypto_sign_SECRETKEYBYTES) {
        std::copy(raw.begin(), raw.end(), secret_key_);
    } else {
        std::cerr << "[MARKET DATA] Secret must be 32 or 64-byte Ed25519 key" << std::endl;
        return;
    }
    
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
        
        // Set up TLS context
        ws_client->set_tls_init_handler([](websocketpp::connection_hdl) {
            return std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
        });
        
        ws_client->start_perpetual();
        
        // Store the WebSocket client
        websocket_handle_ = static_cast<void*>(ws_client.release());
        
        // Set up event handlers
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        
        client->set_open_handler([this](websocketpp::connection_hdl hdl) {
            std::cout << "[MARKET DATA] WebSocket connection opened." << std::endl;
            connection_state_.store(ConnectionState::CONNECTED);
            connection_hdl_ = hdl;
            notify_connection_state_change(ConnectionState::CONNECTED, "WebSocket connection opened");
            
            // Send subscriptions
            send_subscriptions(hdl);
            
            // Start JWT refresh timer
            start_jwt_refresh_timer(hdl);
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
                std::cout << "[MARKET DATA] Received: " << message.substr(0, 200) << (message.length() > 200 ? "..." : "") << std::endl;
                
                // Process message immediately
                process_message(message);
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
    if (websocket_thread_ && websocket_thread_->joinable()) {
        websocket_thread_->join();
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
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        statistics_.reconnection_count++;
    }
    
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
    std::cout << "[MARKET DATA] Establishing connection to Advanced Trade WebSocket" << std::endl;
    
    if (!websocket_handle_) {
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
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        
        // Connect to Advanced Trade WebSocket URL
        websocketpp::lib::error_code ec;
        auto conn = client->get_connection("wss://advanced-trade-ws.coinbase.com", ec);

        if (ec) {
            std::cerr << "[MARKET DATA] Failed to create connection: " << ec.message() << std::endl;
            connection_state_.store(ConnectionState::ERROR);
            notify_connection_state_change(ConnectionState::ERROR, "Failed to create WebSocket connection: " + ec.message());
            return false;
        }
        
        client->connect(conn);
        
        std::cout << "[MARKET DATA] Connection initiated successfully" << std::endl;
        
        // Wait for connection to establish
        const int timeout_ms = 10000;
        const int check_interval_ms = 200;
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
    
    if (current_state == ConnectionState::DISCONNECTED) {
        std::cout << "[MARKET DATA] Connection already disconnected" << std::endl;
        return;
    }
    
    std::cout << "[MARKET DATA] Closing WebSocket connection (current state: " << static_cast<int>(current_state) << ")" << std::endl;
    
    connection_state_.store(ConnectionState::DISCONNECTING);
    
    if (websocket_handle_) {
        try {
            auto* client = static_cast<WebSocketClient*>(websocket_handle_);
            
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

            client->stop();
            
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] Error during connection cleanup: " << ex.what() << std::endl;
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    connection_state_.store(ConnectionState::DISCONNECTED);
    notify_connection_state_change(ConnectionState::DISCONNECTED, "Connection closed");
}

void MarketDataFeed::websocket_thread_main() {
    std::cout << "[MARKET DATA] WebSocket thread started" << std::endl;
    
    if (websocket_handle_) {
        try {
            auto* client = static_cast<WebSocketClient*>(websocket_handle_);
            client->run();
        } catch (const std::exception& ex) {
            std::cerr << "[MARKET DATA] WebSocket thread error: " << ex.what() << std::endl;
        }
    }
    
    std::cout << "[MARKET DATA] WebSocket thread finished" << std::endl;
}

void MarketDataFeed::send_subscriptions(websocketpp::connection_hdl hdl) {
    std::cout << "[MARKET DATA] Sending subscriptions" << std::endl;
    
    // Build initial JWT
    std::string jwt = build_jwt(config_.coinbase_api_key, secret_key_);
    
    // Get subscribed products
    std::vector<std::string> products;
    {
        std::lock_guard<std::mutex> lock(products_mutex_);
        products = subscribed_products_;
    }
    
    // Subscribe to channels
    auto sub = [&](const std::string& channel) {
        json msg = {{"type", "subscribe"}, {"channel", channel}, {"product_ids", products}, {"jwt", jwt}};
        auto* client = static_cast<WebSocketClient*>(websocket_handle_);
        client->send(hdl, msg.dump(), websocketpp::frame::opcode::text);
        std::cout << "[MARKET DATA] >>> " << msg << std::endl;
    };
    
    sub("level2");
    sub("market_trades");
    sub("ticker");
    
    connection_state_.store(ConnectionState::SUBSCRIBED);
    notify_connection_state_change(ConnectionState::SUBSCRIBED, "Subscribed to channels");
}

void MarketDataFeed::start_jwt_refresh_timer(websocketpp::connection_hdl hdl) {
    auto* client = static_cast<WebSocketClient*>(websocket_handle_);
    auto& io = client->get_io_service();
    auto timer = std::make_shared<boost::asio::steady_timer>(io);
    
    std::function<void(websocketpp::connection_hdl)> refresh_jwt;
    
    refresh_jwt = [&, timer](websocketpp::connection_hdl hdl) {
        std::string jwt = build_jwt(config_.coinbase_api_key, secret_key_);
        // Re-authenticate by sending ping with refresh token
        json auth = {{"type", "ping"}, {"jwt", jwt}};
        client->send(hdl, auth.dump(), websocketpp::frame::opcode::text);
        timer->expires_after(std::chrono::seconds(110));
        timer->async_wait([&, timer](const boost::system::error_code& ec) {
            if (!ec) refresh_jwt(hdl);
        });
    };
    
    timer->expires_after(std::chrono::seconds(110));
    timer->async_wait([&, timer](const boost::system::error_code& ec) {
        if (!ec) refresh_jwt(hdl);
    });
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
    auto trade = parse_trade_message(message);
    
    update_order_book_from_trade(trade);
    
    if (trade_callback_) {
        trade_callback_(trade);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.trades_processed++;
}

void MarketDataFeed::handle_book_message(const std::string& message) {
    auto book = parse_book_message(message);
    
    if (book.type == "snapshot") {
        update_order_book_from_snapshot(book);
    } else if (book.type == "l2update") {
        update_order_book_from_l2update(book);
    }
    
    if (book_callback_) {
        book_callback_(book);
    }
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_.book_updates_processed++;
}

void MarketDataFeed::handle_heartbeat_message(const std::string& /* message */) {
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
        
        trade.parsed_price = std::stod(trade.price);
        trade.parsed_size = std::stod(trade.size);
        trade.parsed_side = (trade.side == "buy") ? Side::BUY : Side::SELL;
        trade.parsed_time = now();
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "[MARKET DATA] JSON parse error for trade message: " << ex.what() << std::endl;
    } catch (const std::exception& ex) {
        std::cerr << "[MARKET DATA] Error parsing trade message: " << ex.what() << std::endl;
    }
    
    return trade;
}

CoinbaseBookMessage MarketDataFeed::parse_book_message(const std::string& message) {
    CoinbaseBookMessage book;
    
    try {
        auto json = nlohmann::json::parse(message);
        book.type = json["type"].get<std::string>();
        book.product_id = json["product_id"].get<std::string>();
        book.time = json["time"].get<std::string>();
        book.parsed_time = now();

        if (json.contains("changes")) {
            book.changes = json["changes"].get<std::vector<std::vector<std::string>>>();
            
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

void MarketDataFeed::update_order_book_from_trade(const CoinbaseTradeMessage& trade) {
    std::cout << "[MARKET DATA] Processing trade: " << trade.product_id 
              << " " << trade.side << " " << trade.parsed_size 
              << " @ " << trade.parsed_price << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
}

void MarketDataFeed::update_order_book_from_snapshot(const CoinbaseBookMessage& book) {
    std::cout << "[MARKET DATA] Processing book snapshot for " << book.product_id << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
}

void MarketDataFeed::update_order_book_from_l2update(const CoinbaseBookMessage& book) {
    std::cout << "[MARKET DATA] Processing L2 update for " << book.product_id 
              << " with " << book.parsed_changes.size() << " changes" << std::endl;
    
    // TODO: Integrate with actual OrderBookEngine API when available
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
    
    if (should_stop_.load()) {
        std::cout << "[MARKET DATA] Stopping reconnection attempts due to shutdown" << std::endl;
        return;
    }
    
    const int max_retries = 10;
    if (statistics_.reconnection_count > max_retries) {
        std::cout << "[MARKET DATA] Maximum reconnection attempts (" << max_retries << ") exceeded" << std::endl;
        connection_state_.store(ConnectionState::ERROR);
        notify_connection_state_change(ConnectionState::ERROR, "Maximum reconnection attempts exceeded");
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
    config.subscribe_to_heartbeat = false;
    config.subscribe_to_ticker = false;
    
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