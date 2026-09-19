// hl_live_check — scripted acceptance run of the order-management contract against a live venue.
//
// Exercises, step by step, what a trading application depends on: readiness, resting orders,
// amendments, cancels, batches, post-only rejection, IOC execution, info queries, the dead-man's
// switch, leverage and reconciliation after a forced reconnect. Each step has a deadline and a
// pass/fail verdict; the process exits non-zero if any step fails.
//
//   hl_live_check --key-file secrets/testnet.env --coin ETH [--notional 12] [--taker] [--mainnet …]
//
// Orders are placed far from the mid (default 2 %) so nothing executes unintentionally; --taker
// additionally sends one small IOC order that is expected to trade.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "hl/hyperliquid.h"

namespace {

struct StepResult {
    std::string name;
    bool passed{false};
    std::string detail;
};

class Runner final : public hl::ExchangeListener, public hl::MarketDataListener {
public:
    Runner(hl::EventLoop& loop, std::string coin, hl::Decimal notional, bool taker)
        : loop_(loop), coin_(std::move(coin)), notional_(notional), taker_(taker) {}

    void attach(hl::ExchangeClient& ex, hl::MarketDataClient& md) {
        ex_ = &ex;
        md_ = &md;
    }

    // ── listeners ───────────────────────────────────────────────────────────
    void onReady() override { ready_ = true; }
    void onDisconnected(std::string_view reason) override {
        ready_ = false;
        ++disconnects_;
        lastDisconnect_ = std::string{reason};
    }
    void onOrderUpdate(const hl::Order& o) override {
        ++updates_;
        if (verbose_) {
            std::printf("      · %s %s oid=%" PRIu64 " px=%s filled=%s %s\n", o.cloid.toString().c_str(),
                        std::string{hl::toString(o.state)}.c_str(), o.oid, o.px.toString().c_str(),
                        o.filledSz.toString().c_str(), o.lastError.c_str());
        }
    }
    void onFill(const hl::Fill& f) override {
        fills_.push_back(f);
        std::printf("      · fill %s %s @ %s (%s, fee %s)\n", std::string{hl::toString(f.side)}.c_str(),
                    f.sz.toString().c_str(), f.px.toString().c_str(), f.crossed ? "taker" : "maker",
                    f.fee.toString().c_str());
    }
    void onError(const hl::Error& e) override {
        std::printf("      · error (%s): %s\n", std::string{hl::toString(e.kind)}.c_str(), e.message.c_str());
    }
    void onBookUpdate(const hl::OrderBook&, BookUpdate) override {}

    // ── harness ─────────────────────────────────────────────────────────────
    bool waitFor(const std::function<bool()>& pred, int timeoutMs = 15'000) {
        const std::int64_t deadline = hl::EventLoop::nowMs() + timeoutMs;
        while (!pred()) {
            if (hl::EventLoop::nowMs() >= deadline) {
                return false;
            }
            loop_.runOnce(20);
        }
        return true;
    }

    void step(const std::string& name, const std::function<bool(std::string&)>& body) {
        std::printf("\n▶ %s\n", name.c_str());
        StepResult r{name, false, {}};
        r.passed = body(r.detail);
        std::printf("   %s%s%s\n", r.passed ? "PASS" : "FAIL", r.detail.empty() ? "" : " — ", r.detail.c_str());
        results_.push_back(std::move(r));
    }

    [[nodiscard]] const std::vector<StepResult>& results() const noexcept { return results_; }
    [[nodiscard]] const std::vector<hl::Fill>& fills() const noexcept { return fills_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] int disconnects() const noexcept { return disconnects_; }
    [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
    void setVerbose(bool v) noexcept { verbose_ = v; }

    // Book-derived helpers.
    [[nodiscard]] const hl::OrderBook* book() const { return md_->book(coin_); }
    [[nodiscard]] const hl::AssetInfo* asset() const { return ex_->assets().find(coin_); }

    /// Price `offsetBps` away from mid on the given side, rounded to a valid price.
    [[nodiscard]] hl::Decimal priceAway(hl::Side side, double offsetBps) const {
        const double mid = book()->mid().toDouble();
        const double px = side == hl::Side::Buy ? mid * (1.0 - offsetBps / 10'000.0) : mid * (1.0 + offsetBps / 10'000.0);
        return asset()->roundPx(hl::Decimal::fromDouble(px),
                                side == hl::Side::Buy ? hl::RoundingMode::Down : hl::RoundingMode::Up);
    }

    [[nodiscard]] hl::Decimal size() const { return asset()->roundSz(notional_.div(book()->mid()), hl::RoundingMode::Up); }

    [[nodiscard]] hl::OrderRequest request(hl::Side side, hl::Decimal px, hl::Tif tif = hl::Tif::Alo) const {
        hl::OrderRequest r;
        r.coin = coin_;
        r.side = side;
        r.px = px;
        r.sz = size();
        r.tif = tif;
        return r;
    }

    [[nodiscard]] bool taker() const noexcept { return taker_; }
    [[nodiscard]] const std::string& coin() const noexcept { return coin_; }
    hl::ExchangeClient& ex() { return *ex_; }
    hl::MarketDataClient& md() { return *md_; }

private:
    hl::EventLoop& loop_;
    std::string coin_;
    hl::Decimal notional_;
    bool taker_{false};
    bool verbose_{false};
    hl::ExchangeClient* ex_{nullptr};
    hl::MarketDataClient* md_{nullptr};
    bool ready_{false};
    int disconnects_{0};
    std::uint64_t updates_{0};
    std::string lastDisconnect_;
    std::vector<hl::Fill> fills_;
    std::vector<StepResult> results_;
};

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

void loadKeyFile(const std::string& path, std::string& key, std::string& account, std::string& vault) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot read key file %s\n", path.c_str());
        std::exit(2);
    }
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) {
            v.pop_back();
        }
        if (k == "HL_PRIVATE_KEY") {
            key = v;
        } else if (k == "HL_ACCOUNT_ADDRESS") {
            account = v;
        } else if (k == "HL_VAULT_ADDRESS") {
            vault = v;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string coin = "ETH";
    std::string key = env("HL_PRIVATE_KEY");
    std::string account = env("HL_ACCOUNT_ADDRESS");
    std::string vault = env("HL_VAULT_ADDRESS");
    hl::Decimal notional = hl::Decimal::fromInt(12);
    double offsetBps = 200.0;
    bool taker = false;
    bool flattenOnly = false;
    bool httpTransport = false;
    bool mainnet = false;
    bool mainnetAck = false;
    bool verbose = false;
    std::size_t presign = 64;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--coin") {
            coin = next();
        } else if (a == "--key-file") {
            loadKeyFile(next(), key, account, vault);
        } else if (a == "--notional") {
            notional = hl::Decimal::parseOrZero(next());
        } else if (a == "--offset-bps") {
            offsetBps = std::stod(next());
        } else if (a == "--flatten") {
            flattenOnly = true;
        } else if (a == "--transport") {
            httpTransport = next() == "http";
        } else if (a == "--taker") {
            taker = true;
        } else if (a == "--presign") {
            presign = static_cast<std::size_t>(std::stoull(next()));
        } else if (a == "--mainnet") {
            mainnet = true;
        } else if (a == "--i-understand-this-trades-real-money") {
            mainnetAck = true;
        } else if (a == "--verbose") {
            verbose = true;
            hl::setLogLevel(hl::LogLevel::Debug);
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: hl_live_check [--coin ETH] [--key-file PATH] [--notional 12] [--offset-bps 200]\n"
                        "                     [--taker] [--transport ws|http] [--presign N] [--verbose]\n"
                        "                     [--flatten]   cancel all orders and close the position, then exit\n"
                        "                     [--mainnet --i-understand-this-trades-real-money]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (mainnet && !mainnetAck) {
        std::fprintf(stderr, "refusing to run on mainnet without --i-understand-this-trades-real-money\n");
        return 2;
    }
    if (key.empty()) {
        std::fprintf(stderr, "no private key: set HL_PRIVATE_KEY or pass --key-file\n");
        return 2;
    }
    const hl::Network network = mainnet ? hl::Network::Mainnet : hl::Network::Testnet;

    hl::EventLoop loop;
    Runner runner(loop, coin, notional, taker);
    runner.setVerbose(verbose);

    hl::MarketDataConfig mdCfg;
    mdCfg.network = network;
    hl::MarketDataClient md(loop, runner, mdCfg);
    md.subscribeBook(coin);

    hl::ExchangeConfig exCfg;
    exCfg.network = network;
    exCfg.privateKey = key;
    exCfg.accountAddress = account;
    exCfg.vaultAddress = vault;
    exCfg.precomputedNonces = presign;
    exCfg.transport = httpTransport ? hl::ActionTransport::Http : hl::ActionTransport::WebSocket;
    std::unique_ptr<hl::ExchangeClient> ex;
    try {
        ex = std::make_unique<hl::ExchangeClient>(loop, runner, exCfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "configuration error: %s\n", e.what());
        return 2;
    }
    runner.attach(*ex, md);

    std::printf("hyperliquid-cpp %s — live acceptance check on %s (%s, %s transport, presign %zu)\n",
                hl::kVersionString, mainnet ? "MAINNET" : "testnet", coin.c_str(), httpTransport ? "HTTP" : "WebSocket",
                presign);
    md.start();
    ex->start();

    // Close any open position of the coin with a reduce-only IOC through the book.
    const auto flatten = [&](std::string& detail) {
        (void)ex->cancelAll();
        runner.waitFor([&] { return ex->liveOrders().empty(); }, 10'000);
        runner.waitFor([] { return false; }, 2'000);  // let late fills land before measuring
        const hl::Decimal pos = ex->position(coin);
        if (pos.isZero()) {
            detail = "flat";
            return true;
        }
        const bool isLong = !pos.isNegative();
        hl::OrderRequest req = runner.request(isLong ? hl::Side::Sell : hl::Side::Buy,
                                              runner.priceAway(isLong ? hl::Side::Sell : hl::Side::Buy, -50.0),
                                              hl::Tif::Ioc);
        req.sz = runner.asset()->roundSz(isLong ? pos : -pos, hl::RoundingMode::Down);
        req.reduceOnly = true;
        auto placed = ex->placeOrder(req);
        if (!placed) {
            detail = "closing " + pos.toString() + " failed: " + placed.error().message;
            return false;
        }
        runner.waitFor([&] {
            const hl::Order* o = ex->findOrder(placed.value());
            return o != nullptr && !o->isLive();
        });
        const bool flat = runner.waitFor([&] { return ex->position(coin).isZero(); }, 15'000);
        detail = "closed " + pos.toString() + " → position " + ex->position(coin).toString();
        return flat;
    };

    if (flattenOnly) {
        std::printf("\n▶ flatten: cancel all orders and close the %s position\n", coin.c_str());
        if (!runner.waitFor([&] { return runner.ready() && runner.book() != nullptr && runner.book()->isValid(); },
                            20'000)) {
            std::printf("   FAIL — not ready\n");
            return 1;
        }
        std::string detail;
        const bool ok = flatten(detail);
        std::printf("   %s — %s\n", ok ? "PASS" : "FAIL", detail.c_str());
        ex->stop();
        md.stop();
        return ok ? 0 : 1;
    }

    // ── 1. readiness ────────────────────────────────────────────────────────
    runner.step("connect, load metadata, subscribe user streams", [&](std::string& detail) {
        if (!runner.waitFor([&] { return runner.ready(); }, 20'000)) {
            detail = "not ready in 20 s";
            return false;
        }
        const hl::AssetInfo* a = runner.asset();
        detail = "account " + hl::toHex(ex->accountAddress()) + ", " + std::to_string(ex->assets().all().size()) +
                 " assets, " + coin + " asset=" + std::to_string(a != nullptr ? a->asset : 0) +
                 " szDecimals=" + std::to_string(a != nullptr ? a->szDecimals : -1);
        return a != nullptr;
    });

    runner.step("market data: order book", [&](std::string& detail) {
        if (!runner.waitFor([&] { return runner.book() != nullptr && runner.book()->isValid(); })) {
            detail = "no valid book";
            return false;
        }
        const hl::OrderBook* b = runner.book();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "bid %s / ask %s, spread %.2f bps, size to use %s", b->bestBid()->px.toString().c_str(),
                      b->bestAsk()->px.toString().c_str(), b->spreadBps(), runner.size().toString().c_str());
        detail = buf;
        return true;
    });

    // ── 2. resting order ────────────────────────────────────────────────────
    hl::Cloid resting;
    runner.step("place post-only order far from mid → Open with oid", [&](std::string& detail) {
        const auto px = runner.priceAway(hl::Side::Buy, offsetBps);
        auto placed = ex->placeOrder(runner.request(hl::Side::Buy, px));
        if (!placed) {
            detail = placed.error().message;
            return false;
        }
        resting = placed.value();
        if (!runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(resting);
                return o != nullptr && o->state != hl::OrderState::PendingNew;
            })) {
            detail = "no acknowledgement";
            return false;
        }
        const hl::Order* o = ex->findOrder(resting);
        detail = "px " + px.toString() + ", state " + std::string{hl::toString(o->state)} + ", oid " +
                 std::to_string(o->oid) + (o->lastError.empty() ? "" : ", " + o->lastError);
        return o->state == hl::OrderState::Open && o->oid != 0;
    });

    // ── 3. the venue agrees it is resting ───────────────────────────────────
    runner.step("info: orderStatus and frontendOpenOrders see the order", [&](std::string& detail) {
        // These endpoints are eventually consistent — poll for a few seconds.
        std::string statusText = "no answer";
        bool statusOk = false;
        const std::int64_t deadline = hl::EventLoop::nowMs() + 15'000;
        while (!statusOk && hl::EventLoop::nowMs() < deadline) {
            bool done = false;
            ex->info().orderStatus(ex->accountAddress(), resting, [&](const hl::Result<hl::OrderStatusInfo>& r) {
                done = true;
                if (!r) {
                    statusText = r.error().message;
                    return;
                }
                statusText = r->found ? "status " + r->statusText + ", oid " + std::to_string(r->order.oid)
                                      : "not found (" + r->statusText + ")";
                statusOk = r->found && r->status == hl::OrderUpdateStatus::Open;
            });
            runner.waitFor([&] { return done; });
            if (!statusOk) {
                runner.waitFor([] { return false; }, 1'000);  // pump the loop for a second
            }
        }
        bool listed = false;
        std::string listText = "no answer";
        const std::int64_t listDeadline = hl::EventLoop::nowMs() + 15'000;
        while (!listed && hl::EventLoop::nowMs() < listDeadline) {
            bool done = false;
            ex->info().openOrders(ex->accountAddress(), [&](const hl::Result<std::vector<hl::OpenOrder>>& r) {
                done = true;
                if (!r) {
                    listText = r.error().message;
                    return;
                }
                for (const auto& o : r.value()) {
                    if (o.cloid == resting) {
                        listed = true;
                        listText = "listed: oid " + std::to_string(o.oid) + " px " + o.limitPx.toString() + " sz " +
                                   o.sz.toString() + " tif " + o.tif;
                    }
                }
                if (!listed) {
                    listText = std::to_string(r.value().size()) + " open orders, ours not yet among them";
                }
            });
            runner.waitFor([&] { return done; });
            if (!listed) {
                runner.waitFor([] { return false; }, 1'000);
            }
        }
        detail = "orderStatus: " + statusText + " | frontendOpenOrders: " + listText;
        return statusOk && listed;
    });

    // ── 4. amend ────────────────────────────────────────────────────────────
    runner.step("modify price and size in place (cloid preserved)", [&](std::string& detail) {
        const hl::Order* before = ex->findOrder(resting);
        const std::uint64_t oldOid = before->oid;
        const auto newPx = runner.priceAway(hl::Side::Buy, offsetBps * 1.5);
        const auto newSz = runner.asset()->roundSz(runner.size() + runner.size(), hl::RoundingMode::Up);
        if (hl::Error e = ex->modify(resting, newPx, newSz)) {
            detail = e.message;
            return false;
        }
        if (!runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(resting);
                return o != nullptr && !o->modifyPending;
            })) {
            detail = "no modify acknowledgement";
            return false;
        }
        const hl::Order* o = ex->findOrder(resting);
        detail = "px " + o->px.toString() + ", sz " + o->origSz.toString() + ", oid " + std::to_string(oldOid) + " → " +
                 std::to_string(o->oid) + ", state " + std::string{hl::toString(o->state)};
        return o->state == hl::OrderState::Open && o->px == newPx && o->origSz == newSz;
    });

    // ── 5. cancel ───────────────────────────────────────────────────────────
    runner.step("cancel by cloid → Canceled", [&](std::string& detail) {
        if (hl::Error e = ex->cancel(resting)) {
            detail = e.message;
            return false;
        }
        if (!runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(resting);
                return o != nullptr && !o->isLive();
            })) {
            detail = "still live";
            return false;
        }
        const hl::Order* o = ex->findOrder(resting);
        detail = "state " + std::string{hl::toString(o->state)};
        return o->state == hl::OrderState::Canceled;
    });

    // ── 6. batch + cancelAll ────────────────────────────────────────────────
    runner.step("batch of 2 orders in one action, then cancelAll", [&](std::string& detail) {
        std::vector<hl::OrderRequest> batch{runner.request(hl::Side::Buy, runner.priceAway(hl::Side::Buy, offsetBps)),
                                            runner.request(hl::Side::Sell, runner.priceAway(hl::Side::Sell, offsetBps))};
        auto placed = ex->placeOrders(batch);
        if (!placed) {
            detail = placed.error().message;
            return false;
        }
        if (!runner.waitFor([&] {
                for (const auto& c : placed.value()) {
                    const hl::Order* o = ex->findOrder(c);
                    if (o == nullptr || o->state == hl::OrderState::PendingNew) {
                        return false;
                    }
                }
                return true;
            })) {
            detail = "no acknowledgements";
            return false;
        }
        std::string states;
        bool bothOpen = true;
        for (const auto& c : placed.value()) {
            const hl::Order* o = ex->findOrder(c);
            states += std::string{hl::toString(o->state)} + " ";
            bothOpen &= o->state == hl::OrderState::Open;
        }
        if (hl::Error e = ex->cancelAll(coin)) {
            detail = "cancelAll: " + e.message;
            return false;
        }
        const bool cleared = runner.waitFor([&] { return ex->liveOrders(coin).empty(); });
        detail = "states " + states + "→ cancelAll " + (cleared ? "cleared all" : "left orders live");
        return bothOpen && cleared;
    });

    // ── 7. post-only rejection ──────────────────────────────────────────────
    runner.step("post-only order that crosses → rejected by the venue", [&](std::string& detail) {
        // 50 bps through the ask, so it crosses even if the book moves between decision and arrival.
        const auto px = runner.priceAway(hl::Side::Buy, -50.0);
        auto placed = ex->placeOrder(runner.request(hl::Side::Buy, px));
        if (!placed) {
            detail = placed.error().message;
            return false;
        }
        if (!runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(placed.value());
                return o != nullptr && o->state != hl::OrderState::PendingNew;
            })) {
            detail = "no response";
            return false;
        }
        const hl::Order* o = ex->findOrder(placed.value());
        detail = std::string{hl::toString(o->state)} + ": " + o->lastError;
        if (o->isLive()) {
            (void)ex->cancel(o->cloid);
            runner.waitFor([&] { return ex->liveOrders(coin).empty(); }, 5'000);
            return false;  // a crossing ALO must not rest
        }
        return o->state == hl::OrderState::Rejected;
    });

    // ── 8. local validation ─────────────────────────────────────────────────
    runner.step("local validation rejects invalid price/size/coin before signing", [&](std::string& detail) {
        auto badCoin = runner.request(hl::Side::Buy, runner.priceAway(hl::Side::Buy, offsetBps));
        badCoin.coin = "NOSUCHCOIN";
        auto badPx = runner.request(hl::Side::Buy, hl::Decimal::parseOrZero("1234.56789"));
        auto badSz = runner.request(hl::Side::Buy, runner.priceAway(hl::Side::Buy, offsetBps));
        badSz.sz = hl::Decimal::parseOrZero("0.000000001");
        const auto r1 = ex->placeOrder(badCoin);
        const auto r2 = ex->placeOrder(badPx);
        const auto r3 = ex->placeOrder(badSz);
        detail = (r1 ? "coin accepted!" : r1.error().message) + std::string{" | "} +
                 (r2 ? "price accepted!" : r2.error().message) + " | " + (r3 ? "size accepted!" : r3.error().message);
        return !r1 && !r2 && !r3;
    });

    // ── 9. taker execution (optional) ───────────────────────────────────────
    if (runner.taker()) {
        runner.step("IOC order that crosses → fill, position and fees", [&](std::string& detail) {
            const auto px = runner.priceAway(hl::Side::Buy, -50.0);  // 50 bps through the ask
            auto placed = ex->placeOrder(runner.request(hl::Side::Buy, px, hl::Tif::Ioc));
            if (!placed) {
                detail = placed.error().message;
                return false;
            }
            if (!runner.waitFor([&] {
                    const hl::Order* o = ex->findOrder(placed.value());
                    return o != nullptr && !o->isLive();
                })) {
                detail = "no terminal state";
                return false;
            }
            // The acknowledgement reports the fill immediately; the userFills stream (and therefore the
            // tracked position) follows a block later. Wait for it before judging.
            const bool streamed = runner.waitFor([&] { return !runner.fills().empty() && !ex->position(coin).isZero(); },
                                                 20'000);
            const hl::Order* o = ex->findOrder(placed.value());
            detail = std::string{hl::toString(o->state)} + ", filled " + o->filledSz.toString() + " @ " +
                     o->avgFillPx.toString() + ", position " + ex->position(coin).toString() + ", fills " +
                     std::to_string(runner.fills().size()) +
                     (runner.fills().empty() ? "" : ", fee " + runner.fills().back().fee.toString() + " " +
                                                        runner.fills().back().feeToken) +
                     (o->lastError.empty() ? "" : ", " + o->lastError);
            return o->state == hl::OrderState::Filled && streamed;
        });

        runner.step("close the position with a reduce-only IOC", [&](std::string& detail) {
            const hl::Decimal pos = ex->position(coin);
            if (pos.isZero()) {
                detail = "no position to close";
                return true;
            }
            const bool isLong = !pos.isNegative();
            hl::OrderRequest req = runner.request(isLong ? hl::Side::Sell : hl::Side::Buy,
                                                  runner.priceAway(isLong ? hl::Side::Sell : hl::Side::Buy, -50.0),
                                                  hl::Tif::Ioc);
            req.sz = runner.asset()->roundSz(isLong ? pos : -pos, hl::RoundingMode::Down);
            req.reduceOnly = true;
            auto placed = ex->placeOrder(req);
            if (!placed) {
                detail = placed.error().message;
                return false;
            }
            runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(placed.value());
                return o != nullptr && !o->isLive();
            });
            // The closing fill may arrive after the acknowledgement — wait for the position to flatten.
            const bool flat = runner.waitFor([&] { return ex->position(coin).isZero(); }, 10'000);
            const hl::Order* o = ex->findOrder(placed.value());
            detail = std::string{hl::toString(o->state)} + ", position now " + ex->position(coin).toString() +
                     (o->lastError.empty() ? "" : ", " + o->lastError);
            return o->state == hl::OrderState::Filled && flat;
        });
    }

    // ── 10. account-level actions ───────────────────────────────────────────
    runner.step("scheduleCancel (dead-man's switch): arm and clear", [&](std::string& detail) {
        std::string armed;
        std::string cleared;
        bool a = false;
        bool c = false;
        (void)ex->scheduleCancel(hl::EventLoop::wallClockMs() + 60'000, [&](const hl::Result<hl::ExchangeResponse>& r) {
            a = true;
            armed = r ? "ok" : r.error().message;
        });
        if (!runner.waitFor([&] { return a; })) {
            detail = "no response to arm";
            return false;
        }
        (void)ex->scheduleCancel(std::nullopt, [&](const hl::Result<hl::ExchangeResponse>& r) {
            c = true;
            cleared = r ? "ok" : r.error().message;
        });
        runner.waitFor([&] { return c; });
        detail = "arm: " + armed + " | clear: " + cleared;
        // Some accounts are not allowed to use it (volume requirement) — report, do not fail.
        return true;
    });

    runner.step("updateLeverage", [&](std::string& detail) {
        bool done = false;
        std::string text;
        if (hl::Error e = ex->updateLeverage(coin, 5, true, [&](const hl::Result<hl::ExchangeResponse>& r) {
                done = true;
                text = r ? "ok" : r.error().message;
            })) {
            detail = e.message;
            return false;
        }
        if (!runner.waitFor([&] { return done; })) {
            detail = "no response";
            return false;
        }
        detail = text;
        return true;
    });

    // ── 11. reconnect and reconciliation ────────────────────────────────────
    runner.step("reconnect: drop the private socket, reconcile a live order", [&](std::string& detail) {
        auto placed = ex->placeOrder(runner.request(hl::Side::Buy, runner.priceAway(hl::Side::Buy, offsetBps)));
        if (!placed) {
            detail = placed.error().message;
            return false;
        }
        if (!runner.waitFor([&] {
                const hl::Order* o = ex->findOrder(placed.value());
                return o != nullptr && o->state == hl::OrderState::Open;
            })) {
            detail = "order did not rest";
            return false;
        }
        const int before = runner.disconnects();
        const auto reconcilesBefore = ex->stats().reconciles;
        ex->session().reconnectNow("live-check forced reconnect");
        if (!runner.waitFor([&] { return runner.disconnects() > before; }, 10'000)) {
            detail = "no disconnect observed";
            return false;
        }
        if (!runner.waitFor([&] { return runner.ready(); }, 30'000)) {
            detail = "did not become ready again";
            return false;
        }
        const bool stillOpen = runner.waitFor([&] {
            const hl::Order* o = ex->findOrder(placed.value());
            return o != nullptr && o->state == hl::OrderState::Open && ex->stats().reconciles > reconcilesBefore;
        });
        detail = "reconnected, reconciles " + std::to_string(reconcilesBefore) + " → " +
                 std::to_string(ex->stats().reconciles) + ", order " +
                 std::string{hl::toString(ex->findOrder(placed.value())->state)};
        (void)ex->cancelAll(coin);
        runner.waitFor([&] { return ex->liveOrders(coin).empty(); }, 10'000);
        return stillOpen;
    });

    // ── 12. clean exit ──────────────────────────────────────────────────────
    runner.step("no orders left on the venue", [&](std::string& detail) {
        (void)ex->cancelAll();
        runner.waitFor([&] { return ex->liveOrders().empty(); }, 10'000);
        bool done = false;
        std::size_t remaining = 0;
        ex->info().openOrders(ex->accountAddress(), [&](const hl::Result<std::vector<hl::OpenOrder>>& r) {
            done = true;
            remaining = r ? r.value().size() : 999;
        });
        if (!runner.waitFor([&] { return done; })) {
            detail = "openOrders timed out";
            return false;
        }
        detail = std::to_string(remaining) + " open orders on the venue";
        return remaining == 0;
    });

    // ── 13. leave the account flat ──────────────────────────────────────────
    runner.step("no leftover position (a resting test order may have been filled)", [&](std::string& detail) {
        if (!runner.taker() && !ex->position(coin).isZero()) {
            detail = "position " + ex->position(coin).toString() +
                     " left open — rerun with --taker or --flatten to close it";
            return false;
        }
        return flatten(detail);
    });

    // ── report ──────────────────────────────────────────────────────────────
    int failed = 0;
    std::printf("\n══ live check summary ═══════════════════════════════════\n");
    for (const auto& r : runner.results()) {
        std::printf("  %-4s %s\n", r.passed ? "PASS" : "FAIL", r.name.c_str());
        failed += r.passed ? 0 : 1;
    }
    const auto& st = ex->stats();
    const auto sig = ex->signingStats();
    std::printf("  ---------------------------------------------------\n");
    std::printf("  actions %" PRIu64 " (%" PRIu64 " via HTTP), errors %" PRIu64 ", timeouts %" PRIu64
                ", reconciles %" PRIu64 "\n",
                st.actionsSent, st.actionsViaHttp, st.actionErrors, st.timeouts, st.reconciles);
    std::printf("  signatures %" PRIu64 " precomputed-nonce / %" PRIu64 " deterministic\n", sig.precomputed,
                sig.deterministic);
    std::printf("  order updates %" PRIu64 ", fills %zu, md messages %" PRIu64 " (parse errors %" PRIu64 ")\n",
                runner.updates(), runner.fills().size(), md.parserStats().messages, md.parserStats().parseErrors);
    std::printf("  %d of %zu steps failed\n", failed, runner.results().size());
    std::printf("═════════════════════════════════════════════════════════\n");

    ex->stop();
    md.stop();
    return failed == 0 ? 0 : 1;
}
