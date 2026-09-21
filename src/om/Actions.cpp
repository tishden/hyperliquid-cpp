// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/om/Actions.h"

#include <chrono>
#include <charconv>

#include "hl/core/Hex.h"
#include "hl/om/MsgPack.h"

namespace hl {

namespace {

void appendUint(std::string& s, std::uint64_t v) {
    char buf[20];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, res.ptr);
}

void appendBool(std::string& s, bool b) { s.append(b ? "true" : "false"); }

void packDecimal(MsgPackWriter& w, Decimal d) {
    char buf[Decimal::kMaxChars];
    w.str(std::string_view{buf, d.toChars(buf)});
}

constexpr char kHexDigits[] = "0123456789abcdef";

void appendHex32(std::string& s, const Hash256& h) {
    const std::size_t at = s.size();
    s.resize(at + 64);
    for (std::size_t i = 0; i < 32; ++i) {
        s[at + 2 * i] = kHexDigits[h[i] >> 4];
        s[at + 2 * i + 1] = kHexDigits[h[i] & 0x0F];
    }
}

void appendAddress(std::string& s, const Address& a) {
    s += '"';
    s += toHex(a);
    s += '"';
}

std::string_view tpslWire(TriggerSpec::Kind k) noexcept { return k == TriggerSpec::Kind::TakeProfit ? "tp" : "sl"; }

// Order wire key order (reference SDK order_request_to_order_wire): a b p s r t [c]
void packOrder(MsgPackWriter& w, const OrderWire& o) {
    w.mapHeader(o.cloid ? 7 : 6);
    w.str("a");
    w.uint(o.asset);
    w.str("b");
    w.boolean(o.isBuy);
    w.str("p");
    packDecimal(w, o.px);
    w.str("s");
    packDecimal(w, o.sz);
    w.str("r");
    w.boolean(o.reduceOnly);
    w.str("t");
    w.mapHeader(1);
    if (o.trigger) {
        w.str("trigger");
        w.mapHeader(3);
        w.str("isMarket");
        w.boolean(o.trigger->isMarket);
        w.str("triggerPx");
        packDecimal(w, o.trigger->triggerPx);
        w.str("tpsl");
        w.str(tpslWire(o.trigger->kind));
    } else {
        w.str("limit");
        w.mapHeader(1);
        w.str("tif");
        w.str(toString(o.tif));
    }
    if (o.cloid) {
        w.str("c");
        w.str(o.cloid->toString());
    }
}

void jsonOrder(std::string& j, const OrderWire& o) {
    j += R"({"a":)";
    appendUint(j, o.asset);
    j += R"(,"b":)";
    appendBool(j, o.isBuy);
    j += R"(,"p":")";
    o.px.appendTo(j);
    j += R"(","s":")";
    o.sz.appendTo(j);
    j += R"(","r":)";
    appendBool(j, o.reduceOnly);
    if (o.trigger) {
        j += R"(,"t":{"trigger":{"isMarket":)";
        appendBool(j, o.trigger->isMarket);
        j += R"(,"triggerPx":")";
        o.trigger->triggerPx.appendTo(j);
        j += R"(","tpsl":")";
        j += tpslWire(o.trigger->kind);
        j += R"("}})";
    } else {
        j += R"(,"t":{"limit":{"tif":")";
        j += toString(o.tif);
        j += R"("}})";
    }
    if (o.cloid) {
        j += R"(,"c":")";
        j += o.cloid->toString();
        j += '"';
    }
    j += '}';
}

}  // namespace

namespace actions {

EncodedAction order(std::span<const OrderWire> orders, OrderGrouping grouping,
                    const std::optional<BuilderFee>& builder) {
    const auto* priority = std::get_if<PriorityRate>(&grouping);
    const std::string_view groupingText = priority != nullptr ? "" : groupingWire(std::get<Grouping>(grouping));
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(builder ? 4 : 3);
    w.str("type");
    w.str("order");
    w.str("orders");
    w.arrayHeader(orders.size());
    for (const auto& o : orders) {
        packOrder(w, o);
    }
    w.str("grouping");
    if (priority != nullptr) {
        w.mapHeader(1);
        w.str("p");
        w.uint(priority->rate);
    } else {
        w.str(groupingText);
    }
    if (builder) {
        w.str("builder");
        w.mapHeader(2);
        w.str("b");
        w.str(toHex(builder->address));
        w.str("f");
        w.uint(builder->feeTenthsOfBps);
    }
    out.msgpack = w.release();

    out.json.reserve(64 + orders.size() * 120);
    out.json = R"({"type":"order","orders":[)";
    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (i != 0) {
            out.json += ',';
        }
        jsonOrder(out.json, orders[i]);
    }
    out.json += R"(],"grouping":)";
    if (priority != nullptr) {
        out.json += R"({"p":)";
        appendUint(out.json, priority->rate);
        out.json += '}';
    } else {
        out.json += '"';
        out.json += groupingText;
        out.json += '"';
    }
    if (builder) {
        out.json += R"(,"builder":{"b":)";
        appendAddress(out.json, builder->address);
        out.json += R"(,"f":)";
        appendUint(out.json, builder->feeTenthsOfBps);
        out.json += '}';
    }
    out.json += '}';
    return out;
}

EncodedAction cancel(std::span<const CancelWire> cancels, bool fast) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(fast ? 3 : 2);
    w.str("type");
    w.str("cancel");
    w.str("cancels");
    w.arrayHeader(cancels.size());
    out.json = R"({"type":"cancel","cancels":[)";
    for (std::size_t i = 0; i < cancels.size(); ++i) {
        const auto& c = cancels[i];
        w.mapHeader(2);
        w.str("a");
        w.uint(c.asset);
        w.str("o");
        w.uint(c.oid);
        if (i != 0) {
            out.json += ',';
        }
        out.json += R"({"a":)";
        appendUint(out.json, c.asset);
        out.json += R"(,"o":)";
        appendUint(out.json, c.oid);
        out.json += '}';
    }
    out.json += ']';
    if (fast) {
        w.str("f");
        w.boolean(true);
        out.json += R"(,"f":true)";
    }
    out.json += '}';
    out.msgpack = w.release();
    return out;
}

EncodedAction cancelByCloid(std::span<const CancelByCloidWire> cancels, bool fast) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(fast ? 3 : 2);
    w.str("type");
    w.str("cancelByCloid");
    w.str("cancels");
    w.arrayHeader(cancels.size());
    out.json = R"({"type":"cancelByCloid","cancels":[)";
    for (std::size_t i = 0; i < cancels.size(); ++i) {
        const auto& c = cancels[i];
        const std::string cloid = c.cloid.toString();
        w.mapHeader(2);
        w.str("asset");
        w.uint(c.asset);
        w.str("cloid");
        w.str(cloid);
        if (i != 0) {
            out.json += ',';
        }
        out.json += R"({"asset":)";
        appendUint(out.json, c.asset);
        out.json += R"(,"cloid":")";
        out.json += cloid;
        out.json += R"("})";
    }
    out.json += ']';
    if (fast) {
        w.str("f");
        w.boolean(true);
        out.json += R"(,"f":true)";
    }
    out.json += '}';
    out.msgpack = w.release();
    return out;
}

EncodedAction batchModify(std::span<const ModifyWire> modifies) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(2);
    w.str("type");
    w.str("batchModify");
    w.str("modifies");
    w.arrayHeader(modifies.size());
    out.json = R"({"type":"batchModify","modifies":[)";
    for (std::size_t i = 0; i < modifies.size(); ++i) {
        const auto& m = modifies[i];
        w.mapHeader(2);
        w.str("oid");
        if (i != 0) {
            out.json += ',';
        }
        out.json += R"({"oid":)";
        if (const auto* oid = std::get_if<std::uint64_t>(&m.target)) {
            w.uint(*oid);
            appendUint(out.json, *oid);
        } else {
            const std::string cloid = std::get<Cloid>(m.target).toString();
            w.str(cloid);
            out.json += '"';
            out.json += cloid;
            out.json += '"';
        }
        w.str("order");
        packOrder(w, m.order);
        out.json += R"(,"order":)";
        jsonOrder(out.json, m.order);
        out.json += '}';
    }
    out.json += "]}";
    out.msgpack = w.release();
    return out;
}

EncodedAction scheduleCancel(std::optional<std::uint64_t> timeMs) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(timeMs ? 2 : 1);
    w.str("type");
    w.str("scheduleCancel");
    out.json = R"({"type":"scheduleCancel")";
    if (timeMs) {
        w.str("time");
        w.uint(*timeMs);
        out.json += R"(,"time":)";
        appendUint(out.json, *timeMs);
    }
    out.json += '}';
    out.msgpack = w.release();
    return out;
}

EncodedAction updateLeverage(std::uint32_t asset, bool isCross, std::uint32_t leverage) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(4);
    w.str("type");
    w.str("updateLeverage");
    w.str("asset");
    w.uint(asset);
    w.str("isCross");
    w.boolean(isCross);
    w.str("leverage");
    w.uint(leverage);
    out.msgpack = w.release();
    out.json = R"({"type":"updateLeverage","asset":)";
    appendUint(out.json, asset);
    out.json += R"(,"isCross":)";
    appendBool(out.json, isCross);
    out.json += R"(,"leverage":)";
    appendUint(out.json, leverage);
    out.json += '}';
    return out;
}

Result<EncodedAction> updateIsolatedMargin(std::uint32_t asset, Decimal usdc) {
    // The venue takes the amount as an integer number of micro-USDC; Decimal carries 8 decimals.
    if (usdc.raw() % 100 != 0) {
        return Error{Error::Kind::Rejected, 0, "updateIsolatedMargin: " + usdc.toString() + " is finer than 1e-6 USDC"};
    }
    const std::int64_t micro = usdc.raw() / 100;
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(4);
    w.str("type");
    w.str("updateIsolatedMargin");
    w.str("asset");
    w.uint(asset);
    w.str("isBuy");
    w.boolean(true);  // constant in the venue's wire format; the sign of `ntli` adds or removes margin
    w.str("ntli");
    w.sint(micro);
    out.msgpack = w.release();
    out.json = R"({"type":"updateIsolatedMargin","asset":)";
    appendUint(out.json, asset);
    out.json += R"(,"isBuy":true,"ntli":)";
    if (micro < 0) {
        out.json += '-';
        appendUint(out.json, static_cast<std::uint64_t>(-(micro + 1)) + 1U);
    } else {
        appendUint(out.json, static_cast<std::uint64_t>(micro));
    }
    out.json += '}';
    return out;
}

EncodedAction noop() {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(1);
    w.str("type");
    w.str("noop");
    out.msgpack = w.release();
    out.json = R"({"type":"noop"})";
    return out;
}

EncodedAction reserveRequestWeight(std::uint64_t weight) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(2);
    w.str("type");
    w.str("reserveRequestWeight");
    w.str("weight");
    w.uint(weight);
    out.msgpack = w.release();
    out.json = R"({"type":"reserveRequestWeight","weight":)";
    appendUint(out.json, weight);
    out.json += '}';
    return out;
}

}  // namespace actions

std::string_view groupingWire(Grouping grouping) noexcept {
    switch (grouping) {
        case Grouping::Na: return "na";
        case Grouping::NormalTpsl: return "normalTpsl";
        case Grouping::PositionTpsl: return "positionTpsl";
    }
    return "na";
}

std::uint64_t NonceGenerator::next() noexcept {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::uint64_t prev = last_.load(std::memory_order_relaxed);
    std::uint64_t candidate = 0;
    do {
        candidate = now > prev ? now : prev + 1;
    } while (!last_.compare_exchange_weak(prev, candidate, std::memory_order_relaxed));
    return candidate;
}

void RequestBuilder::appendPayload(std::string& body, const EncodedAction& action, std::uint64_t nonce,
                                   std::optional<std::uint64_t> expiresAfter) const {
    const Signature sig = signer_.signL1Action(action.msgpack, vault_, nonce, expiresAfter, isMainnet_);
    body += R"({"action":)";
    body += action.json;
    body += R"(,"nonce":)";
    appendUint(body, nonce);
    body += R"(,"signature":{"r":"0x)";
    appendHex32(body, sig.r);
    body += R"(","s":"0x)";
    appendHex32(body, sig.s);
    body += R"(","v":)";
    appendUint(body, sig.v);
    body += R"(},"vaultAddress":)";
    if (vault_) {
        body += '"';
        body += toHex(*vault_);
        body += '"';
    } else {
        body += "null";
    }
    if (expiresAfter) {
        body += R"(,"expiresAfter":)";
        appendUint(body, *expiresAfter);
    }
    body += '}';
}

std::string RequestBuilder::payload(const EncodedAction& action, std::uint64_t nonce,
                                    std::optional<std::uint64_t> expiresAfter) const {
    std::string body;
    body.reserve(action.json.size() + 256);
    appendPayload(body, action, nonce, expiresAfter);
    return body;
}

std::string RequestBuilder::wsPostAction(std::uint64_t requestId, const EncodedAction& action, std::uint64_t nonce,
                                         std::optional<std::uint64_t> expiresAfter) const {
    std::string frame;
    frame.reserve(action.json.size() + 320);
    frame += R"({"method":"post","id":)";
    appendUint(frame, requestId);
    frame += R"(,"request":{"type":"action","payload":)";
    appendPayload(frame, action, nonce, expiresAfter);
    frame += "}}";
    return frame;
}

std::string RequestBuilder::wsPost(std::uint64_t requestId, std::string_view payloadJson) {
    std::string frame;
    frame.reserve(payloadJson.size() + 64);
    frame += R"({"method":"post","id":)";
    appendUint(frame, requestId);
    frame += R"(,"request":{"type":"action","payload":)";
    frame += payloadJson;
    frame += "}}";
    return frame;
}

std::string RequestBuilder::wsInfo(std::uint64_t requestId, std::string_view infoJson) {
    std::string frame;
    frame.reserve(infoJson.size() + 64);
    frame += R"({"method":"post","id":)";
    appendUint(frame, requestId);
    frame += R"(,"request":{"type":"info","payload":)";
    frame += infoJson;
    frame += "}}";
    return frame;
}

}  // namespace hl
