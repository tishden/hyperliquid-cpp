// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/md/MarketDataClient.h"

#include <algorithm>

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

std::string MarketDataClient::l2BookSubscriptionJson(std::string_view coin, L2BookOptions options) {
    std::string json = subscriptionJson("l2Book", coin);
    if (!options.nSigFigs && !options.fast) {
        return json;
    }
    json.pop_back();
    if (options.nSigFigs) {
        json += R"(,"nSigFigs":)" + std::to_string(*options.nSigFigs);
        if (options.mantissa) {
            json += R"(,"mantissa":)" + std::to_string(*options.mantissa);
        }
    }
    if (options.fast) {
        // Omitted when false: the venue defaults to the 20-level path, and leaving the key out
        // keeps the string identical to what earlier versions sent.
        json += R"(,"fast":true)";
    }
    json += '}';
    return json;
}

void MarketDataClient::subscribeL2Book(std::string_view coin, L2BookOptions options) {
    if (!isValidCoinName(coin)) {
        logf(LogLevel::Error, "md: refusing to subscribe to invalid coin name");
        return;
    }
    std::string json = l2BookSubscriptionJson(coin, options);
    const auto existing = std::find_if(books_.begin(), books_.end(),
                                       [coin](const BookEntry& e) { return e.book->coin() == coin; });
    if (existing == books_.end()) {
        books_.push_back({std::make_unique<OrderBook>(std::string{coin}), json});
    } else if (existing->subscription != json) {
        // Both subscriptions arrive on the same `l2Book` channel and are indistinguishable in the
        // frame, so the shared book would flip between their depths on every snapshot.
        logf(LogLevel::Warn, "md: %.*s already has a different l2Book subscription; the maintained book "
                             "will be fed by both and flip between them",
             static_cast<int>(coin.size()), coin.data());
    }
    session_.subscribe(std::move(json));
}

void MarketDataClient::subscribeBbo(std::string_view coin) { session_.subscribe(subscriptionJson("bbo", coin)); }

void MarketDataClient::subscribeBook(std::string_view coin, L2BookOptions options) {
    subscribeL2Book(coin, options);
    subscribeBbo(coin);
}

void MarketDataClient::subscribeTrades(std::string_view coin) { session_.subscribe(subscriptionJson("trades", coin)); }

void MarketDataClient::subscribeAssetCtx(std::string_view coin) {
    session_.subscribe(subscriptionJson("activeAssetCtx", coin));
}

void MarketDataClient::subscribeAllMids() { session_.subscribe(subscriptionJson("allMids")); }

std::string MarketDataClient::userSubscriptionJson(std::string_view type, const Address& user) {
    std::string json = R"({"type":")";
    json += type;
    json += R"(","user":")";
    json += toHex(user);
    json += R"("})";
    return json;
}

void MarketDataClient::subscribeCandle(std::string_view coin, std::string_view interval) {
    if (!isValidCoinName(coin)) {
        logf(LogLevel::Error, "md: refusing to subscribe to invalid coin name");
        return;
    }
    std::string json = R"({"type":"candle","coin":")";
    json += coin;
    json += R"(","interval":")";
    json += interval;
    json += R"("})";
    session_.subscribe(std::move(json));
}

void MarketDataClient::subscribeUserEvents(const Address& user) {
    session_.subscribe(userSubscriptionJson("userEvents", user));
}

void MarketDataClient::subscribeUserFundings(const Address& user) {
    session_.subscribe(userSubscriptionJson("userFundings", user));
}

void MarketDataClient::subscribeActiveAssetData(const Address& user, std::string_view coin) {
    if (!isValidCoinName(coin)) {
        logf(LogLevel::Error, "md: refusing to subscribe to invalid coin name");
        return;
    }
    std::string json = R"({"type":"activeAssetData","user":")";
    json += toHex(user);
    json += R"(","coin":")";
    json += coin;
    json += R"("})";
    session_.subscribe(std::move(json));
}

void MarketDataClient::subscribeNotifications(const Address& user) {
    session_.subscribe(userSubscriptionJson("notification", user));
}

void MarketDataClient::subscribeRaw(std::string json) { session_.subscribe(std::move(json)); }

void MarketDataClient::unsubscribeRaw(std::string_view json) {
    session_.unsubscribe(json);
    // Drop a book that is no longer fed, so book(coin) cannot return a frozen snapshot. Matched on
    // the exact subscription string, the same rule WsSession::unsubscribe applies.
    std::erase_if(books_, [json](const BookEntry& e) { return e.subscription == json; });
}

const OrderBook* MarketDataClient::book(std::string_view coin) const noexcept {
    for (const auto& e : books_) {
        if (e.book->coin() == coin) {
            return e.book.get();
        }
    }
    return nullptr;
}

OrderBook* MarketDataClient::findBook(std::string_view coin) noexcept {
    for (auto& e : books_) {
        if (e.book->coin() == coin) {
            return e.book.get();
        }
    }
    return nullptr;
}

void MarketDataClient::onSessionOpen() { listener_.onConnected(); }

void MarketDataClient::onSessionMessage(std::string_view message) { parser_.parse(message, *this); }

void MarketDataClient::onSessionClosed(std::string_view reason) {
    for (auto& e : books_) {
        e.book->clear();
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
void MarketDataClient::onCandle(const CandleMsg& msg) { listener_.onCandle(msg); }
void MarketDataClient::onLiquidation(const LiquidationMsg& msg) { listener_.onLiquidation(msg); }
void MarketDataClient::onNonUserCancels(std::span<const NonUserCancelMsg> c) { listener_.onNonUserCancels(c); }
void MarketDataClient::onUserFundings(std::span<const UserFundingMsg> f) { listener_.onUserFundings(f); }
void MarketDataClient::onActiveAssetData(const ActiveAssetDataMsg& msg) { listener_.onActiveAssetData(msg); }
void MarketDataClient::onNotification(const NotificationMsg& msg) { listener_.onNotification(msg); }
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
