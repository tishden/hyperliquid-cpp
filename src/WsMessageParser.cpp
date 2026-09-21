// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/WsMessageParser.h"

#include <simdjson.h>

#include <cstring>
#include <vector>

namespace hl {

namespace od = simdjson::ondemand;

namespace {

constexpr std::size_t kMaxLevelsPerSide = 64;

// ── field helpers (all return false on a type error) ─────────────────────────

bool readString(od::value v, std::string_view& out) { return v.get_string().get(out) == simdjson::SUCCESS; }

bool readDecimal(od::value v, Decimal& out) {
    std::string_view s;
    if (v.get_string().get(s) != simdjson::SUCCESS) {
        // Some fields may arrive as JSON numbers; accept them exactly via their raw token.
        std::string_view raw = v.raw_json_token();
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == ',' || raw.back() == '}' || raw.back() == ']')) {
            raw.remove_suffix(1);
        }
        return Decimal::parse(raw, out);
    }
    return Decimal::parse(s, out);
}

bool readInt(od::value v, std::int64_t& out) { return v.get_int64().get(out) == simdjson::SUCCESS; }
bool readUint(od::value v, std::uint64_t& out) { return v.get_uint64().get(out) == simdjson::SUCCESS; }

bool readSide(od::value v, Side& out) {
    std::string_view s;
    if (!readString(v, s) || s.empty()) {
        return false;
    }
    out = (s[0] == 'B') ? Side::Buy : Side::Sell;
    return true;
}

void readOptionalCloid(od::value v, std::optional<Cloid>& out) {
    std::string_view s;
    if (v.get_string().get(s) == simdjson::SUCCESS) {
        out = Cloid::parse(s);
    }
}

}  // namespace

OrderUpdateStatus parseOrderUpdateStatus(std::string_view s) noexcept {
    if (s == "open") {
        return OrderUpdateStatus::Open;
    }
    if (s == "filled") {
        return OrderUpdateStatus::Filled;
    }
    if (s == "triggered") {
        return OrderUpdateStatus::Triggered;
    }
    if (s == "canceled" || s == "scheduledCancel" ||
        (s.size() > 8 && s.substr(s.size() - 8) == "Canceled")) {
        return OrderUpdateStatus::Canceled;
    }
    if (s == "rejected" || (s.size() > 8 && s.substr(s.size() - 8) == "Rejected")) {
        return OrderUpdateStatus::Rejected;
    }
    return OrderUpdateStatus::Unknown;
}

std::string_view toString(OrderUpdateStatus status) noexcept {
    switch (status) {
        case OrderUpdateStatus::Open: return "Open";
        case OrderUpdateStatus::Filled: return "Filled";
        case OrderUpdateStatus::Canceled: return "Canceled";
        case OrderUpdateStatus::Triggered: return "Triggered";
        case OrderUpdateStatus::Rejected: return "Rejected";
        case OrderUpdateStatus::Unknown: return "Unknown";
    }
    return "Unknown";
}

// Per-nesting-level parse state: simdjson parser, padded input copy and the reusable message
// buffers. A handler callback may run the event loop and thus re-enter parse(); each nesting level
// gets its own state so the views handed to the outer callback stay valid.
struct ParseState {
    od::parser parser{};
    std::vector<char> padded{};
    std::vector<BookLevel> bids{};
    std::vector<BookLevel> asks{};
    std::vector<TradeMsg> trades{};
    std::vector<MidEntry> mids{};
    std::vector<OrderUpdateMsg> orderUpdates{};
    std::vector<FillMsg> fills{};
    std::vector<NonUserCancelMsg> nonUserCancels{};
    std::vector<UserFundingMsg> fundings{};

    ParseState() {
        padded.resize(4096 + simdjson::SIMDJSON_PADDING);
        bids.reserve(kMaxLevelsPerSide);
        asks.reserve(kMaxLevelsPerSide);
        trades.reserve(256);
        mids.reserve(512);
        orderUpdates.reserve(64);
        fills.reserve(256);
    }

    bool parse(std::string_view frame, WsMessageHandler& h, WsMessageParser::Stats& stats) {
        ++stats.messages;
        if (padded.size() < frame.size() + simdjson::SIMDJSON_PADDING) {
            padded.resize(frame.size() + simdjson::SIMDJSON_PADDING);
        }
        std::memcpy(padded.data(), frame.data(), frame.size());
        std::memset(padded.data() + frame.size(), 0, simdjson::SIMDJSON_PADDING);

        od::document doc;
        if (parser.iterate(padded.data(), frame.size(), padded.size()).get(doc) != simdjson::SUCCESS) {
            ++stats.parseErrors;
            return false;
        }
        std::string_view channel;
        if (doc["channel"].get_string().get(channel) != simdjson::SUCCESS) {
            ++stats.parseErrors;
            return false;
        }
        bool ok = true;
        if (channel == "bbo") {
            ok = onBbo(doc, h);
        } else if (channel == "l2Book") {
            ok = onL2Book(doc, h);
        } else if (channel == "trades") {
            ok = onTrades(doc, h);
        } else if (channel == "activeAssetCtx" || channel == "activeSpotAssetCtx") {
            ok = onAssetCtx(doc, channel == "activeSpotAssetCtx", h);
        } else if (channel == "orderUpdates") {
            ok = onOrderUpdates(doc, h);
        } else if (channel == "userFills") {
            ok = onUserFills(doc, h);
        } else if (channel == "candle") {
            ok = onCandle(doc, h);
        } else if (channel == "user") {
            // The `userEvents` subscription delivers on a channel called "user".
            ok = onUserEvents(doc, h);
        } else if (channel == "userFundings") {
            ok = onUserFundings(doc, h);
        } else if (channel == "activeAssetData") {
            ok = onActiveAssetData(doc, h);
        } else if (channel == "notification") {
            ok = onNotification(doc, h);
        } else if (channel == "post") {
            ok = onPost(doc, h);
        } else if (channel == "allMids") {
            ok = onAllMids(doc, h);
        } else if (channel == "subscriptionResponse") {
            ok = onSubscriptionResponse(doc, h);
        } else if (channel == "pong") {
            h.onPong();
        } else if (channel == "error") {
            std::string_view text;
            if (doc["data"].get_string().get(text) != simdjson::SUCCESS) {
                text = frame;
            }
            h.onVenueError(text);
        } else {
            ++stats.unhandled;
            h.onUnhandled(channel, frame);
        }
        if (!ok) {
            ++stats.parseErrors;
        }
        return ok;
    }

    static bool readLevel(od::value v, BookLevel& lvl) {
        od::object obj;
        if (v.get_object().get(obj) != simdjson::SUCCESS) {
            return false;
        }
        for (auto field : obj) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "px") {
                if (!readDecimal(field.value(), lvl.px)) { return false; }
            } else if (key == "sz") {
                if (!readDecimal(field.value(), lvl.sz)) { return false; }
            } else if (key == "n") {
                std::uint64_t n = 0;
                if (!readUint(field.value(), n)) { return false; }
                lvl.n = static_cast<std::uint32_t>(n);
            }
        }
        return true;
    }

    bool onL2Book(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        L2BookMsg msg;
        bids.clear();
        asks.clear();
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "coin") {
                if (!readString(field.value(), msg.coin)) { return false; }
            } else if (key == "time") {
                if (!readInt(field.value(), msg.timeMs)) { return false; }
            } else if (key == "levels") {
                od::array sides;
                if (field.value().get_array().get(sides) != simdjson::SUCCESS) {
                    return false;
                }
                std::size_t sideIdx = 0;
                for (auto side : sides) {
                    auto& dst = sideIdx == 0 ? bids : asks;
                    od::array levels;
                    if (side.get_array().get(levels) != simdjson::SUCCESS) {
                        return false;
                    }
                    for (auto lvlVal : levels) {
                        BookLevel lvl;
                        od::value v;
                        if (lvlVal.get(v) != simdjson::SUCCESS || !readLevel(v, lvl)) {
                            return false;
                        }
                        if (dst.size() < kMaxLevelsPerSide) {
                            dst.push_back(lvl);
                        }
                    }
                    if (++sideIdx == 2) {
                        break;
                    }
                }
            }
        }
        msg.bids = bids;
        msg.asks = asks;
        h.onL2Book(msg);
        return true;
    }

    bool onBbo(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        BboMsg msg;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "coin") {
                if (!readString(field.value(), msg.coin)) { return false; }
            } else if (key == "time") {
                if (!readInt(field.value(), msg.timeMs)) { return false; }
            } else if (key == "bbo") {
                od::array arr;
                if (field.value().get_array().get(arr) != simdjson::SUCCESS) {
                    return false;
                }
                std::size_t idx = 0;
                for (auto elem : arr) {
                    od::value v;
                    if (elem.get(v) != simdjson::SUCCESS) {
                        return false;
                    }
                    bool isNull = false;
                    if (v.is_null().get(isNull) == simdjson::SUCCESS && isNull) {
                        ++idx;
                        continue;
                    }
                    if (idx == 0) {
                        if (!readLevel(v, msg.bid)) { return false; }
                        msg.hasBid = true;
                    } else if (idx == 1) {
                        if (!readLevel(v, msg.ask)) { return false; }
                        msg.hasAsk = true;
                    }
                    ++idx;
                }
            }
        }
        h.onBbo(msg);
        return true;
    }

    bool onTrades(od::document& doc, WsMessageHandler& h) {
        od::array arr;
        if (doc["data"].get_array().get(arr) != simdjson::SUCCESS) {
            return false;
        }
        trades.clear();
        for (auto elem : arr) {
            od::object obj;
            if (elem.get_object().get(obj) != simdjson::SUCCESS) {
                return false;
            }
            TradeMsg t;
            for (auto field : obj) {
                std::string_view key;
                if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                    return false;
                }
                if (key == "coin") {
                    if (!readString(field.value(), t.coin)) { return false; }
                } else if (key == "side") {
                    if (!readSide(field.value(), t.side)) { return false; }
                } else if (key == "px") {
                    if (!readDecimal(field.value(), t.px)) { return false; }
                } else if (key == "sz") {
                    if (!readDecimal(field.value(), t.sz)) { return false; }
                } else if (key == "time") {
                    if (!readInt(field.value(), t.timeMs)) { return false; }
                } else if (key == "tid") {
                    if (!readUint(field.value(), t.tid)) { return false; }
                } else if (key == "hash") {
                    if (!readString(field.value(), t.hash)) { return false; }
                }
            }
            trades.push_back(t);
        }
        h.onTrades(trades);
        return true;
    }

    bool onAssetCtx(od::document& doc, bool isSpot, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        AssetCtxMsg msg;
        msg.isSpot = isSpot;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "coin") {
                if (!readString(field.value(), msg.coin)) { return false; }
            } else if (key == "ctx") {
                od::object ctx;
                if (field.value().get_object().get(ctx) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto c : ctx) {
                    std::string_view k;
                    if (c.escaped_key().get(k) != simdjson::SUCCESS) {
                        return false;
                    }
                    Decimal* target = nullptr;
                    if (k == "funding") {
                        target = &msg.funding;
                    } else if (k == "openInterest") {
                        target = &msg.openInterest;
                    } else if (k == "oraclePx") {
                        target = &msg.oraclePx;
                    } else if (k == "premium") {
                        target = &msg.premium;
                    } else if (k == "markPx") {
                        target = &msg.markPx;
                    } else if (k == "midPx") {
                        target = &msg.midPx;
                    } else if (k == "prevDayPx") {
                        target = &msg.prevDayPx;
                    } else if (k == "dayNtlVlm") {
                        target = &msg.dayNtlVlm;
                    } else if (k == "dayBaseVlm") {
                        target = &msg.dayBaseVlm;
                    } else if (k == "circulatingSupply") {
                        target = &msg.circulatingSupply;
                    }
                    if (target != nullptr) {
                        od::value v;
                        if (c.value().get(v) != simdjson::SUCCESS) {
                            return false;
                        }
                        bool isNull = false;
                        if (v.is_null().get(isNull) == simdjson::SUCCESS && isNull) {
                            continue;
                        }
                        if (!readDecimal(v, *target)) {
                            return false;
                        }
                        if (target == &msg.midPx) {
                            msg.hasMidPx = true;
                        }
                    }
                }
            }
        }
        h.onAssetCtx(msg);
        return true;
    }

    bool onAllMids(od::document& doc, WsMessageHandler& h) {
        od::object mids_obj;
        if (doc["data"]["mids"].get_object().get(mids_obj) != simdjson::SUCCESS) {
            return false;
        }
        mids.clear();
        for (auto field : mids_obj) {
            MidEntry e;
            if (field.escaped_key().get(e.coin) != simdjson::SUCCESS || !readDecimal(field.value(), e.mid)) {
                return false;
            }
            mids.push_back(e);
        }
        h.onAllMids(AllMidsMsg{mids});
        return true;
    }

    bool onOrderUpdates(od::document& doc, WsMessageHandler& h) {
        od::array arr;
        if (doc["data"].get_array().get(arr) != simdjson::SUCCESS) {
            return false;
        }
        orderUpdates.clear();
        for (auto elem : arr) {
            od::object obj;
            if (elem.get_object().get(obj) != simdjson::SUCCESS) {
                return false;
            }
            OrderUpdateMsg u;
            for (auto field : obj) {
                std::string_view key;
                if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                    return false;
                }
                if (key == "order") {
                    od::object order;
                    if (field.value().get_object().get(order) != simdjson::SUCCESS) {
                        return false;
                    }
                    for (auto of : order) {
                        std::string_view k;
                        if (of.escaped_key().get(k) != simdjson::SUCCESS) {
                            return false;
                        }
                        if (k == "coin") {
                            if (!readString(of.value(), u.coin)) { return false; }
                        } else if (k == "side") {
                            if (!readSide(of.value(), u.side)) { return false; }
                        } else if (k == "limitPx") {
                            if (!readDecimal(of.value(), u.limitPx)) { return false; }
                        } else if (k == "sz") {
                            if (!readDecimal(of.value(), u.sz)) { return false; }
                        } else if (k == "origSz") {
                            if (!readDecimal(of.value(), u.origSz)) { return false; }
                        } else if (k == "oid") {
                            if (!readUint(of.value(), u.oid)) { return false; }
                        } else if (k == "timestamp") {
                            if (!readInt(of.value(), u.timestampMs)) { return false; }
                        } else if (k == "cloid") {
                            readOptionalCloid(of.value(), u.cloid);
                        }
                    }
                } else if (key == "status") {
                    if (!readString(field.value(), u.statusText)) { return false; }
                    u.status = parseOrderUpdateStatus(u.statusText);
                } else if (key == "statusTimestamp") {
                    if (!readInt(field.value(), u.statusTimestampMs)) { return false; }
                }
            }
            orderUpdates.push_back(u);
        }
        h.onOrderUpdates(orderUpdates);
        return true;
    }

    bool readFill(od::value value, FillMsg& f) {
        od::object obj;
        if (value.get_object().get(obj) != simdjson::SUCCESS) {
            return false;
        }
            for (auto ff : obj) {
                std::string_view k;
                if (ff.escaped_key().get(k) != simdjson::SUCCESS) {
                    return false;
                }
                if (k == "coin") {
                    if (!readString(ff.value(), f.coin)) { return false; }
                } else if (k == "px") {
                    if (!readDecimal(ff.value(), f.px)) { return false; }
                } else if (k == "sz") {
                    if (!readDecimal(ff.value(), f.sz)) { return false; }
                } else if (k == "side") {
                    if (!readSide(ff.value(), f.side)) { return false; }
                } else if (k == "time") {
                    if (!readInt(ff.value(), f.timeMs)) { return false; }
                } else if (k == "startPosition") {
                    if (!readDecimal(ff.value(), f.startPosition)) { return false; }
                } else if (k == "dir") {
                    if (!readString(ff.value(), f.dir)) { return false; }
                } else if (k == "closedPnl") {
                    if (!readDecimal(ff.value(), f.closedPnl)) { return false; }
                } else if (k == "hash") {
                    if (!readString(ff.value(), f.hash)) { return false; }
                } else if (k == "oid") {
                    if (!readUint(ff.value(), f.oid)) { return false; }
                } else if (k == "crossed") {
                    if (ff.value().get_bool().get(f.crossed) != simdjson::SUCCESS) { return false; }
                } else if (k == "fee") {
                    if (!readDecimal(ff.value(), f.fee)) { return false; }
                } else if (k == "feeToken") {
                    if (!readString(ff.value(), f.feeToken)) { return false; }
                } else if (k == "tid") {
                    if (!readUint(ff.value(), f.tid)) { return false; }
                } else if (k == "cloid") {
                    readOptionalCloid(ff.value(), f.cloid);
                }
            }
        return true;
    }

    bool onUserFills(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        UserFillsMsg msg;
        fills.clear();
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "isSnapshot") {
                if (field.value().get_bool().get(msg.isSnapshot) != simdjson::SUCCESS) { return false; }
            } else if (key == "user") {
                if (!readString(field.value(), msg.user)) { return false; }
            } else if (key == "fills") {
                od::array arr;
                if (field.value().get_array().get(arr) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto elem : arr) {
                    od::value v;
                    if (elem.get(v) != simdjson::SUCCESS) {
                        return false;
                    }
                    FillMsg f;
                    if (!readFill(v, f)) {
                        return false;
                    }
                    fills.push_back(f);
                }
            }
        }
        msg.fills = fills;
        h.onUserFills(msg);
        return true;
    }

    bool onCandle(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        CandleMsg msg;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "s") {
                if (!readString(field.value(), msg.coin)) { return false; }
            } else if (key == "i") {
                if (!readString(field.value(), msg.interval)) { return false; }
            } else if (key == "t") {
                if (!readInt(field.value(), msg.openTimeMs)) { return false; }
            } else if (key == "T") {
                if (!readInt(field.value(), msg.closeTimeMs)) { return false; }
            } else if (key == "o") {
                if (!readDecimal(field.value(), msg.open)) { return false; }
            } else if (key == "c") {
                if (!readDecimal(field.value(), msg.close)) { return false; }
            } else if (key == "h") {
                if (!readDecimal(field.value(), msg.high)) { return false; }
            } else if (key == "l") {
                if (!readDecimal(field.value(), msg.low)) { return false; }
            } else if (key == "v") {
                if (!readDecimal(field.value(), msg.volume)) { return false; }
            } else if (key == "n") {
                if (!readUint(field.value(), msg.trades)) { return false; }
            }
        }
        h.onCandle(msg);
        return true;
    }

    bool readFundingEntry(od::value value, UserFundingMsg& f) {
        od::object obj;
        if (value.get_object().get(obj) != simdjson::SUCCESS) {
            return false;
        }
        for (auto field : obj) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "time") {
                if (!readInt(field.value(), f.timeMs)) { return false; }
            } else if (key == "coin") {
                if (!readString(field.value(), f.coin)) { return false; }
            } else if (key == "usdc") {
                if (!readDecimal(field.value(), f.usdc)) { return false; }
            } else if (key == "szi") {
                if (!readDecimal(field.value(), f.szi)) { return false; }
            } else if (key == "fundingRate") {
                if (!readDecimal(field.value(), f.rate)) { return false; }
            }
        }
        return true;
    }

    bool readFills(od::value value, WsMessageHandler& h, bool isSnapshot) {
        od::array arr;
        if (value.get_array().get(arr) != simdjson::SUCCESS) {
            return false;
        }
        fills.clear();
        for (auto elem : arr) {
            od::value v;
            if (elem.get(v) != simdjson::SUCCESS) {
                return false;
            }
            FillMsg f;
            if (!readFill(v, f)) {
                return false;
            }
            fills.push_back(f);
        }
        UserFillsMsg msg;
        msg.isSnapshot = isSnapshot;
        msg.fills = fills;
        h.onUserFills(msg);
        return true;
    }

    // userEvents: {"channel":"user","data":{ fills | funding | liquidation | nonUserCancel }}
    bool onUserEvents(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "fills") {
                if (!readFills(field.value(), h, false)) { return false; }
            } else if (key == "funding") {
                UserFundingMsg f;
                if (!readFundingEntry(field.value(), f)) { return false; }
                fundings.assign(1, f);
                h.onUserFundings(fundings);
            } else if (key == "liquidation") {
                od::object obj;
                if (field.value().get_object().get(obj) != simdjson::SUCCESS) {
                    return false;
                }
                LiquidationMsg m;
                for (auto lf : obj) {
                    std::string_view k;
                    if (lf.escaped_key().get(k) != simdjson::SUCCESS) {
                        return false;
                    }
                    if (k == "lid") {
                        if (!readUint(lf.value(), m.lid)) { return false; }
                    } else if (k == "liquidator") {
                        if (!readString(lf.value(), m.liquidator)) { return false; }
                    } else if (k == "liquidated_user") {
                        if (!readString(lf.value(), m.liquidatedUser)) { return false; }
                    } else if (k == "liquidated_ntl_pos") {
                        if (!readDecimal(lf.value(), m.liquidatedNtlPos)) { return false; }
                    } else if (k == "liquidated_account_value") {
                        if (!readDecimal(lf.value(), m.liquidatedAccountValue)) { return false; }
                    }
                }
                h.onLiquidation(m);
            } else if (key == "nonUserCancel") {
                od::array arr;
                if (field.value().get_array().get(arr) != simdjson::SUCCESS) {
                    return false;
                }
                nonUserCancels.clear();
                for (auto elem : arr) {
                    od::object obj;
                    if (elem.get_object().get(obj) != simdjson::SUCCESS) {
                        return false;
                    }
                    NonUserCancelMsg c;
                    for (auto cf : obj) {
                        std::string_view k;
                        if (cf.escaped_key().get(k) != simdjson::SUCCESS) {
                            return false;
                        }
                        if (k == "coin") {
                            if (!readString(cf.value(), c.coin)) { return false; }
                        } else if (k == "oid") {
                            if (!readUint(cf.value(), c.oid)) { return false; }
                        }
                    }
                    nonUserCancels.push_back(c);
                }
                h.onNonUserCancels(nonUserCancels);
            }
        }
        return true;
    }

    bool onUserFundings(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        fundings.clear();
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "fundings") {
                od::array arr;
                if (field.value().get_array().get(arr) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto elem : arr) {
                    od::value v;
                    if (elem.get(v) != simdjson::SUCCESS) {
                        return false;
                    }
                    UserFundingMsg f;
                    if (!readFundingEntry(v, f)) {
                        return false;
                    }
                    fundings.push_back(f);
                }
            }
        }
        h.onUserFundings(fundings);
        return true;
    }

    bool onActiveAssetData(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        ActiveAssetDataMsg msg;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "user") {
                if (!readString(field.value(), msg.user)) { return false; }
            } else if (key == "coin") {
                if (!readString(field.value(), msg.coin)) { return false; }
            } else if (key == "markPx") {
                if (!readDecimal(field.value(), msg.markPx)) { return false; }
            } else if (key == "leverage") {
                od::object lev;
                if (field.value().get_object().get(lev) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto lf : lev) {
                    std::string_view k;
                    if (lf.escaped_key().get(k) != simdjson::SUCCESS) {
                        return false;
                    }
                    if (k == "type") {
                        std::string_view type;
                        if (!readString(lf.value(), type)) { return false; }
                        msg.isCross = type != "isolated";
                    } else if (k == "value") {
                        std::uint64_t v = 0;
                        if (!readUint(lf.value(), v)) { return false; }
                        msg.leverage = static_cast<std::uint32_t>(v);
                    }
                }
            } else if (key == "maxTradeSzs" || key == "availableToTrade") {
                od::array arr;
                if (field.value().get_array().get(arr) != simdjson::SUCCESS) {
                    return false;
                }
                std::size_t side = 0;
                for (auto elem : arr) {
                    od::value v;
                    if (elem.get(v) != simdjson::SUCCESS) {
                        return false;
                    }
                    Decimal value;
                    if (!readDecimal(v, value)) {
                        return false;
                    }
                    if (key == "maxTradeSzs") {
                        (side == 0 ? msg.maxTradeSzBuy : msg.maxTradeSzSell) = value;
                    } else {
                        (side == 0 ? msg.availableToTradeBuy : msg.availableToTradeSell) = value;
                    }
                    if (++side == 2) {
                        break;
                    }
                }
            }
        }
        h.onActiveAssetData(msg);
        return true;
    }

    bool onNotification(od::document& doc, WsMessageHandler& h) {
        NotificationMsg msg;
        if (doc["data"]["notification"].get_string().get(msg.text) != simdjson::SUCCESS) {
            return false;
        }
        h.onNotification(msg);
        return true;
    }

    bool onPost(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        PostResponseMsg msg;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "id") {
                if (!readUint(field.value(), msg.id)) { return false; }
            } else if (key == "response") {
                od::object resp;
                if (field.value().get_object().get(resp) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto rf : resp) {
                    std::string_view k;
                    if (rf.escaped_key().get(k) != simdjson::SUCCESS) {
                        return false;
                    }
                    if (k == "type") {
                        std::string_view t;
                        if (!readString(rf.value(), t)) { return false; }
                        msg.type = t == "action" ? PostResponseMsg::Type::Action
                                 : t == "info"   ? PostResponseMsg::Type::Info
                                                 : PostResponseMsg::Type::Error;
                    } else if (k == "payload") {
                        od::value v;
                        if (rf.value().get(v) != simdjson::SUCCESS) {
                            return false;
                        }
                        std::string_view text;
                        if (v.get_string().get(text) == simdjson::SUCCESS) {
                            msg.payload = text;
                        } else if (v.raw_json().get(text) == simdjson::SUCCESS) {
                            msg.payload = text;
                        } else {
                            return false;
                        }
                    }
                }
            }
        }
        h.onPostResponse(msg);
        return true;
    }

    bool onSubscriptionResponse(od::document& doc, WsMessageHandler& h) {
        od::object data;
        if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
            return false;
        }
        SubscriptionResponseMsg msg;
        for (auto field : data) {
            std::string_view key;
            if (field.escaped_key().get(key) != simdjson::SUCCESS) {
                return false;
            }
            if (key == "method") {
                if (!readString(field.value(), msg.method)) { return false; }
            } else if (key == "subscription") {
                od::object sub;
                if (field.value().get_object().get(sub) != simdjson::SUCCESS) {
                    return false;
                }
                for (auto sf : sub) {
                    std::string_view k;
                    if (sf.escaped_key().get(k) != simdjson::SUCCESS) {
                        return false;
                    }
                    if (k == "type") {
                        if (!readString(sf.value(), msg.subscriptionType)) { return false; }
                    } else if (k == "coin") {
                        (void)readString(sf.value(), msg.coin);
                    }
                }
            }
        }
        h.onSubscriptionResponse(msg);
        return true;
    }
};

struct WsMessageParser::Impl {
    std::vector<std::unique_ptr<ParseState>> states;  // one per active nesting level
    std::size_t depth{0};
    Stats stats{};

    bool parse(std::string_view frame, WsMessageHandler& handler) {
        if (states.size() <= depth) {
            states.push_back(std::make_unique<ParseState>());
        }
        ParseState& state = *states[depth];
        ++depth;
        const bool ok = state.parse(frame, handler, stats);
        --depth;
        return ok;
    }
};

WsMessageParser::WsMessageParser() : impl_(std::make_unique<Impl>()) {}
WsMessageParser::~WsMessageParser() = default;
WsMessageParser::WsMessageParser(WsMessageParser&&) noexcept = default;
WsMessageParser& WsMessageParser::operator=(WsMessageParser&&) noexcept = default;

bool WsMessageParser::parse(std::string_view frame, WsMessageHandler& handler) { return impl_->parse(frame, handler); }

const WsMessageParser::Stats& WsMessageParser::stats() const noexcept { return impl_->stats; }

}  // namespace hl
