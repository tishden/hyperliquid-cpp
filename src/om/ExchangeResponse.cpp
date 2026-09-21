// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/om/ExchangeResponse.h"

#include "json/Json.h"

namespace hl {

namespace {

void readCloid(const json::Value& v, ActionStatus& st) {
    if (v.field("cloid").isString()) {
        st.cloid = Cloid::parse(v.field("cloid").asString());
    }
}

}  // namespace

Result<ExchangeResponse> parseExchangeResponse(std::string_view body) {
    json::Document doc;
    if (!doc.parse(body)) {
        return Error{Error::Kind::Parse, 0, "exchange response is not valid JSON: " + std::string{body.substr(0, 200)}};
    }
    const auto root = doc.root();
    const auto status = root.field("status").asString();
    const auto response = root.field("response");
    if (status != "ok") {
        std::string message = response.isString() ? std::string{response.asString()} : root.dump();
        return Error{Error::Kind::Venue, 0, std::move(message)};
    }
    ExchangeResponse out;
    out.type = std::string{response.field("type").asString()};
    const auto data = response.field("data");
    // Some actions (twapOrder / twapCancel) report a single `status` instead of a `statuses` array.
    if (!data.field("statuses").isArray() && data.field("status").present()) {
        const auto single = data.field("status");
        ActionStatus st;
        if (single.isString()) {
            const auto text = single.asString();
            st.kind = text == "success" ? ActionStatus::Kind::Success : ActionStatus::Kind::Error;
            if (st.kind == ActionStatus::Kind::Error) {
                st.error = std::string{text};
            }
        } else if (auto err = single.field("error"); err.present()) {
            st.kind = ActionStatus::Kind::Error;
            st.error = err.isString() ? std::string{err.asString()} : err.dump();
        } else {
            st.kind = ActionStatus::Kind::Success;
            st.error = single.dump();  // e.g. {"running":{"twapId":…}} — kept verbatim for the caller
        }
        out.statuses.push_back(std::move(st));
        return out;
    }
    const auto statuses = data.field("statuses");
    for (auto item : statuses.array()) {
        ActionStatus st;
        if (item.isString()) {
            const auto s = item.asString();
            if (s == "success") {
                st.kind = ActionStatus::Kind::Success;
            } else if (s == "waitingForFill") {
                st.kind = ActionStatus::Kind::WaitingForFill;
            } else if (s == "waitingForTrigger") {
                st.kind = ActionStatus::Kind::WaitingForTrigger;
            } else {
                st.kind = ActionStatus::Kind::Error;
                st.error = std::string{s};
            }
        } else if (auto resting = item.field("resting"); resting.isObject()) {
            st.kind = ActionStatus::Kind::Resting;
            st.oid = resting.field("oid").asUint();
            readCloid(resting, st);
        } else if (auto filled = item.field("filled"); filled.isObject()) {
            st.kind = ActionStatus::Kind::Filled;
            st.oid = filled.field("oid").asUint();
            st.totalSz = filled.field("totalSz").asDecimal();
            st.avgPx = filled.field("avgPx").asDecimal();
            readCloid(filled, st);
        } else if (auto err = item.field("error"); err.present()) {
            st.kind = ActionStatus::Kind::Error;
            st.error = err.isString() ? std::string{err.asString()} : err.dump();
        } else {
            st.kind = ActionStatus::Kind::Error;
            st.error = "unrecognised status: " + item.dump();
        }
        out.statuses.push_back(std::move(st));
    }
    return out;
}

}  // namespace hl
