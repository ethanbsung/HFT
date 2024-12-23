// MarketMaker.h
#pragma once

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
        }
        else if (field == ASK) {
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

    void connectionClosed() override {
        std::cout << "Connection Closed." << std::endl;
    }

    // Implement all other pure virtual functions from EWrapper with empty bodies
    void tickGeneric(TickerId tickerId, TickType tickType, double value) override {}
    void tickString(TickerId tickerId, TickType tickType, const std::string& value) override {}
    void tickEFP(TickerId tickerId, TickType tickType, double basisPoints,
        const std::string& formattedBasisPoints, double totalDividends, int holdDays,
        const std::string& futureExpiry, double dividendImpact, double dividendsToExpiry) override {}
    void openOrder(OrderId orderId, const Contract& contract, const Order& order,
        const OrderState& orderState) override {}
    void openOrderEnd() override {}
    void updateAccountValue(const std::string& key, const std::string& val,
        const std::string& currency, const std::string& accountName) override {}
    void updateAccountTime(const std::string& timeStamp) override {}
    void accountDownloadEnd(const std::string& accountName) override {}
    void contractDetails(int reqId, const ContractDetails& contractDetails) override {}
    void bondContractDetails(int reqId, const ContractDetails& contractDetails) override {}
    void contractDetailsEnd(int reqId) override {}
    void execDetails(int reqId, const Contract& contract, const Execution& execution) override {}
    void execDetailsEnd(int reqId) override {}
    void updateNewsBulletin(int msgId, int msgType, const std::string& message,
        const std::string& origExchange) override {}
    void managedAccounts(const std::string& accountsList) override {}
    void historicalData(TickerId reqId, const Bar& bar) override {}
    void scannerParameters(const std::string& xml) override {}
    void scannerData(int reqId, int rank, const ContractDetails& contractDetails,
        const std::string& distance, const std::string& benchmark,
        const std::string& projection, const std::string& legsStr) override {}
    void scannerDataEnd(int reqId) override {}
    void currentTime(long time) override {}
    void fundamentalData(TickerId reqId, const std::string& data) override {}
    void deltaNeutralValidation(int reqId, const DeltaNeutralContract& deltaNeutralContract) override {}
    void tickSnapshotEnd(int reqId) override {}
    void marketDataType(TickerId reqId, int marketDataType) override {}
    void commissionReport(const CommissionReport& commissionReport) override {}
    void positionEnd() override {}
    void accountSummary(int reqId, const std::string& account, const std::string& tag,
        const std::string& value, const std::string& currency) override {}
    void accountSummaryEnd(int reqId) override {}
    void verifyMessageAPI(const std::string& apiData) override {}
    void verifyCompleted(bool isSuccessful, const std::string& errorText) override {}
    void displayGroupList(int reqId, const std::string& groups) override {}
    void displayGroupUpdated(int reqId, const std::string& contractInfo) override {}
    void connectAck() override {}
    void positionMultiEnd(int reqId) override {}
    void accountUpdateMulti(int reqId, const std::string& account, const std::string& modelCode,
        const std::string& key, const std::string& value, const std::string& currency) override {}
    void accountUpdateMultiEnd(int reqId) override {}
    void securityDefinitionOptionalParameter(int reqId, const std::string& exchange,
        int underlyingConId, const std::string& tradingClass, const std::string& multiplier,
        const std::set<std::string>& expirations, const std::set<double>& strikes) override {}
    void securityDefinitionOptionalParameterEnd(int reqId) override {}
    void softDollarTiers(int reqId, const std::vector<SoftDollarTier>& tiers) override {}
    void familyCodes(const std::vector<FamilyCode>& familyCodes) override {}
    void symbolSamples(int reqId, const std::vector<ContractDescription>& contractDescriptions) override {}
    void mktDepthExchanges(const std::vector<DepthMktDataDescription>& depthMktDataDescriptions) override {}
    void tickReqParams(int tickerId, double minTick, const std::string& bboExchange,
        int snapshotPermissions) override {}
    void newsProviders(const std::vector<NewsProvider>& newsProviders) override {}
    void newsArticle(int reqId, int articleType, const std::string& articleText) override {}
    void historicalNews(int reqId, const std::string& time, const std::string& providerCode,
        const std::string& articleId, const std::string& headline) override {}
    void historicalNewsEnd(int reqId, bool hasMore) override {}
    void headTimestamp(int reqId, const std::string& headTimestamp) override {}
    void histogramData(int reqId, const std::vector<HistogramEntry>& items) override {}
    void historicalDataUpdate(TickerId reqId, const Bar& bar) override {}
    void rerouteMktDataReq(int reqId, int conid, const std::string& exchange) override {}
    void rerouteMktDepthReq(int reqId, int conid, const std::string& exchange) override {}
    void marketRule(int marketRuleId, const std::vector<PriceIncrement>& priceIncrements) override {}
    void pnl(int reqId, double dailyPnL, double unrealizedPnL, double realizedPnL) override {}
    void historicalTicks(int reqId, const std::vector<HistoricalTick>& ticks, bool done) override {}
    void historicalTicksBidAsk(int reqId, const std::vector<HistoricalTickBidAsk>& ticks, bool done) override {}
    void historicalTicksLast(int reqId, const std::vector<HistoricalTickLast>& ticks, bool done) override {}
    void orderBound(long long orderId, int apiClientId, int apiOrderId) override {}
    void completedOrdersEnd() override {}

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
        contract.lastTradeDateOrContractMonth = "202503"; // March 2025

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
        connectionCV_.wait(lock, [&]() { return connected_.load(); });
    }
};