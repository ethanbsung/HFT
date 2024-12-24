// HFT.cpp

#include "EWrapper.h"
#include "EClientSocket.h"
#include "Contract.h"
#include "Order.h"
#include "TagValue.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>
#include <set>
#include <map>
using namespace std;

// Define IBString if not defined
#ifndef IBSTRING_DEFINED
typedef std::string IBString;
#define IBSTRING_DEFINED
#endif

// MarketMaker class inheriting from EWrapper
class MarketMaker : public EWrapper {
public:
    MarketMaker() : client_(this), nextOrderId_(0), connected_(false) {}

    // EWrapper interface methods

    // Market Data Handlers
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

    void tickSize(TickerId tickerId, TickType field, Decimal size) override {}
    void tickOptionComputation( TickerId tickerId, TickType tickType, int tickAttrib, double impliedVol, double delta,
	   double optPrice, double pvDividend, double gamma, double vega, double theta, double undPrice) override { }
    void tickGeneric(TickerId tickerId, TickType tickType, double value) override {}
    void tickString(TickerId tickerId, TickType tickType, const std::string& value) override {}
    void tickEFP(TickerId tickerId, TickType tickType, double basisPoints,
        const std::string& formattedBasisPoints, double totalDividends, int holdDays,
        const std::string& futureExpiry, double dividendImpact, double dividendsToExpiry) override {}

    // Order Handlers
    void orderStatus( OrderId orderId, const std::string& status, Decimal filled,
	   Decimal remaining, double avgFillPrice, int permId, int parentId,
	   double lastFillPrice, int clientId, const std::string& whyHeld, double mktCapPrice) override {}
    void openOrder(OrderId orderId, const Contract& contract, const Order& order,
        const OrderState& orderState) override {}
    void openOrderEnd() override {}
    void winError(const IBString& str, int lastError) override {}

    // Account Handlers
    void updateAccountValue(const std::string& key, const std::string& val,
        const std::string& currency, const std::string& accountName) override {}
    void updatePortfolio(const Contract& contract, Decimal position,
        double marketPrice, double marketValue, double averageCost,
        double unrealizedPNL, double realizedPNL, const std::string& accountName) override {}
    void updateAccountTime(const std::string& timeStamp) override {}
    void accountDownloadEnd(const std::string& accountName) override {}
    void nextValidId( OrderId orderId) override {}

    // Contract Handlers
    void contractDetails(int reqId, const ContractDetails& contractDetails) override {}
    void bondContractDetails(int reqId, const ContractDetails& contractDetails) override {}
    void contractDetailsEnd(int reqId) override {}

    // Execution Handlers
    void execDetails(int reqId, const Contract& contract, const Execution& execution) override {}
    void execDetailsEnd(int reqId) override {}

    void error(int id, int errorCode, const std::string& errorString, const std::string& advancedOrderRejectJson) override {}

    // Market Depth Handlers
    void updateMktDepth(TickerId tickerId, int position, int operation,
        int side, double price, Decimal size) override {}
    void updateMktDepthL2(TickerId id, int position, const std::string& marketMaker, int operation,
      int side, double price, Decimal size, bool isSmartDepth) override { }

    // News Handlers
    void updateNewsBulletin(int msgId, int msgType, const std::string& message,
        const std::string& origExchange) override {}

    // Account Management Handlers
    void managedAccounts(const std::string& accountsList) override {}
    void receiveFA(faDataType pFaDataType, const std::string& cxml) override {}

    // Historical Data Handlers
    void historicalData(TickerId reqId, const Bar& bar) override {}
    void historicalDataEnd(int reqId, const std::string& startDateStr, const std::string& endDateStr) override {}

    // Scanner Handlers
    void scannerParameters(const std::string& xml) override {}
    void scannerData(int reqId, int rank, const ContractDetails& contractDetails,
        const std::string& distance, const std::string& benchmark,
        const std::string& projection, const std::string& legsStr) override {}
    void scannerDataEnd(int reqId) override {}

    // Real-time Bars Handler
    void realtimeBar(TickerId reqId, long time, double open, double high, double low, double close,
	   Decimal volume, Decimal wap, int count) override {}

    // Time Handlers
    void currentTime(long time) override {}

    // Fundamental Data Handlers
    void fundamentalData(TickerId reqId, const std::string& data) override {}

    // Delta Neutral Handlers
    void deltaNeutralValidation(int reqId, const DeltaNeutralContract& deltaNeutralContract) override {}

    // Tick Snapshot End Handler
    void tickSnapshotEnd(int reqId) override {}

    // Market Data Type Handler
    void marketDataType(TickerId reqId, int marketDataType) override {}

    // Commission Report Handler
    void commissionReport(const CommissionReport& commissionReport) override {}

    // Position Handlers
    void position(const std::string& account, const Contract& contract, Decimal position,
        double avgCost) override {}
    void positionEnd() override {}

    // Account Summary Handlers
    void accountSummary(int reqId, const std::string& account, const std::string& tag,
        const std::string& value, const std::string& currency) override {}
    void accountSummaryEnd(int reqId) override {}

    // Verification Handlers
    void verifyMessageAPI(const std::string& apiData) override {}
    void verifyCompleted(bool isSuccessful, const std::string& errorText) override {}

    // Display Group Handlers
    void displayGroupList(int reqId, const std::string& groups) override {}
    void displayGroupUpdated(int reqId, const std::string& contractInfo) override {}

    void verifyAndAuthMessageAPI( const std::string& apiData, const std::string& xyzChallange) override { }
    void verifyAndAuthCompleted( bool isSuccessful, const std::string& errorText) override { }

    // Connect Acknowledgment Handler
    void connectAck() override {}

    // Position Multi Handlers
    void positionMulti(int reqId, const std::string& account, const std::string& modelCode,
        const Contract& contract, Decimal pos, double avgCost) override {}
    void positionMultiEnd(int reqId) override {}

    // Account Update Multi Handlers
    void accountUpdateMulti(int reqId, const std::string& account, const std::string& modelCode,
        const std::string& key, const std::string& value, const std::string& currency) override {}
    void accountUpdateMultiEnd(int reqId) override {}

    // Security Definition Optional Parameter Handlers
    void securityDefinitionOptionalParameter(int reqId, const std::string& exchange,
        int underlyingConId, const std::string& tradingClass, const std::string& multiplier,
        const std::set<std::string>& expirations, const std::set<double>& strikes) override {}
    void securityDefinitionOptionalParameterEnd(int reqId) override {}

    // Soft Dollar Tiers Handler
    void softDollarTiers(int reqId, const std::vector<SoftDollarTier>& tiers) override {}

    // Family Codes Handler
    void familyCodes(const std::vector<FamilyCode>& familyCodes) override {}

    // Symbol Samples Handler
    void symbolSamples(int reqId, const std::vector<ContractDescription>& contractDescriptions) override {}

    // Market Depth Exchanges Handler
    void mktDepthExchanges(const std::vector<DepthMktDataDescription>& depthMktDataDescriptions) override {}

    // Tick News Handler
    void tickNews(int tickerId, long timeStamp, const std::string& providerCode,
        const std::string& articleId, const std::string& headline, const std::string& extraData) override {}

    // Smart Components Handler
    void smartComponents(int reqId, const SmartComponentsMap& theMap) override {}

    // Tick Request Parameters Handler
    void tickReqParams(int tickerId, double minTick, const std::string& bboExchange,
        int snapshotPermissions) override {}

    // News Providers Handler
    void newsProviders(const std::vector<NewsProvider>& newsProviders) override {}

    // News Article Handler
    void newsArticle(int reqId, int articleType, const std::string& articleText) override {}

    // Historical News Handlers
    void historicalNews(int reqId, const std::string& time, const std::string& providerCode,
        const std::string& articleId, const std::string& headline) override {}
    void historicalNewsEnd(int reqId, bool hasMore) override {}

    // Head Timestamp Handler
    void headTimestamp(int reqId, const std::string& headTimestamp) override {}

    // Histogram Data Handler
    void histogramData(int reqId, const std::vector<HistogramEntry>& items) override {}

    // Historical Data Update Handler
    void historicalDataUpdate(TickerId reqId, const Bar& bar) override {}

    // Reroute Market Data Request Handlers
    void rerouteMktDataReq(int reqId, int conid, const std::string& exchange) override {}
    void rerouteMktDepthReq(int reqId, int conid, const std::string& exchange) override {}

    // Market Rule Handler
    void marketRule(int marketRuleId, const std::vector<PriceIncrement>& priceIncrements) override {}

    // PnL Handlers
    void pnl(int reqId, double dailyPnL, double unrealizedPnL, double realizedPnL) override {}
    void pnlSingle(int reqId, Decimal pos, double dailyPnL, double unrealizedPnL, double realizedPnL, double value) override {}

    // Historical Ticks Handlers
    void historicalTicks(int reqId, const std::vector<HistoricalTick>& ticks, bool done) override {}
    void historicalTicksBidAsk(int reqId, const std::vector<HistoricalTickBidAsk>& ticks, bool done) override {}
    void historicalTicksLast(int reqId, const std::vector<HistoricalTickLast>& ticks, bool done) override {}

    // Tick By Tick Handlers
    void tickByTickAllLast(int reqId, int tickType, time_t time, double price, Decimal size,
        const TickAttribLast& attribs, const std::string& exchange, const std::string& specialConditions) override {}
    void tickByTickBidAsk(int reqId, time_t time, double bidPrice, double askPrice,
        Decimal bidSize, Decimal askSize, const TickAttribBidAsk& attribs) override {}
    void tickByTickMidPoint(int reqId, time_t time, double midPoint) override {}

    // Order Bound Handler
    void orderBound(long long orderId, int apiClientId, int apiOrderId) override {}

    // Completed Orders Handlers
    void completedOrder(const Contract& contract, const Order& order, const OrderState& orderState) override {}
    void completedOrdersEnd() override {}

    // Replace FA End Handler
    void replaceFAEnd(int reqId, const std::string& faData) override {}

    // WebSocket Handlers
    void wshMetaData(int reqId, const std::string& dataJson) override {}
    void wshEventData(int reqId, const std::string& dataJson) override {}

    // Historical Schedule Handler
    void historicalSchedule(int reqId, const std::string& startDateTime, const std::string& endDateTime, const std::string& timeZone, const std::vector<HistoricalSession>& sessions) override {}

    // User Info Handler
    void userInfo(int reqId, const std::string& info) override {}

    // Connection Closed Handler
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

    // Accessor for EClientSocket
    EClientSocket& getClient() { return client_; }

    // Wait for connection to be established
    void waitForConnection() {
        std::unique_lock<std::mutex> lock(connectionMutex_);
        connectionCV_.wait(lock, [&]() { return connected_.load(); });
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
};

// Function to create MES Contract
Contract createMESContract() {
    Contract contract;
    contract.symbol = "MES";
    contract.secType = "FUT";
    contract.exchange = "CME";
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
    const TagValueListSPtr tagValues = std::make_shared<std::vector<TagValueSPtr>>();
    marketMaker.getClient().reqMktData(tickerId, mesContract, "151", false, false, tagValues);

    // Keep the application running to receive callbacks
    std::cout << "Press Ctrl+C to exit..." << std::endl;
    std::mutex exitMutex;
    std::unique_lock<std::mutex> lock(exitMutex);
    std::condition_variable().wait(lock); // Wait indefinitely

    // Disconnect on exit
    marketMaker.getClient().eDisconnect();
    return 0;
}