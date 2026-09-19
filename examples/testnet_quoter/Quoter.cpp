// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include "testnet_quoter/Quoter.h"

#include <cinttypes>
#include <algorithm>
#include <cstdio>

namespace example {

using hl::Decimal;
using hl::Side;

namespace {

std::size_t idx(Side s) noexcept { return s == Side::Buy ? 0 : 1; }

std::string str(Decimal d) { return d.toString(); }

}  // namespace

Quoter::Quoter(hl::EventLoop& loop, QuoterSettings settings) : loop_(loop), settings_(std::move(settings)) {
    loop_.addTimer(settings_.statusIntervalMs, [this] { printStatus(); });
}

// ── market data ─────────────────────────────────────────────────────────────

void Quoter::onConnected() { std::printf("[md] connected, waiting for %s book\n", settings_.coin.c_str()); }

void Quoter::onDisconnected(std::string_view reason) {
    std::printf("[conn] disconnected: %.*s (auto-reconnecting)\n", static_cast<int>(reason.size()), reason.data());
}

void Quoter::onBookUpdate(const hl::OrderBook& book, BookUpdate /*kind*/) {
    if (book.coin() == settings_.coin) {
        requote(book);
    }
}

// ── exchange ────────────────────────────────────────────────────────────────

void Quoter::onReady() {
    if (!positionSeeded_) {
        startPosition_ = exchange_->position(settings_.coin);
        positionSeeded_ = true;
    }
    const hl::AssetInfo* asset = exchange_->assets().find(settings_.coin);
    std::printf("[ex] ready: account %s, signer %s, %s asset=%u szDecimals=%d, position %s\n",
                hl::toHex(exchange_->accountAddress()).c_str(), hl::toHex(exchange_->signerAddress()).c_str(),
                settings_.coin.c_str(), asset != nullptr ? asset->asset : 0U, asset != nullptr ? asset->szDecimals : -1,
                str(exchange_->position(settings_.coin)).c_str());
    if (settings_.deadManSwitch) {
        refreshDeadManSwitch();
    }
}

void Quoter::onOrderUpdate(const hl::Order& order) {
    if (order.coin != settings_.coin || order.external) {
        return;
    }
    if (order.state == hl::OrderState::Rejected) {
        ++rejects_;
        std::printf("[ex] %s %s @ %s rejected: %s\n", std::string{hl::toString(order.side)}.c_str(), str(order.origSz).c_str(),
                    str(order.px).c_str(), order.lastError.c_str());
    }
    Slot& slot = slots_[idx(order.side)];
    if (slot.cloid != order.cloid) {
        return;
    }
    if (order.state == hl::OrderState::Rejected) {
        // Exponential back-off on repeated rejections (1 s, 2 s, 4 s … 60 s) so a permanent
        // error such as an unfunded account does not turn into a request storm.
        ++slot.consecutiveRejects;
        const std::int64_t delay = std::min<std::int64_t>(60'000, 1'000LL << std::min(slot.consecutiveRejects - 1, 6));
        slot.backoffUntilMs = hl::EventLoop::nowMs() + delay;
    } else if (order.state == hl::OrderState::Open || order.state == hl::OrderState::Filled) {
        slot.consecutiveRejects = 0;
    }
    if (hl::isTerminal(order.state)) {
        slot.cloid.reset();  // free the side for a new quote on the next book update
    }
}

void Quoter::onFill(const hl::Fill& fill) {
    if (fill.coin != settings_.coin) {
        return;
    }
    ++fills_;
    if (!fill.crossed) {
        ++makerFills_;
    }
    const Decimal notional = fill.px.mul(fill.sz);
    volumeUsd_ += notional;
    cash_ += fill.side == Side::Sell ? notional : -notional;
    cash_ -= fill.fee;
    std::printf("[fill] %s %s %s @ %s (%s, fee %s %s) → position %s\n", std::string{hl::toString(fill.side)}.c_str(),
                str(fill.sz).c_str(), fill.coin.c_str(), str(fill.px).c_str(), fill.crossed ? "taker" : "maker",
                str(fill.fee).c_str(), fill.feeToken.c_str(), str(fill.endPosition()).c_str());
}

void Quoter::onError(const hl::Error& error) {
    std::printf("[ex] error (%s): %s\n", std::string{hl::toString(error.kind)}.c_str(), error.message.c_str());
}

// ── strategy ────────────────────────────────────────────────────────────────

void Quoter::requote(const hl::OrderBook& book) {
    if (shuttingDown_ || haltedByRisk_) {
        return;
    }
    const hl::AssetInfo* asset = nullptr;
    Decimal position;
    if (exchange_ != nullptr) {
        if (!exchange_->isReady()) {
            return;
        }
        asset = exchange_->assets().find(settings_.coin);
        position = exchange_->position(settings_.coin);
    } else {
        asset = dryRunAssets_.find(settings_.coin);
    }
    if (asset == nullptr) {
        return;
    }
    lastQuotes_ = computeQuotes(book, position, *asset, settings_.quote);

    // Hard stop: inventory beyond 150% of the limit (e.g. after a burst of fills) → flatten quotes and halt.
    if (std::abs(lastQuotes_.inventoryRatio) >= 1.0 &&
        std::abs(position.toDouble() * lastQuotes_.fair.toDouble()) > 1.5 * settings_.quote.maxPositionUsd.toDouble()) {
        haltedByRisk_ = true;
        std::printf("[risk] position %s beyond 150%% of limit — canceling quotes and halting\n", str(position).c_str());
        if (exchange_ != nullptr) {
            (void)exchange_->cancelAll(settings_.coin);
        }
        return;
    }
    if (settings_.dryRun || exchange_ == nullptr) {
        return;
    }
    manageSide(Side::Buy, lastQuotes_.bidPx, lastQuotes_.size);
    manageSide(Side::Sell, lastQuotes_.askPx, lastQuotes_.size);
}

void Quoter::manageSide(Side side, const std::optional<Decimal>& target, Decimal size) {
    Slot& slot = slots_[idx(side)];
    const std::int64_t now = hl::EventLoop::nowMs();
    const hl::Order* live = slot.cloid ? exchange_->findOrder(*slot.cloid) : nullptr;
    if (live != nullptr && !live->isLive()) {
        slot.cloid.reset();
        live = nullptr;
    }

    if (!target) {
        if (live != nullptr && !live->cancelPending) {
            (void)exchange_->cancel(live->cloid);  // inventory limit reached on this side
        }
        return;
    }
    if (now - slot.lastActionMs < settings_.minRequoteIntervalMs || now < slot.backoffUntilMs) {
        return;
    }
    if (live == nullptr) {
        hl::OrderRequest req;
        req.coin = settings_.coin;
        req.side = side;
        req.px = *target;
        req.sz = size;
        req.tif = hl::Tif::Alo;
        auto placed = exchange_->placeOrder(req);
        if (!placed) {
            std::printf("[strategy] place %s failed: %s\n", std::string{hl::toString(side)}.c_str(),
                        placed.error().message.c_str());
            slot.lastActionMs = now;
            return;
        }
        slot.cloid = placed.value();
        slot.lastActionMs = now;
        ++ordersPlaced_;
        return;
    }
    if (live->state == hl::OrderState::PendingNew || live->cancelPending || live->modifyPending || live->oid == 0) {
        return;
    }
    const bool priceStale = distanceBps(live->px, *target) >= settings_.requoteBps;
    const bool sizeStale = live->remainingSz() != size;
    if (!priceStale && !sizeStale) {
        return;
    }
    if (hl::Error err = exchange_->modify(live->cloid, *target, size)) {
        std::printf("[strategy] modify failed: %s\n", err.message.c_str());
    } else {
        ++amendments_;
    }
    slot.lastActionMs = now;
}

void Quoter::refreshDeadManSwitch() {
    if (shuttingDown_ || exchange_ == nullptr) {
        return;
    }
    const std::int64_t at = hl::EventLoop::wallClockMs() + 90'000;
    (void)exchange_->scheduleCancel(at, [](const hl::Result<hl::ExchangeResponse>& r) {
        if (!r) {
            std::printf("[ex] scheduleCancel not accepted: %s\n", r.error().message.c_str());
        }
    });
    loop_.addTimer(30'000, [this] { refreshDeadManSwitch(); });
}

void Quoter::printStatus() {
    loop_.addTimer(settings_.statusIntervalMs, [this] { printStatus(); });
    const hl::OrderBook* book = md_ != nullptr ? md_->book(settings_.coin) : nullptr;
    if (book == nullptr || !book->isValid()) {
        std::printf("[status] waiting for %s book…\n", settings_.coin.c_str());
        return;
    }
    const Decimal mid = book->mid();
    std::string quotes;
    if (exchange_ != nullptr && !settings_.dryRun) {
        for (Side s : {Side::Buy, Side::Sell}) {
            const Slot& slot = slots_[idx(s)];
            const hl::Order* o = slot.cloid ? exchange_->findOrder(*slot.cloid) : nullptr;
            quotes += s == Side::Buy ? " bid=" : " ask=";
            quotes += o != nullptr ? str(o->px) + "(" + std::string{hl::toString(o->state)} + ")" : "-";
        }
    } else {
        quotes = " bid=" + (lastQuotes_.bidPx ? str(*lastQuotes_.bidPx) : "-") +
                 " ask=" + (lastQuotes_.askPx ? str(*lastQuotes_.askPx) : "-") + " size=" + str(lastQuotes_.size) +
                 " (dry run)";
    }
    const Decimal position = exchange_ != nullptr ? exchange_->position(settings_.coin) : Decimal{};
    const Decimal pnl = cash_ + (position - startPosition_).mul(mid);
    // Order-path latency so far: the venue round trip an order costs, and the local share of it.
    std::string latency;
    if (exchange_ != nullptr) {
        const auto& st = exchange_->stats();
        if (st.orderRoundTrip.count != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " order_rt=%.0f/%.0fms(mean/last) sign=%.2fms",
                          static_cast<double>(st.orderRoundTrip.meanUs()) / 1000.0,
                          static_cast<double>(st.orderRoundTrip.lastUs) / 1000.0,
                          static_cast<double>(st.buildAndSign.meanUs()) / 1000.0);
            latency = buf;
        }
    }
    std::printf("[status] %s mid=%s spread=%.2fbps%s pos=%s fills=%" PRIu64 " vol=$%s pnl≈$%s%s%s\n",
                settings_.coin.c_str(), str(mid).c_str(), book->spreadBps(), quotes.c_str(), str(position).c_str(),
                fills_, str(volumeUsd_).c_str(), str(pnl).c_str(), latency.c_str(),
                haltedByRisk_ ? "  [HALTED: position beyond 150% of the limit, quoting stopped]" : "");
}

void Quoter::shutdown(std::int64_t timeoutMs) {
    shuttingDown_ = true;
    if (exchange_ == nullptr || settings_.dryRun || !exchange_->isReady()) {
        return;
    }
    std::printf("[shutdown] canceling %zu live order(s)…\n", exchange_->liveOrders(settings_.coin).size());
    (void)exchange_->cancelAll(settings_.coin);
    const std::int64_t deadline = hl::EventLoop::nowMs() + timeoutMs;
    while (!exchange_->liveOrders(settings_.coin).empty() && hl::EventLoop::nowMs() < deadline) {
        loop_.runOnce(50);
    }
    if (settings_.deadManSwitch) {
        bool done = false;
        (void)exchange_->scheduleCancel(std::nullopt, [&done](const auto&) { done = true; });
        while (!done && hl::EventLoop::nowMs() < deadline) {
            loop_.runOnce(50);
        }
    }
}

void Quoter::printSummary() const {
    const hl::OrderBook* book = md_ != nullptr ? md_->book(settings_.coin) : nullptr;
    const Decimal mid = book != nullptr ? book->mid() : Decimal{};
    const Decimal position = exchange_ != nullptr ? exchange_->position(settings_.coin) : Decimal{};
    std::printf("\n══ summary ══════════════════════════════════════════════\n");
    std::printf("  coin            %s\n", settings_.coin.c_str());
    std::printf("  orders placed   %" PRIu64 "   amendments %" PRIu64 "   rejects %" PRIu64 "\n", ordersPlaced_,
                amendments_, rejects_);
    std::printf("  fills           %" PRIu64 " (maker %" PRIu64 ")   volume $%s\n", fills_, makerFills_,
                str(volumeUsd_).c_str());
    if (haltedByRisk_) {
        std::printf("  risk            HALTED — inventory exceeded 150%% of the limit; quoting was stopped\n");
    }
    std::printf("  position        %s → %s\n", str(startPosition_).c_str(), str(position).c_str());
    std::printf("  pnl (mark@mid)  $%s\n", str(cash_ + (position - startPosition_).mul(mid)).c_str());
    if (exchange_ != nullptr) {
        const auto& st = exchange_->stats();
        std::printf("  actions         %" PRIu64 " sent, %" PRIu64 " via HTTP, %" PRIu64 " errors, %" PRIu64
                    " timeouts, %" PRIu64 " reconciles\n",
                    st.actionsSent, st.actionsViaHttp, st.actionErrors, st.timeouts, st.reconciles);
        const auto sig = exchange_->signingStats();
        std::printf("  signatures      %" PRIu64 " precomputed-nonce, %" PRIu64 " deterministic\n", sig.precomputed,
                    sig.deterministic);
        const auto row = [](const char* label, const hl::ExchangeClient::LatencyStats& l) {
            if (l.count == 0) {
                return;
            }
            std::printf("  %-15s n=%-4" PRIu64 " mean %7.3f ms   min %7.3f   max %7.3f\n", label, l.count,
                        static_cast<double>(l.meanUs()) / 1000.0, static_cast<double>(l.minUs) / 1000.0,
                        static_cast<double>(l.maxUs) / 1000.0);
        };
        row("build+sign", st.buildAndSign);
        row("order rt", st.orderRoundTrip);
        row("cancel rt", st.cancelRoundTrip);
        row("modify rt", st.modifyRoundTrip);
    }
    if (md_ != nullptr) {
        std::printf("  md messages     %" PRIu64 " (parse errors %" PRIu64 ", reconnects %" PRIu64 ")\n",
                    md_->parserStats().messages, md_->parserStats().parseErrors, md_->reconnectCount());
    }
    std::printf("═════════════════════════════════════════════════════════\n");
}

}  // namespace example
