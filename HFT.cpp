#include "EWrapper.h"
#include "EClientSocket.h"
#include "Contract.h"
#include "Order.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class MarketMaker : public EWrapper {
public:
    MarketMaker() : client_(this), nextOrderId_(0), connected_(false) {}

    // EWrapper interface methods
    void tickPrice(TickerId tickerId, TickType field, double price, const TickAttrib& attrib) override {
        std::lock_guard<std::mutex> lock(marketDataMutex_);
        if (field == BID) {
            bidPrice_ = price;
            std::cout << "Bid Price: " << bidPrice_ << std::endl;
        } else if (field == ASK) {
            askPrice_ = price;
            std::cout << "Ask Price: " << askPrice_ << std::endl;
        }
        // Trigger order placement when both bid and ask are received
        if (bidPrice_ > 0 && askPrice_ > 0) {
            placeMarketMakerOrders();
        }
    }

    void nextValidId(OrderId orderId) override {
        nextOrderId_ = orderId;
        std::cout << "Next Valid Order ID: " << nextOrderId_ << std::endl;
        // Signal that the connection is ready
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            connected_ = true;
        }
        connectionCV_.notify_one();
    }

    void error(int id, int errorCode, const std::string& errorString) override {
        std::cerr << "Error. Id: " << id << ", Code: " << errorCode << ", Msg: " << errorString << std::endl;
    }

    // Other EWrapper methods can be overridden as needed
    void connectionClosed() override {
        std::cout << "Connection Closed." << std::endl;
    }

    // Additional methods for handling orders
    void placeMarketMakerOrders() {
        std::lock_guard<std::mutex> lock(orderMutex_);
        // Check if orders are already placed to avoid duplication
        if (buyOrderId_ != 0 || sellOrderId_ != 0) {
            return;
        }

        // Define the contract for MES futures
        Contract contract;
        contract.symbol = "MES";
        contract.secType = "FUT";
        contract.exchange = "GLOBEX";
        contract.currency = "USD";
        contract.lastTradeDateOrContractMonth = "202503"; // Example: June 2024

        // Create Buy Order (Bid)
        Order buyOrder;
        buyOrder.action = "BUY";
        buyOrder.orderType = "LMT";
        buyOrder.totalQuantity = 1;
        buyOrder.lmtPrice = bidPrice_ - 0.5; // Adjust as needed

        // Create Sell Order (Ask)
        Order sellOrder;
        sellOrder.action = "SELL";
        sellOrder.orderType = "LMT";
        sellOrder.totalQuantity = 1;
        sellOrder.lmtPrice = askPrice_ + 0.5; // Adjust as needed

        // Place Buy Order
        buyOrderId_ = nextOrderId_++;
        client_.placeOrder(buyOrderId_, contract, buyOrder);
        std::cout << "Placed Buy Order ID: " << buyOrderId_ << " at Price: " << buyOrder.lmtPrice << std::endl;

        // Place Sell Order
        sellOrderId_ = nextOrderId_++;
        client_.placeOrder(sellOrderId_, contract, sellOrder);
        std::cout << "Placed Sell Order ID: " << sellOrderId_ << " at Price: " << sellOrder.lmtPrice << std::endl;
    }

private:
    EClientSocket client_;
    std::atomic<OrderId> nextOrderId_;
    std::atomic<bool> connected_;

    double bidPrice_ = 0.0;
    double askPrice_ = 0.0;
    std::mutex marketDataMutex_;

    OrderId buyOrderId_ = 0;
    OrderId sellOrderId_ = 0;
    std::mutex orderMutex_;

    std::mutex connectionMutex_;
    std::condition_variable connectionCV_;

public:
    EClientSocket& getClient() { return client_; }

    void waitForConnection() {
        std::unique_lock<std::mutex> lock(connectionMutex_);
        connectionCV_.wait(lock, [&]() { return connected_; });
    }
};

Contract createMESContract() {
    Contract contract;
    contract.symbol = "MES";
    contract.secType = "FUT";
    contract.exchange = "GLOBEX";
    contract.currency = "USD";
    contract.lastTradeDateOrContractMonth = "202503"; // March 2025
    return contract;
}

int main() {
    MarketMaker marketMaker;

    // Connect to IB API (localhost:7497 is the default TWS gateway port)
    bool connected = marketMaker.getClient().eConnect("127.0.0.1", 7497, 0);
    if (!connected) {
        std::cerr << "Failed to connect to IB API." << std::endl;
        return 1;
    }
    std::cout << "Connected to IB API." << std::endl;

    // Wait until nextValidId is received
    marketMaker.waitForConnection();

    // Define MES contract
    Contract mesContract = createMESContract();

    // Request Market Data (Bid and Ask)
    // The tickerId can be any unique identifier
    int tickerId = 1001;
    marketMaker.getClient().reqMktData(tickerId, mesContract, "151", false, false, TagValueListSPtr());

    // Keep the application running to receive callbacks
    std::cout << "Press Ctrl+C to exit..." << std::endl;
    std::mutex exitMutex;
    std::unique_lock<std::mutex> lock(exitMutex);
    std::condition_variable().wait(lock); // Wait indefinitely

    // Disconnect on exit
    marketMaker.getClient().eDisconnect();
    return 0;
}