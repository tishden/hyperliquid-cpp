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
    std::string tmp;
    d.appendTo(tmp);
    w.str(tmp);
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

EncodedAction order(std::span<const OrderWire> orders, std::string_view grouping) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(3);
    w.str("type");
    w.str("order");
    w.str("orders");
    w.arrayHeader(orders.size());
    for (const auto& o : orders) {
        packOrder(w, o);
    }
    w.str("grouping");
    w.str(grouping);
    out.msgpack = w.release();

    out.json.reserve(64 + orders.size() * 120);
    out.json = R"({"type":"order","orders":[)";
    for (std::size_t i = 0; i < orders.size(); ++i) {
        if (i != 0) {
            out.json += ',';
        }
        jsonOrder(out.json, orders[i]);
    }
    out.json += R"(],"grouping":")";
    out.json += grouping;
    out.json += R"("})";
    return out;
}

EncodedAction cancel(std::span<const CancelWire> cancels) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(2);
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
    out.json += "]}";
    out.msgpack = w.release();
    return out;
}

EncodedAction cancelByCloid(std::span<const CancelByCloidWire> cancels) {
    EncodedAction out;
    MsgPackWriter w;
    w.mapHeader(2);
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
    out.json += "]}";
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

}  // namespace actions

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

std::string RequestBuilder::payload(const EncodedAction& action, std::uint64_t nonce,
                                    std::optional<std::uint64_t> expiresAfter) const {
    const Signature sig = signer_.signL1Action(action.msgpack, vault_, nonce, expiresAfter, isMainnet_);
    std::string body;
    body.reserve(action.json.size() + 256);
    body += R"({"action":)";
    body += action.json;
    body += R"(,"nonce":)";
    appendUint(body, nonce);
    body += R"(,"signature":{"r":"0x)";
    body += toHex(sig.r.data(), sig.r.size());
    body += R"(","s":"0x)";
    body += toHex(sig.s.data(), sig.s.size());
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
    return body;
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
