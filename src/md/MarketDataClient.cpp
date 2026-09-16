#include "hl/md/MarketDataClient.h"

#include "hl/core/Log.h"

namespace hl {

namespace {

WsSessionOptions sessionOptions(const MarketDataConfig& config) {
    WsSessionOptions options = config.session;
    options.url = config.urlOverride.empty() ? std::string{wsUrl(config.network)} : config.urlOverride;
    return options;
}

}  // namespace

MarketDataClient::MarketDataClient(EventLoop& loop, MarketDataListener& listener, MarketDataConfig config)
    : listener_(listener), config_(std::move(config)), session_(loop, *this, sessionOptions(config_)) {}

MarketDataClient::~MarketDataClient() { session_.stop(); }

void MarketDataClient::start() { session_.start(); }

void MarketDataClient::stop() { session_.stop(); }

std::string MarketDataClient::subscriptionJson(std::string_view type, std::string_view coin) {
    std::string json = R"({"type":")";
    json += type;
    json += '"';
    if (!coin.empty()) {
        json += R"(,"coin":")";
        json += coin;
        json += '"';
    }
    json += '}';
    return json;
}

void MarketDataClient::subscribeL2Book(std::string_view coin, L2BookOptions options) {
    std::string json = subscriptionJson("l2Book", coin);
    if (options.nSigFigs) {
        json.pop_back();
        json += R"(,"nSigFigs":)" + std::to_string(*options.nSigFigs);
        if (options.mantissa) {
            json += R"(,"mantissa":)" + std::to_string(*options.mantissa);
        }
        json += '}';
    }
    if (findBook(coin) == nullptr) {
        books_.push_back(std::make_unique<OrderBook>(std::string{coin}));
    }
    session_.subscribe(std::move(json));
}

void MarketDataClient::subscribeBbo(std::string_view coin) { session_.subscribe(subscriptionJson("bbo", coin)); }

void MarketDataClient::subscribeBook(std::string_view coin) {
    subscribeL2Book(coin);
    subscribeBbo(coin);
}

void MarketDataClient::subscribeTrades(std::string_view coin) { session_.subscribe(subscriptionJson("trades", coin)); }

void MarketDataClient::subscribeAssetCtx(std::string_view coin) {
    session_.subscribe(subscriptionJson("activeAssetCtx", coin));
}

void MarketDataClient::subscribeAllMids() { session_.subscribe(subscriptionJson("allMids")); }

void MarketDataClient::subscribeRaw(std::string json) { session_.subscribe(std::move(json)); }

void MarketDataClient::unsubscribeRaw(std::string_view json) { session_.unsubscribe(json); }

const OrderBook* MarketDataClient::book(std::string_view coin) const noexcept {
    for (const auto& b : books_) {
        if (b->coin() == coin) {
            return b.get();
        }
    }
    return nullptr;
}

OrderBook* MarketDataClient::findBook(std::string_view coin) noexcept {
    for (auto& b : books_) {
        if (b->coin() == coin) {
            return b.get();
        }
    }
    return nullptr;
}

void MarketDataClient::onSessionOpen() { listener_.onConnected(); }

void MarketDataClient::onSessionMessage(std::string_view message) { parser_.parse(message, *this); }

void MarketDataClient::onSessionClosed(std::string_view reason) {
    for (auto& b : books_) {
        b->clear();
    }
    listener_.onDisconnected(reason);
}

void MarketDataClient::onL2Book(const L2BookMsg& msg) {
    if (OrderBook* b = findBook(msg.coin)) {
        b->applySnapshot(msg);
        listener_.onL2Book(msg);
        listener_.onBookUpdate(*b, MarketDataListener::BookUpdate::Snapshot);
        return;
    }
    listener_.onL2Book(msg);
}

void MarketDataClient::onBbo(const BboMsg& msg) {
    listener_.onBbo(msg);
    if (!config_.applyBboToBooks) {
        return;
    }
    if (OrderBook* b = findBook(msg.coin); b != nullptr && b->snapshotTimeMs() != 0 && b->applyBbo(msg)) {
        listener_.onBookUpdate(*b, MarketDataListener::BookUpdate::Bbo);
    }
}

void MarketDataClient::onTrades(std::span<const TradeMsg> trades) { listener_.onTrades(trades); }
void MarketDataClient::onAssetCtx(const AssetCtxMsg& msg) { listener_.onAssetCtx(msg); }
void MarketDataClient::onAllMids(const AllMidsMsg& msg) { listener_.onAllMids(msg); }
void MarketDataClient::onOrderUpdates(std::span<const OrderUpdateMsg> updates) { listener_.onOrderUpdates(updates); }
void MarketDataClient::onUserFills(const UserFillsMsg& msg) { listener_.onUserFills(msg); }
void MarketDataClient::onPostResponse(const PostResponseMsg& msg) { listener_.onPostResponse(msg); }
void MarketDataClient::onSubscriptionResponse(const SubscriptionResponseMsg& msg) {
    listener_.onSubscriptionResponse(msg);
}
void MarketDataClient::onPong() { listener_.onPong(); }

void MarketDataClient::onVenueError(std::string_view text) {
    logf(LogLevel::Warn, "md: venue error: %.*s", static_cast<int>(text.size()), text.data());
    listener_.onVenueError(text);
}

void MarketDataClient::onUnhandled(std::string_view channel, std::string_view frame) {
    listener_.onUnhandled(channel, frame);
}

}  // namespace hl
