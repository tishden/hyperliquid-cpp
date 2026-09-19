#include "hl/om/ExchangeClient.h"

#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>

#include "hl/core/Log.h"

namespace hl {

namespace {

constexpr std::size_t kMaxRememberedTids = 20'000;
constexpr std::int64_t kEvictionPeriodMs = 10'000;
constexpr std::uint64_t kExternalCloidHigh = ~0ULL;

std::unique_ptr<Signer> makeSigner(const ExchangeConfig& config) {
    if (config.privateKey.empty()) {
        throw std::invalid_argument("hl::ExchangeClient: privateKey is required");
    }
    return std::make_unique<Signer>(config.privateKey);
}

std::optional<Address> parseOptionalAddress(const std::string& text, const char* what) {
    if (text.empty()) {
        return std::nullopt;
    }
    auto a = parseAddress(text);
    if (!a) {
        throw std::invalid_argument(std::string{"hl::ExchangeClient: malformed "} + what);
    }
    return a;
}

WsSessionOptions sessionOptions(const ExchangeConfig& config) {
    WsSessionOptions o = config.session;
    o.url = config.wsUrlOverride.empty() ? std::string{wsUrl(config.network)} : config.wsUrlOverride;
    o.ws.tls = config.http.tls;
    return o;
}

std::string restBase(const ExchangeConfig& config) {
    return config.restUrlOverride.empty() ? std::string{restUrl(config.network)} : config.restUrlOverride;
}

bool isTransportFailure(const Error& e) noexcept {
    return e.kind == Error::Kind::Transport || e.kind == Error::Kind::Timeout;
}

}  // namespace

std::string_view toString(OrderState state) noexcept {
    switch (state) {
        case OrderState::PendingNew: return "PendingNew";
        case OrderState::Open: return "Open";
        case OrderState::PartiallyFilled: return "PartiallyFilled";
        case OrderState::Filled: return "Filled";
        case OrderState::Canceled: return "Canceled";
        case OrderState::Rejected: return "Rejected";
    }
    return "Unknown";
}

ExchangeClient::ExchangeClient(EventLoop& loop, ExchangeListener& listener, ExchangeConfig config)
    : loop_(loop),
      listener_(listener),
      config_(std::move(config)),
      signer_(makeSigner(config_)),
      vault_(parseOptionalAddress(config_.vaultAddress, "vaultAddress")),
      builder_(*signer_, config_.network, vault_),
      info_(loop, restBase(config_), config_.http),
      exchangeHttp_(loop, restBase(config_), config_.http),
      session_(loop, *this, sessionOptions(config_)) {
    account_ = parseOptionalAddress(config_.accountAddress, "accountAddress").value_or(signer_->address());
    user_ = vault_.value_or(account_);
    if (config_.precomputedNonces > 0) {
        signer_->enableNoncePool(config_.precomputedNonces, /*backgroundThread=*/true);
    }
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&cloidSession_), sizeof(cloidSession_)) != 1) {
        cloidSession_ = static_cast<std::uint64_t>(EventLoop::wallClockMs());
    }
    if (cloidSession_ == kExternalCloidHigh) {
        cloidSession_ = 1;
    }
}

ExchangeClient::~ExchangeClient() { stop(); }

Cloid ExchangeClient::nextCloid() noexcept { return Cloid{cloidSession_, ++cloidCounter_}; }

// ── lifecycle ───────────────────────────────────────────────────────────────

void ExchangeClient::start() {
    if (started_) {
        return;
    }
    started_ = true;
    logf(LogLevel::Info, "exchange: starting (%s, account %s, signer %s)",
         config_.network == Network::Mainnet ? "mainnet" : "testnet", toHex(account_).c_str(),
         toHex(signer_->address()).c_str());
    bootstrap();
    const std::string user = toHex(user_);
    session_.subscribe(R"({"type":"orderUpdates","user":")" + user + R"("})");
    session_.subscribe(R"({"type":"userFills","user":")" + user + R"("})");
    session_.start();
    armEviction();
}

void ExchangeClient::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    ready_ = false;
    session_.stop();
    if (evictionTimer_ != 0) {
        loop_.cancelTimer(evictionTimer_);
        evictionTimer_ = 0;
    }
    for (auto& [id, p] : pending_) {
        if (p.timer != 0) {
            loop_.cancelTimer(p.timer);
        }
    }
    pending_.clear();
}

void ExchangeClient::bootstrap() {
    info_.assets(config_.loadSpotAssets, [this](const Result<AssetRegistry>& r) {
        if (!r) {
            logf(LogLevel::Error, "exchange: loading asset metadata failed: %s", r.error().message.c_str());
            listener_.onError(r.error());
            loop_.addTimer(2'000, [this, alive = std::weak_ptr<int>(lifetime_)] {
                if (!alive.expired() && started_ && !assetsLoaded_) {
                    bootstrap();
                }
            });
            return;
        }
        assets_ = r.value();
        assetsLoaded_ = true;
        logf(LogLevel::Info, "exchange: %zu assets loaded", assets_.all().size());
        maybeReady();
    });
    // The venue attributes an agent wallet's actions to its master account: order updates, fills and
    // positions belong to the master, not to the agent address. Getting `accountAddress` wrong here
    // silently produces "orders work but nothing is reported", so check it explicitly.
    info_.userRole(signer_->address(), [this](const Result<UserRole>& r) {
        if (!r) {
            logf(LogLevel::Debug, "exchange: userRole query failed (%s)", r.error().message.c_str());
            return;
        }
        if (!r->isAgent()) {
            return;
        }
        if (!r->master.has_value()) {
            logf(LogLevel::Warn, "exchange: signer is an API (agent) wallet but the venue did not report its master");
            return;
        }
        const Address master = *r->master;
        if (master == user_) {
            return;
        }
        if (config_.accountAddress.empty() && !vault_.has_value()) {
            // Not configured: adopt the master the venue reports, so streams and positions match the orders.
            logf(LogLevel::Info, "exchange: signer is an agent wallet of %s — using it as the account",
                 toHex(master).c_str());
            account_ = master;
            user_ = master;
            resubscribeUser();
            return;
        }
        logf(LogLevel::Error,
             "exchange: signer %s is an agent wallet of account %s, but this client is configured for %s — "
             "orders will be placed on %s while order updates, fills and positions are read from %s",
             toHex(signer_->address()).c_str(), toHex(master).c_str(), toHex(user_).c_str(), toHex(master).c_str(),
             toHex(user_).c_str());
        listener_.onError(Error{Error::Kind::Rejected, 0,
                                "accountAddress " + toHex(user_) + " does not match the agent's master account " +
                                    toHex(master)});
    });
    if (positionsLoaded_) {
        return;
    }
    info_.clearinghouseState(user_, [this](const Result<AccountState>& r) {
        if (!r) {
            logf(LogLevel::Warn, "exchange: clearinghouseState failed (%s); positions start from fills only",
                 r.error().message.c_str());
        } else {
            for (const auto& p : r.value().positions) {
                positions_.try_emplace(p.coin, p.szi);
            }
            logf(LogLevel::Info, "exchange: account value %s USDC, %zu open positions",
                 r.value().accountValue.toString().c_str(), r.value().positions.size());
        }
        positionsLoaded_ = true;
        maybeReady();
    });
}

void ExchangeClient::resubscribeUser() {
    const std::string user = toHex(user_);
    for (const auto& sub : session_.subscriptions()) {
        session_.unsubscribe(sub);
    }
    orderUpdatesAcked_ = false;
    userFillsAcked_ = false;
    ready_ = false;
    session_.subscribe(R"({"type":"orderUpdates","user":")" + user + R"("})");
    session_.subscribe(R"({"type":"userFills","user":")" + user + R"("})");
    positions_.clear();
    fillsSnapshotSeen_ = false;
    positionsLoaded_ = false;
    info_.clearinghouseState(user_, [this](const Result<AccountState>& r) {
        if (r) {
            for (const auto& p : r.value().positions) {
                positions_.try_emplace(p.coin, p.szi);
            }
            logf(LogLevel::Info, "exchange: account value %s USDC, %zu open positions",
                 r.value().accountValue.toString().c_str(), r.value().positions.size());
        }
        positionsLoaded_ = true;
        maybeReady();
    });
}

void ExchangeClient::maybeReady() {
    if (ready_ || !started_ || !assetsLoaded_ || !positionsLoaded_ || !session_.isOpen() || !orderUpdatesAcked_ ||
        !userFillsAcked_) {
        return;
    }
    ready_ = true;
    if (wasReadyBefore_) {
        reconcileAll();
    }
    wasReadyBefore_ = true;
    logf(LogLevel::Info, "exchange: ready");
    listener_.onReady();
}

void ExchangeClient::onSessionOpen() {
    orderUpdatesAcked_ = false;
    userFillsAcked_ = false;
}

void ExchangeClient::onSessionMessage(std::string_view message) { parser_.parse(message, *this); }

void ExchangeClient::onSessionClosed(std::string_view reason) {
    ready_ = false;
    orderUpdatesAcked_ = false;
    userFillsAcked_ = false;
    // Actions posted over the dropped socket have an unknown outcome.
    std::vector<std::uint64_t> lost;
    for (const auto& [id, p] : pending_) {
        if (p.viaWebSocket) {
            lost.push_back(id);
        }
    }
    for (auto id : lost) {
        completeAction(id, Error{Error::Kind::Transport, 0, "WebSocket closed: " + std::string{reason}});
    }
    listener_.onDisconnected(reason);
}

void ExchangeClient::onSubscriptionResponse(const SubscriptionResponseMsg& msg) {
    if (msg.subscriptionType == "orderUpdates") {
        orderUpdatesAcked_ = true;
    } else if (msg.subscriptionType == "userFills") {
        userFillsAcked_ = true;
    }
    maybeReady();
}

void ExchangeClient::onVenueError(std::string_view text) {
    logf(LogLevel::Warn, "exchange: venue error frame: %.*s", static_cast<int>(text.size()), text.data());
    listener_.onError(Error{Error::Kind::Venue, 0, std::string{text}});
}

// ── order entry ─────────────────────────────────────────────────────────────

Error ExchangeClient::checkReady() const {
    if (!started_) {
        return Error{Error::Kind::Rejected, 0, "client not started"};
    }
    if (!assetsLoaded_) {
        return Error{Error::Kind::Rejected, 0, "asset metadata not loaded yet"};
    }
    return {};
}

Result<OrderWire> ExchangeClient::buildWire(const OrderRequest& req, Cloid cloid) const {
    const AssetInfo* asset = assets_.find(req.coin);
    if (asset == nullptr) {
        return Error{Error::Kind::Rejected, 0, "unknown coin '" + req.coin + "'"};
    }
    if (asset->isDelisted) {
        return Error{Error::Kind::Rejected, 0, req.coin + " is delisted"};
    }
    if (req.px.raw() <= 0 || req.sz.raw() <= 0) {
        return Error{Error::Kind::Rejected, 0, "price and size must be positive"};
    }
    if (!asset->isValidPx(req.px)) {
        return Error{Error::Kind::Rejected, 0,
                     "invalid price " + req.px.toString() + " for " + req.coin + " (nearest valid " +
                         asset->roundPx(req.px).toString() + ")"};
    }
    if (!asset->isValidSz(req.sz)) {
        return Error{Error::Kind::Rejected, 0,
                     "invalid size " + req.sz.toString() + " for " + req.coin + " (szDecimals " +
                         std::to_string(asset->szDecimals) + ")"};
    }
    if (req.trigger && !asset->isValidPx(req.trigger->triggerPx)) {
        return Error{Error::Kind::Rejected, 0, "invalid trigger price " + req.trigger->triggerPx.toString()};
    }
    OrderWire w;
    w.asset = asset->asset;
    w.isBuy = req.side == Side::Buy;
    w.px = req.px;
    w.sz = req.sz;
    w.reduceOnly = req.reduceOnly;
    w.tif = req.tif;
    w.trigger = req.trigger;
    w.cloid = cloid;
    return w;
}

Result<Cloid> ExchangeClient::placeOrder(const OrderRequest& request) {
    auto r = placeOrders(std::span<const OrderRequest>{&request, 1});
    if (!r) {
        return r.error();
    }
    return r.value().front();
}

Result<std::vector<Cloid>> ExchangeClient::placeOrders(std::span<const OrderRequest> requests) {
    if (Error e = checkReady()) {
        return e;
    }
    if (requests.empty()) {
        return Error{Error::Kind::Rejected, 0, "no orders"};
    }
    std::vector<OrderWire> wires;
    std::vector<Cloid> cloids;
    wires.reserve(requests.size());
    cloids.reserve(requests.size());
    for (const auto& req : requests) {
        const Cloid cloid = req.cloid.value_or(nextCloid());
        if (orders_.count(cloid) != 0) {
            return Error{Error::Kind::Rejected, 0, "duplicate cloid " + cloid.toString()};
        }
        auto wire = buildWire(req, cloid);
        if (!wire) {
            return wire.error();
        }
        wires.push_back(wire.value());
        cloids.push_back(cloid);
    }
    const std::int64_t now = EventLoop::wallClockMs();
    for (std::size_t i = 0; i < requests.size(); ++i) {
        Tracked& t = track(cloids[i]);
        Order& o = t.order;
        o.coin = requests[i].coin;
        o.asset = wires[i].asset;
        o.side = requests[i].side;
        o.px = requests[i].px;
        o.origSz = requests[i].sz;
        o.tif = requests[i].tif;
        o.reduceOnly = requests[i].reduceOnly;
        o.isTrigger = requests[i].trigger.has_value();
        o.state = OrderState::PendingNew;
        o.createdMs = now;
        o.updatedMs = now;
    }
    PendingAction pending;
    pending.kind = ActionKind::Order;
    pending.cloids = cloids;
    send(actions::order(wires), std::move(pending));
    return cloids;
}

Error ExchangeClient::cancel(const Cloid& cloid) {
    if (Error e = checkReady()) {
        return e;
    }
    Tracked* t = find(cloid);
    if (t == nullptr) {
        return Error{Error::Kind::Rejected, 0, "unknown order " + cloid.toString()};
    }
    if (!t->order.isLive()) {
        return Error{Error::Kind::Rejected, 0, "order is not live (" + std::string{toString(t->order.state)} + ")"};
    }
    if (t->order.cancelPending) {
        return Error{Error::Kind::Rejected, 0, "cancel already pending"};
    }
    PendingAction pending;
    pending.kind = ActionKind::Cancel;
    pending.cloids = {cloid};
    t->order.cancelPending = true;
    if (t->order.external) {
        if (t->order.oid == 0) {
            t->order.cancelPending = false;
            return Error{Error::Kind::Rejected, 0, "external order without oid"};
        }
        const CancelWire w{t->order.asset, t->order.oid};
        send(actions::cancel(std::span<const CancelWire>{&w, 1}), std::move(pending));
    } else {
        const CancelByCloidWire w{t->order.asset, cloid};
        send(actions::cancelByCloid(std::span<const CancelByCloidWire>{&w, 1}), std::move(pending));
    }
    return {};
}

Error ExchangeClient::cancelByOid(std::string_view coin, std::uint64_t oid) {
    if (Error e = checkReady()) {
        return e;
    }
    const AssetInfo* asset = assets_.find(coin);
    if (asset == nullptr) {
        return Error{Error::Kind::Rejected, 0, "unknown coin '" + std::string{coin} + "'"};
    }
    PendingAction pending;
    pending.kind = ActionKind::Cancel;
    if (Tracked* t = findByOid(oid)) {
        t->order.cancelPending = true;
        pending.cloids = {t->order.cloid};
    } else {
        pending.cloids = {Cloid{}};
    }
    const CancelWire w{asset->asset, oid};
    send(actions::cancel(std::span<const CancelWire>{&w, 1}), std::move(pending));
    return {};
}

Error ExchangeClient::cancelAll(std::string_view coin) {
    if (Error e = checkReady()) {
        return e;
    }
    std::vector<CancelByCloidWire> byCloid;
    std::vector<CancelWire> byOid;
    PendingAction cloidPending;
    PendingAction oidPending;
    cloidPending.kind = ActionKind::Cancel;
    oidPending.kind = ActionKind::Cancel;
    for (auto& [cloid, t] : orders_) {
        Order& o = t.order;
        if (!o.isLive() || o.cancelPending || (!coin.empty() && o.coin != coin)) {
            continue;
        }
        if (o.external) {
            if (o.oid == 0) {
                continue;
            }
            byOid.push_back(CancelWire{o.asset, o.oid});
            oidPending.cloids.push_back(cloid);
        } else {
            byCloid.push_back(CancelByCloidWire{o.asset, cloid});
            cloidPending.cloids.push_back(cloid);
        }
        o.cancelPending = true;
    }
    if (!byCloid.empty()) {
        send(actions::cancelByCloid(byCloid), std::move(cloidPending));
    }
    if (!byOid.empty()) {
        send(actions::cancel(byOid), std::move(oidPending));
    }
    return {};
}

Error ExchangeClient::modify(const Cloid& cloid, Decimal newPx, Decimal newSz) {
    if (Error e = checkReady()) {
        return e;
    }
    Tracked* t = find(cloid);
    if (t == nullptr) {
        return Error{Error::Kind::Rejected, 0, "unknown order " + cloid.toString()};
    }
    Order& o = t->order;
    if (!o.isLive() || o.cancelPending) {
        return Error{Error::Kind::Rejected, 0, "order is not live or being canceled"};
    }
    if (o.oid == 0) {
        return Error{Error::Kind::Rejected, 0, "order not acknowledged yet"};
    }
    if (o.modifyPending) {
        return Error{Error::Kind::Rejected, 0, "modify already pending"};
    }
    OrderRequest req;
    req.coin = o.coin;
    req.side = o.side;
    req.px = newPx;
    req.sz = newSz;
    req.tif = o.tif;
    req.reduceOnly = o.reduceOnly;
    auto wire = buildWire(req, cloid);
    if (!wire) {
        return wire.error();
    }
    ModifyWire m;
    m.target = o.oid;
    m.order = wire.value();
    if (o.external) {
        m.order.cloid.reset();
    }
    PendingAction pending;
    pending.kind = ActionKind::Modify;
    pending.cloids = {cloid};
    pending.modifyTargets = {{newPx, newSz}};
    o.modifyPending = true;
    t->canceledDuringModify = false;
    send(actions::batchModify(std::span<const ModifyWire>{&m, 1}), std::move(pending));
    return {};
}

Error ExchangeClient::scheduleCancel(std::optional<std::int64_t> timeMs, ActionCallback callback) {
    if (!started_) {
        return Error{Error::Kind::Rejected, 0, "client not started"};
    }
    PendingAction pending;
    pending.callback = std::move(callback);
    std::optional<std::uint64_t> t;
    if (timeMs) {
        t = static_cast<std::uint64_t>(*timeMs);
    }
    send(actions::scheduleCancel(t), std::move(pending));
    return {};
}

Error ExchangeClient::updateLeverage(std::string_view coin, std::uint32_t leverage, bool isCross,
                                     ActionCallback callback) {
    if (Error e = checkReady()) {
        return e;
    }
    const AssetInfo* asset = assets_.find(coin);
    if (asset == nullptr || asset->kind != AssetInfo::Kind::Perp) {
        return Error{Error::Kind::Rejected, 0, "unknown perp '" + std::string{coin} + "'"};
    }
    if (leverage == 0 || (asset->maxLeverage != 0 && leverage > asset->maxLeverage)) {
        return Error{Error::Kind::Rejected, 0, "leverage out of range (max " + std::to_string(asset->maxLeverage) + ")"};
    }
    PendingAction pending;
    pending.callback = std::move(callback);
    send(actions::updateLeverage(asset->asset, isCross, leverage), std::move(pending));
    return {};
}

void ExchangeClient::submitAction(const EncodedAction& action, ActionCallback callback) {
    PendingAction pending;
    pending.callback = std::move(callback);
    send(action, std::move(pending));
}

// ── transport ───────────────────────────────────────────────────────────────

void ExchangeClient::send(const EncodedAction& action, PendingAction pending) {
    const std::uint64_t id = nextRequestId_++;
    ++stats_.actionsSent;
    if (config_.transport == ActionTransport::WebSocket && session_.isOpen()) {
        const std::string frame = builder_.wsPostAction(id, action, nonces_.next());
        pending.viaWebSocket = true;
        pending.timer = loop_.addTimer(config_.requestTimeoutMs, [this, id] {
            if (auto it = pending_.find(id); it != pending_.end()) {
                it->second.timer = 0;
            }
            completeAction(id, Error{Error::Kind::Timeout, 0, "no response to WebSocket post"});
        });
        pending_.emplace(id, std::move(pending));
        session_.send(frame);
        return;
    }
    const std::string payload = builder_.payload(action, nonces_.next());
    ++stats_.actionsViaHttp;
    pending_.emplace(id, std::move(pending));
    exchangeHttp_.postJson("/exchange", payload, [this, id](const Error& err, const HttpResponse& resp) {
        if (err && resp.body.empty()) {
            completeAction(id, err);
            return;
        }
        auto parsed = parseExchangeResponse(resp.body);
        if (!parsed && err) {
            Error e = err;
            e.message += ": " + resp.body.substr(0, 300);
            completeAction(id, e);
            return;
        }
        completeAction(id, parsed);
    });
}

void ExchangeClient::onPostResponse(const PostResponseMsg& msg) {
    switch (msg.type) {
        case PostResponseMsg::Type::Action:
            completeAction(msg.id, parseExchangeResponse(msg.payload));
            break;
        case PostResponseMsg::Type::Error:
            completeAction(msg.id, Error{Error::Kind::Venue, 0, std::string{msg.payload}});
            break;
        case PostResponseMsg::Type::Info:
            break;
    }
}

void ExchangeClient::completeAction(std::uint64_t requestId, const Result<ExchangeResponse>& result) {
    auto it = pending_.find(requestId);
    if (it == pending_.end()) {
        return;  // late response after timeout / disconnect — reconciliation covers it
    }
    PendingAction pending = std::move(it->second);
    pending_.erase(it);
    if (pending.timer != 0) {
        loop_.cancelTimer(pending.timer);
    }
    if (!result) {
        ++stats_.actionErrors;
        if (result.error().kind == Error::Kind::Timeout) {
            ++stats_.timeouts;
        }
        logf(LogLevel::Warn, "exchange: action failed (%s): %s", std::string{toString(result.error().kind)}.c_str(),
             result.error().message.c_str());
    }
    applyActionResult(pending, result);
    if (pending.callback) {
        pending.callback(result);
    }
}

// ── state machine ───────────────────────────────────────────────────────────

void ExchangeClient::applyActionResult(const PendingAction& pending, const Result<ExchangeResponse>& result) {
    for (std::size_t i = 0; i < pending.cloids.size(); ++i) {
        const Cloid cloid = pending.cloids[i];
        Tracked* t = find(cloid);
        if (t == nullptr) {
            continue;
        }
        Order& o = t->order;
        if (!result) {
            const Error& err = result.error();
            o.lastError = err.message;
            switch (pending.kind) {
                case ActionKind::Order:
                    if (isTransportFailure(err)) {
                        reconcile(cloid);
                    } else if (o.state == OrderState::PendingNew) {
                        o.state = OrderState::Rejected;
                    }
                    break;
                case ActionKind::Cancel:
                    o.cancelPending = false;
                    if (isTransportFailure(err)) {
                        reconcile(cloid);
                    }
                    break;
                case ActionKind::Modify:
                    o.modifyPending = false;
                    if (isTransportFailure(err) || t->canceledDuringModify) {
                        reconcile(cloid);
                    }
                    break;
                case ActionKind::Other:
                    break;
            }
            emit(*t);
            continue;
        }
        const auto& statuses = result.value().statuses;
        if (i >= statuses.size()) {
            continue;
        }
        const ActionStatus& st = statuses[i];
        switch (pending.kind) {
            case ActionKind::Order:
                applyAck(*t, st);
                break;
            case ActionKind::Cancel:
                o.cancelPending = false;
                if (st.kind == ActionStatus::Kind::Success) {
                    if (!isTerminal(o.state)) {
                        o.state = OrderState::Canceled;
                    }
                } else {
                    o.lastError = st.error;
                    reconcile(cloid);  // typically "already canceled, or filled"
                }
                break;
            case ActionKind::Modify: {
                o.modifyPending = false;
                if (st.kind == ActionStatus::Kind::Error) {
                    o.lastError = st.error;
                    reconcile(cloid);
                } else {
                    if (i < pending.modifyTargets.size()) {
                        o.px = pending.modifyTargets[i].first;
                        o.origSz = pending.modifyTargets[i].second;
                    }
                    if (st.oid != 0 && st.oid != o.oid) {
                        if (o.oid != 0) {
                            t->retiredOids.push_back(o.oid);
                        }
                        setOid(*t, st.oid);
                    }
                    t->canceledDuringModify = false;
                    applyAck(*t, st);
                    o.lastError.clear();
                }
                break;
            }
            case ActionKind::Other:
                break;
        }
        emit(*t);
    }
}

void ExchangeClient::applyAck(Tracked& t, const ActionStatus& st) {
    Order& o = t.order;
    switch (st.kind) {
        case ActionStatus::Kind::Resting:
            setOid(t, st.oid);
            if (o.state == OrderState::PendingNew) {
                o.state = o.filledSz.isZero() ? OrderState::Open : OrderState::PartiallyFilled;
            }
            break;
        case ActionStatus::Kind::Filled:
            setOid(t, st.oid);
            t.ackFilledSz = st.totalSz;
            t.ackAvgPx = st.avgPx;
            recomputeFills(t);
            o.state = OrderState::Filled;
            break;
        case ActionStatus::Kind::WaitingForFill:
        case ActionStatus::Kind::WaitingForTrigger:
        case ActionStatus::Kind::Success:
            if (o.state == OrderState::PendingNew) {
                o.state = OrderState::Open;
            }
            break;
        case ActionStatus::Kind::Error:
            o.lastError = st.error;
            if (o.state == OrderState::PendingNew) {
                o.state = OrderState::Rejected;
            }
            break;
    }
}

void ExchangeClient::applyVenueStatus(Tracked& t, OrderUpdateStatus status, std::string_view statusText,
                                      Decimal limitPx, Decimal origSz) {
    Order& o = t.order;
    // Terminal states are sticky; the only accepted transition out of one is an upgrade to Filled
    // (e.g. a local Canceled that the venue reports as filled after the cancel raced a fill).
    if (isTerminal(o.state) && status != OrderUpdateStatus::Filled) {
        o.cancelPending = false;
        return;
    }
    switch (status) {
        case OrderUpdateStatus::Open:
        case OrderUpdateStatus::Triggered:
            if (!limitPx.isZero()) {
                o.px = limitPx;
            }
            if (!origSz.isZero()) {
                o.origSz = origSz;
            }
            if (o.state == OrderState::PendingNew || o.state == OrderState::Open) {
                o.state = o.filledSz.isZero() ? OrderState::Open : OrderState::PartiallyFilled;
            }
            break;
        case OrderUpdateStatus::Filled:
            if (t.ackFilledSz < o.origSz) {
                t.ackFilledSz = o.origSz;
            }
            recomputeFills(t);
            o.state = OrderState::Filled;
            o.cancelPending = false;
            break;
        case OrderUpdateStatus::Canceled:
            o.state = OrderState::Canceled;
            o.cancelPending = false;
            if (statusText != "canceled") {
                o.lastError = std::string{statusText};
            }
            break;
        case OrderUpdateStatus::Rejected:
            o.state = OrderState::Rejected;
            o.lastError = std::string{statusText};
            break;
        case OrderUpdateStatus::Unknown:
            o.lastError = std::string{statusText};
            break;
    }
}

void ExchangeClient::recomputeFills(Tracked& t) noexcept {
    Order& o = t.order;
    o.filledSz = std::max(t.fillSum, t.ackFilledSz);
    if (!t.fillSum.isZero()) {
        o.avgFillPx = Decimal::fromRaw(static_cast<std::int64_t>(t.fillNotional / t.fillSum.raw()));
    } else {
        o.avgFillPx = t.ackAvgPx;
    }
}

void ExchangeClient::setOid(Tracked& t, std::uint64_t oid) {
    if (oid == 0 || t.order.oid == oid) {
        return;
    }
    if (t.order.oid != 0) {
        oidIndex_.erase(t.order.oid);
    }
    t.order.oid = oid;
    oidIndex_[oid] = t.order.cloid;
}

void ExchangeClient::onOrderUpdates(std::span<const OrderUpdateMsg> updates) {
    for (const auto& u : updates) {
        Tracked* t = u.cloid ? find(*u.cloid) : nullptr;
        if (t == nullptr) {
            t = findByOid(u.oid);
        }
        if (t == nullptr) {
            // Order placed outside this client: track it so it can be canceled / observed.
            const Cloid cloid = u.cloid.value_or(Cloid{kExternalCloidHigh, u.oid});
            t = &track(cloid);
            Order& o = t->order;
            o.external = true;
            o.coin = std::string{u.coin};
            if (const AssetInfo* a = assets_.find(u.coin)) {
                o.asset = a->asset;
            }
            o.side = u.side;
            o.px = u.limitPx;
            o.origSz = u.origSz;
            o.createdMs = u.timestampMs;
            setOid(*t, u.oid);
        }
        if (u.oid != 0 && t->order.oid != 0 && u.oid != t->order.oid) {
            const auto& retired = t->retiredOids;
            if (std::find(retired.begin(), retired.end(), u.oid) != retired.end()) {
                continue;  // stale update for an order replaced by modify
            }
            if (t->order.modifyPending) {
                if (u.status != OrderUpdateStatus::Open && u.status != OrderUpdateStatus::Filled &&
                    u.status != OrderUpdateStatus::Triggered) {
                    continue;
                }
                t->retiredOids.push_back(t->order.oid);
            }
            setOid(*t, u.oid);
        } else if (u.oid != 0 && t->order.oid == 0) {
            setOid(*t, u.oid);
        }
        if (t->order.modifyPending && u.oid == t->order.oid && u.status == OrderUpdateStatus::Canceled) {
            t->canceledDuringModify = true;  // resolved by the modify acknowledgement
            continue;
        }
        applyVenueStatus(*t, u.status, u.statusText, u.limitPx, u.origSz);
        emit(*t);
    }
}

void ExchangeClient::onUserFills(const UserFillsMsg& msg) {
    const bool historical = msg.isSnapshot && !fillsSnapshotSeen_;
    if (msg.isSnapshot) {
        fillsSnapshotSeen_ = true;
    }
    for (const auto& f : msg.fills) {
        if (!seenTids_.insert(f.tid).second) {
            continue;
        }
        seenTidOrder_.push_back(f.tid);
        if (seenTidOrder_.size() > kMaxRememberedTids) {
            seenTids_.erase(seenTidOrder_.front());
            seenTidOrder_.pop_front();
        }
        if (historical) {
            continue;  // fills before start-up are already reflected in clearinghouseState
        }
        ++stats_.fills;
        const Fill fill = Fill::from(f);
        positions_[fill.coin] = fill.endPosition();

        Tracked* t = f.cloid ? find(*f.cloid) : nullptr;
        if (t == nullptr) {
            t = findByOid(f.oid);
        }
        listener_.onFill(fill);
        if (t == nullptr) {
            continue;
        }
        t->fillSum += f.sz;
        t->fillNotional += static_cast<Int128>(f.px.raw()) * f.sz.raw();
        recomputeFills(*t);
        Order& o = t->order;
        if (!isTerminal(o.state)) {
            o.state = o.filledSz >= o.origSz ? OrderState::Filled : OrderState::PartiallyFilled;
        }
        emit(*t);
    }
}

// ── reconciliation ──────────────────────────────────────────────────────────

void ExchangeClient::reconcile(const Cloid& cloid) {
    ++stats_.reconciles;
    auto apply = [this, cloid](const Result<OrderStatusInfo>& r) {
        Tracked* t = find(cloid);
        if (t == nullptr) {
            return;
        }
        if (!r) {
            logf(LogLevel::Warn, "exchange: reconcile %s failed: %s", cloid.toString().c_str(),
                 r.error().message.c_str());
            loop_.addTimer(2'000, [this, cloid, alive = std::weak_ptr<int>(lifetime_)] {
                if (alive.expired()) {
                    return;
                }
                if (Tracked* again = find(cloid); again != nullptr && again->order.isLive() && started_) {
                    reconcile(cloid);
                }
            });
            return;
        }
        const OrderStatusInfo& info = r.value();
        Order& o = t->order;
        if (!info.found) {
            if (o.state == OrderState::PendingNew) {
                o.state = OrderState::Rejected;
                if (o.lastError.empty()) {
                    o.lastError = "order not found on venue";
                }
                emit(*t);
            }
            return;
        }
        setOid(*t, info.order.oid);
        applyVenueStatus(*t, info.status, info.statusText, info.order.limitPx, info.order.origSz);
        emit(*t);
    };
    const Order* o = findOrder(cloid);
    if (o != nullptr && o->external) {
        info_.orderStatus(user_, o->oid, apply);
    } else {
        info_.orderStatus(user_, cloid, apply);
    }
}

void ExchangeClient::reconcileAll() {
    std::vector<Cloid> live;
    for (const auto& [cloid, t] : orders_) {
        if (t.order.isLive()) {
            live.push_back(cloid);
        }
    }
    for (const auto& cloid : live) {
        reconcile(cloid);
    }
}

// ── bookkeeping ─────────────────────────────────────────────────────────────

ExchangeClient::Tracked& ExchangeClient::track(const Cloid& cloid) {
    Tracked& t = orders_[cloid];
    t.order.cloid = cloid;
    return t;
}

ExchangeClient::Tracked* ExchangeClient::find(const Cloid& cloid) noexcept {
    auto it = orders_.find(cloid);
    return it == orders_.end() ? nullptr : &it->second;
}

ExchangeClient::Tracked* ExchangeClient::findByOid(std::uint64_t oid) noexcept {
    if (oid == 0) {
        return nullptr;
    }
    auto it = oidIndex_.find(oid);
    return it == oidIndex_.end() ? nullptr : find(it->second);
}

const Order* ExchangeClient::findOrder(const Cloid& cloid) const noexcept {
    auto it = orders_.find(cloid);
    return it == orders_.end() ? nullptr : &it->second.order;
}

std::vector<const Order*> ExchangeClient::liveOrders(std::string_view coin) const {
    std::vector<const Order*> out;
    for (const auto& [cloid, t] : orders_) {
        if (t.order.isLive() && (coin.empty() || t.order.coin == coin)) {
            out.push_back(&t.order);
        }
    }
    return out;
}

Decimal ExchangeClient::position(std::string_view coin) const noexcept {
    for (const auto& [name, pos] : positions_) {
        if (name == coin) {
            return pos;
        }
    }
    return {};
}

void ExchangeClient::emit(Tracked& t) {
    t.order.updatedMs = EventLoop::wallClockMs();
    listener_.onOrderUpdate(t.order);
}

void ExchangeClient::armEviction() {
    evictionTimer_ = loop_.addTimer(kEvictionPeriodMs, [this] {
        evictionTimer_ = 0;
        const std::int64_t cutoff = EventLoop::wallClockMs() - config_.terminalOrderRetentionMs;
        for (auto it = orders_.begin(); it != orders_.end();) {
            const Order& o = it->second.order;
            if (isTerminal(o.state) && o.updatedMs < cutoff) {
                if (o.oid != 0) {
                    oidIndex_.erase(o.oid);
                }
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
        if (started_) {
            armEviction();
        }
    });
}

}  // namespace hl
