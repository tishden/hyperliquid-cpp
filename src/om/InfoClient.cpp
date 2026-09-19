#include "hl/om/InfoClient.h"

#include "json/Json.h"

namespace hl {

namespace {

constexpr std::string_view kInfoPath = "/info";

Side parseSide(std::string_view s) noexcept { return !s.empty() && s[0] == 'B' ? Side::Buy : Side::Sell; }

OpenOrder parseOrder(const json::Value& v) {
    OpenOrder o;
    o.coin = std::string{v.field("coin").asString()};
    o.side = parseSide(v.field("side").asString());
    o.limitPx = v.field("limitPx").asDecimal();
    o.sz = v.field("sz").asDecimal();
    o.origSz = v.field("origSz").asDecimal();
    o.oid = v.field("oid").asUint();
    o.timestampMs = v.field("timestamp").asInt();
    if (v.field("cloid").isString()) {
        o.cloid = Cloid::parse(v.field("cloid").asString());
    }
    o.reduceOnly = v.field("reduceOnly").asBool();
    o.isTrigger = v.field("isTrigger").asBool();
    o.triggerPx = v.field("triggerPx").asDecimal();
    o.orderType = std::string{v.field("orderType").asString()};
    o.tif = std::string{v.field("tif").asString()};
    return o;
}

Fill parseFill(const json::Value& v) {
    Fill f;
    f.coin = std::string{v.field("coin").asString()};
    f.side = parseSide(v.field("side").asString());
    f.px = v.field("px").asDecimal();
    f.sz = v.field("sz").asDecimal();
    f.timeMs = v.field("time").asInt();
    f.oid = v.field("oid").asUint();
    f.tid = v.field("tid").asUint();
    if (v.field("cloid").isString()) {
        f.cloid = Cloid::parse(v.field("cloid").asString());
    }
    f.crossed = v.field("crossed").asBool();
    f.fee = v.field("fee").asDecimal();
    f.feeToken = std::string{v.field("feeToken").asString()};
    f.closedPnl = v.field("closedPnl").asDecimal();
    f.startPosition = v.field("startPosition").asDecimal();
    f.dir = std::string{v.field("dir").asString()};
    f.hash = std::string{v.field("hash").asString()};
    return f;
}

void readLevels(const json::Value& arr, std::vector<BookLevel>& out) {
    for (auto lvl : arr.array()) {
        out.push_back(BookLevel{lvl.field("px").asDecimal(), lvl.field("sz").asDecimal(),
                                static_cast<std::uint32_t>(lvl.field("n").asUint())});
    }
}

// Issue an info request and hand the parsed document to `parse`, which returns Result<T>.
template <typename T, typename Parse>
void request(HttpClient& http, std::string body, std::function<void(const Result<T>&)> callback, Parse parse) {
    http.postJson(kInfoPath, std::move(body),
                  [callback = std::move(callback), parse = std::move(parse)](const Error& err, const HttpResponse& resp) {
                      if (err) {
                          Error e = err;
                          if (!resp.body.empty()) {
                              e.message += ": " + resp.body.substr(0, 300);
                          }
                          callback(Result<T>{e});
                          return;
                      }
                      json::Document doc;
                      if (!doc.parse(resp.body)) {
                          callback(Result<T>{Error{Error::Kind::Parse, resp.status,
                                                   "info: invalid JSON: " + resp.body.substr(0, 300)}});
                          return;
                      }
                      callback(parse(doc.root()));
                  });
}

std::string userRequest(std::string_view type, const Address& user) {
    return std::string{R"({"type":")"} + std::string{type} + R"(","user":")" + toHex(user) + R"("})";
}

}  // namespace

Fill Fill::from(const FillMsg& m) {
    Fill f;
    f.coin = std::string{m.coin};
    f.side = m.side;
    f.px = m.px;
    f.sz = m.sz;
    f.timeMs = m.timeMs;
    f.oid = m.oid;
    f.tid = m.tid;
    f.cloid = m.cloid;
    f.crossed = m.crossed;
    f.fee = m.fee;
    f.feeToken = std::string{m.feeToken};
    f.closedPnl = m.closedPnl;
    f.startPosition = m.startPosition;
    f.dir = std::string{m.dir};
    f.hash = std::string{m.hash};
    return f;
}

InfoClient::InfoClient(EventLoop& loop, Network network, HttpClientOptions options)
    : http_(loop, std::string{restUrl(network)}, std::move(options)) {}

InfoClient::InfoClient(EventLoop& loop, std::string baseUrl, HttpClientOptions options)
    : http_(loop, std::move(baseUrl), std::move(options)) {}

void InfoClient::assets(bool includeSpot, Callback<AssetRegistry> callback) {
    http_.postJson(kInfoPath, R"({"type":"meta"})",
                   [this, includeSpot, callback = std::move(callback)](const Error& err, const HttpResponse& resp) {
                       if (err) {
                           callback(Result<AssetRegistry>{err});
                           return;
                       }
                       AssetRegistry registry;
                       if (auto r = registry.loadPerpMeta(resp.body); !r) {
                           callback(Result<AssetRegistry>{r.error()});
                           return;
                       }
                       if (!includeSpot) {
                           callback(Result<AssetRegistry>{std::move(registry)});
                           return;
                       }
                       http_.postJson(kInfoPath, R"({"type":"spotMeta"})",
                                      [registry = std::move(registry), callback](const Error& e2,
                                                                                const HttpResponse& r2) mutable {
                                          if (e2) {
                                              callback(Result<AssetRegistry>{e2});
                                              return;
                                          }
                                          if (auto r = registry.loadSpotMeta(r2.body); !r) {
                                              callback(Result<AssetRegistry>{r.error()});
                                              return;
                                          }
                                          callback(Result<AssetRegistry>{std::move(registry)});
                                      });
                   });
}

void InfoClient::clearinghouseState(const Address& user, Callback<AccountState> callback) {
    request<AccountState>(http_, userRequest("clearinghouseState", user), std::move(callback),
                          [](const json::Value& root) -> Result<AccountState> {
                              if (!root.isObject()) {
                                  return Error{Error::Kind::Parse, 0, "clearinghouseState: unexpected response"};
                              }
                              AccountState s;
                              const auto summary = root.field("marginSummary");
                              s.accountValue = summary.field("accountValue").asDecimal();
                              s.totalNtlPos = summary.field("totalNtlPos").asDecimal();
                              s.totalRawUsd = summary.field("totalRawUsd").asDecimal();
                              s.totalMarginUsed = summary.field("totalMarginUsed").asDecimal();
                              s.withdrawable = root.field("withdrawable").asDecimal();
                              s.timeMs = root.field("time").asInt();
                              for (auto ap : root.field("assetPositions").array()) {
                                  const auto p = ap.field("position");
                                  Position pos;
                                  pos.coin = std::string{p.field("coin").asString()};
                                  pos.szi = p.field("szi").asDecimal();
                                  pos.entryPx = p.field("entryPx").asDecimal();
                                  pos.positionValue = p.field("positionValue").asDecimal();
                                  pos.unrealizedPnl = p.field("unrealizedPnl").asDecimal();
                                  pos.returnOnEquity = p.field("returnOnEquity").asDecimal();
                                  if (p.field("liquidationPx").isString()) {
                                      pos.liquidationPx = p.field("liquidationPx").asDecimal();
                                  }
                                  pos.marginUsed = p.field("marginUsed").asDecimal();
                                  const auto lev = p.field("leverage");
                                  pos.leverage = static_cast<std::uint32_t>(lev.field("value").asUint());
                                  pos.isCross = lev.field("type").asString() != "isolated";
                                  s.positions.push_back(std::move(pos));
                              }
                              return s;
                          });
}

void InfoClient::openOrders(const Address& user, Callback<std::vector<OpenOrder>> callback) {
    request<std::vector<OpenOrder>>(http_, userRequest("frontendOpenOrders", user), std::move(callback),
                                    [](const json::Value& root) -> Result<std::vector<OpenOrder>> {
                                        if (!root.isArray()) {
                                            return Error{Error::Kind::Parse, 0, "openOrders: expected array"};
                                        }
                                        std::vector<OpenOrder> out;
                                        for (auto o : root.array()) {
                                            out.push_back(parseOrder(o));
                                        }
                                        return out;
                                    });
}

namespace {

Result<OrderStatusInfo> parseOrderStatus(const json::Value& root) {
    OrderStatusInfo info;
    const auto status = root.field("status").asString();
    if (status != "order") {
        info.found = false;
        info.statusText = std::string{status};
        return info;
    }
    const auto wrapper = root.field("order");
    info.found = true;
    info.order = parseOrder(wrapper.field("order"));
    info.statusText = std::string{wrapper.field("status").asString()};
    info.status = parseOrderUpdateStatus(info.statusText);
    info.statusTimestampMs = wrapper.field("statusTimestamp").asInt();
    return info;
}

}  // namespace

void InfoClient::orderStatus(const Address& user, const Cloid& cloid, Callback<OrderStatusInfo> callback) {
    std::string body = R"({"type":"orderStatus","user":")" + toHex(user) + R"(","oid":")" + cloid.toString() + R"("})";
    request<OrderStatusInfo>(http_, std::move(body), std::move(callback), parseOrderStatus);
}

void InfoClient::orderStatus(const Address& user, std::uint64_t oid, Callback<OrderStatusInfo> callback) {
    std::string body = R"({"type":"orderStatus","user":")" + toHex(user) + R"(","oid":)" + std::to_string(oid) + "}";
    request<OrderStatusInfo>(http_, std::move(body), std::move(callback), parseOrderStatus);
}

void InfoClient::userRole(const Address& user, Callback<UserRole> callback) {
    request<UserRole>(http_, userRequest("userRole", user), std::move(callback),
                      [](const json::Value& root) -> Result<UserRole> {
                          if (!root.isObject()) {
                              return Error{Error::Kind::Parse, 0, "userRole: unexpected response"};
                          }
                          UserRole out;
                          out.role = std::string{root.field("role").asString()};
                          const auto master = root.field("data").field("user");
                          if (master.isString()) {
                              out.master = parseAddress(master.asString());
                          }
                          return out;
                      });
}

void InfoClient::userFills(const Address& user, Callback<std::vector<Fill>> callback) {
    request<std::vector<Fill>>(http_, userRequest("userFills", user), std::move(callback),
                               [](const json::Value& root) -> Result<std::vector<Fill>> {
                                   if (!root.isArray()) {
                                       return Error{Error::Kind::Parse, 0, "userFills: expected array"};
                                   }
                                   std::vector<Fill> out;
                                   for (auto f : root.array()) {
                                       out.push_back(parseFill(f));
                                   }
                                   return out;
                               });
}

void InfoClient::l2Book(std::string_view coin, Callback<L2Snapshot> callback) {
    std::string body = R"({"type":"l2Book","coin":")" + std::string{coin} + R"("})";
    request<L2Snapshot>(http_, std::move(body), std::move(callback), [](const json::Value& root) -> Result<L2Snapshot> {
        const auto levels = root.field("levels");
        if (!levels.isArray()) {
            return Error{Error::Kind::Parse, 0, "l2Book: missing levels"};
        }
        L2Snapshot snap;
        snap.coin = std::string{root.field("coin").asString()};
        snap.timeMs = root.field("time").asInt();
        std::size_t side = 0;
        for (auto lv : levels.array()) {
            readLevels(lv, side == 0 ? snap.bids : snap.asks);
            if (++side == 2) {
                break;
            }
        }
        return snap;
    });
}

void InfoClient::allMids(Callback<std::vector<std::pair<std::string, Decimal>>> callback) {
    using Mids = std::vector<std::pair<std::string, Decimal>>;
    http_.postJson(kInfoPath, R"({"type":"allMids"})",
                   [callback = std::move(callback)](const Error& err, const HttpResponse& resp) {
                       if (err) {
                           callback(Result<Mids>{err});
                           return;
                       }
                       simdjson::dom::parser parser;
                       simdjson::dom::object obj;
                       if (parser.parse(resp.body).get_object().get(obj) != simdjson::SUCCESS) {
                           callback(Result<Mids>{Error{Error::Kind::Parse, 0, "allMids: expected object"}});
                           return;
                       }
                       Mids mids;
                       for (auto [key, value] : obj) {
                           std::string_view text;
                           if (value.get_string().get(text) == simdjson::SUCCESS) {
                               mids.emplace_back(std::string{key}, Decimal::parseOrZero(text));
                           }
                       }
                       callback(Result<Mids>{std::move(mids)});
                   });
}

void InfoClient::spotBalances(const Address& user, Callback<std::vector<SpotBalance>> callback) {
    request<std::vector<SpotBalance>>(http_, userRequest("spotClearinghouseState", user), std::move(callback),
                                      [](const json::Value& root) -> Result<std::vector<SpotBalance>> {
                                          std::vector<SpotBalance> out;
                                          for (auto b : root.field("balances").array()) {
                                              SpotBalance sb;
                                              sb.coin = std::string{b.field("coin").asString()};
                                              sb.token = static_cast<std::uint32_t>(b.field("token").asUint());
                                              sb.total = b.field("total").asDecimal();
                                              sb.hold = b.field("hold").asDecimal();
                                              sb.entryNtl = b.field("entryNtl").asDecimal();
                                              out.push_back(std::move(sb));
                                          }
                                          return out;
                                      });
}

void InfoClient::rateLimit(const Address& user, Callback<RateLimitStatus> callback) {
    request<RateLimitStatus>(http_, userRequest("userRateLimit", user), std::move(callback),
                             [](const json::Value& root) -> Result<RateLimitStatus> {
                                 if (!root.isObject()) {
                                     return Error{Error::Kind::Parse, 0, "userRateLimit: unexpected response"};
                                 }
                                 RateLimitStatus out;
                                 out.cumVlm = root.field("cumVlm").asDecimal();
                                 out.requestsUsed = root.field("nRequestsUsed").asUint();
                                 out.requestsCap = root.field("nRequestsCap").asUint();
                                 return out;
                             });
}

void InfoClient::userFillsByTime(const Address& user, std::int64_t startTimeMs, std::int64_t endTimeMs,
                                 Callback<std::vector<Fill>> callback) {
    std::string body = R"({"type":"userFillsByTime","user":")" + toHex(user) + R"(","startTime":)" +
                       std::to_string(startTimeMs);
    if (endTimeMs > 0) {
        body += R"(,"endTime":)" + std::to_string(endTimeMs);
    }
    body += '}';
    request<std::vector<Fill>>(http_, std::move(body), std::move(callback),
                               [](const json::Value& root) -> Result<std::vector<Fill>> {
                                   if (!root.isArray()) {
                                       return Error{Error::Kind::Parse, 0, "userFillsByTime: expected array"};
                                   }
                                   std::vector<Fill> out;
                                   for (auto f : root.array()) {
                                       out.push_back(parseFill(f));
                                   }
                                   return out;
                               });
}

void InfoClient::userFunding(const Address& user, std::int64_t startTimeMs, std::int64_t endTimeMs,
                             Callback<std::vector<FundingPayment>> callback) {
    std::string body = R"({"type":"userFunding","user":")" + toHex(user) + R"(","startTime":)" +
                       std::to_string(startTimeMs);
    if (endTimeMs > 0) {
        body += R"(,"endTime":)" + std::to_string(endTimeMs);
    }
    body += '}';
    request<std::vector<FundingPayment>>(http_, std::move(body), std::move(callback),
                                         [](const json::Value& root) -> Result<std::vector<FundingPayment>> {
                                             if (!root.isArray()) {
                                                 return Error{Error::Kind::Parse, 0, "userFunding: expected array"};
                                             }
                                             std::vector<FundingPayment> out;
                                             for (auto e : root.array()) {
                                                 const auto delta = e.field("delta");
                                                 FundingPayment p;
                                                 p.timeMs = e.field("time").asInt();
                                                 p.hash = std::string{e.field("hash").asString()};
                                                 p.coin = std::string{delta.field("coin").asString()};
                                                 p.usdc = delta.field("usdc").asDecimal();
                                                 p.szi = delta.field("szi").asDecimal();
                                                 p.rate = delta.field("fundingRate").asDecimal();
                                                 out.push_back(std::move(p));
                                             }
                                             return out;
                                         });
}

void InfoClient::historicalOrders(const Address& user, Callback<std::vector<OrderStatusInfo>> callback) {
    request<std::vector<OrderStatusInfo>>(http_, userRequest("historicalOrders", user), std::move(callback),
                                          [](const json::Value& root) -> Result<std::vector<OrderStatusInfo>> {
                                              if (!root.isArray()) {
                                                  return Error{Error::Kind::Parse, 0, "historicalOrders: expected array"};
                                              }
                                              std::vector<OrderStatusInfo> out;
                                              for (auto e : root.array()) {
                                                  OrderStatusInfo info;
                                                  info.found = true;
                                                  info.order = parseOrder(e.field("order"));
                                                  info.statusText = std::string{e.field("status").asString()};
                                                  info.status = parseOrderUpdateStatus(info.statusText);
                                                  info.statusTimestampMs = e.field("statusTimestamp").asInt();
                                                  out.push_back(std::move(info));
                                              }
                                              return out;
                                          });
}

void InfoClient::perpContexts(Callback<std::vector<PerpContext>> callback) {
    request<std::vector<PerpContext>>(http_, R"({"type":"metaAndAssetCtxs"})", std::move(callback),
                                      [](const json::Value& root) -> Result<std::vector<PerpContext>> {
                                          // Response is [ {universe:[…]}, [ctx…] ] — the arrays are parallel.
                                          if (!root.isArray()) {
                                              return Error{Error::Kind::Parse, 0, "metaAndAssetCtxs: expected array"};
                                          }
                                          std::vector<PerpContext> out;
                                          std::vector<json::Value> parts;
                                          for (auto part : root.array()) {
                                              parts.push_back(part);
                                          }
                                          if (parts.size() != 2) {
                                              return Error{Error::Kind::Parse, 0, "metaAndAssetCtxs: expected two elements"};
                                          }
                                          std::vector<json::Value> ctxs;
                                          for (auto c : parts[1].array()) {
                                              ctxs.push_back(c);
                                          }
                                          std::uint32_t index = 0;
                                          for (auto u : parts[0].field("universe").array()) {
                                              PerpContext p;
                                              p.coin = std::string{u.field("name").asString()};
                                              p.asset = index;
                                              p.szDecimals = static_cast<int>(u.field("szDecimals").asInt());
                                              p.maxLeverage = static_cast<std::uint32_t>(u.field("maxLeverage").asUint());
                                              if (index < ctxs.size()) {
                                                  const auto& c = ctxs[index];
                                                  p.funding = c.field("funding").asDecimal();
                                                  p.openInterest = c.field("openInterest").asDecimal();
                                                  p.premium = c.field("premium").asDecimal();
                                                  p.oraclePx = c.field("oraclePx").asDecimal();
                                                  p.markPx = c.field("markPx").asDecimal();
                                                  p.midPx = c.field("midPx").asDecimal();
                                                  p.prevDayPx = c.field("prevDayPx").asDecimal();
                                                  p.dayNtlVlm = c.field("dayNtlVlm").asDecimal();
                                                  p.dayBaseVlm = c.field("dayBaseVlm").asDecimal();
                                                  std::size_t side = 0;
                                                  for (auto px : c.field("impactPxs").array()) {
                                                      (side == 0 ? p.impactBid : p.impactAsk) = px.asDecimal();
                                                      if (++side == 2) {
                                                          break;
                                                      }
                                                  }
                                              }
                                              out.push_back(std::move(p));
                                              ++index;
                                          }
                                          return out;
                                      });
}

void InfoClient::fundingHistory(std::string_view coin, std::int64_t startTimeMs, std::int64_t endTimeMs,
                                Callback<std::vector<FundingRate>> callback) {
    std::string body = R"({"type":"fundingHistory","coin":")" + std::string{coin} + R"(","startTime":)" +
                       std::to_string(startTimeMs);
    if (endTimeMs > 0) {
        body += R"(,"endTime":)" + std::to_string(endTimeMs);
    }
    body += '}';
    request<std::vector<FundingRate>>(http_, std::move(body), std::move(callback),
                                      [](const json::Value& root) -> Result<std::vector<FundingRate>> {
                                          if (!root.isArray()) {
                                              return Error{Error::Kind::Parse, 0, "fundingHistory: expected array"};
                                          }
                                          std::vector<FundingRate> out;
                                          for (auto e : root.array()) {
                                              FundingRate f;
                                              f.coin = std::string{e.field("coin").asString()};
                                              f.rate = e.field("fundingRate").asDecimal();
                                              f.premium = e.field("premium").asDecimal();
                                              f.timeMs = e.field("time").asInt();
                                              out.push_back(std::move(f));
                                          }
                                          return out;
                                      });
}

void InfoClient::predictedFundings(Callback<std::vector<PredictedFunding>> callback) {
    request<std::vector<PredictedFunding>>(
        http_, R"({"type":"predictedFundings"})", std::move(callback),
        [](const json::Value& root) -> Result<std::vector<PredictedFunding>> {
            // [[coin, [[venue, {fundingRate, nextFundingTime, fundingIntervalHours}], …]], …]
            if (!root.isArray()) {
                return Error{Error::Kind::Parse, 0, "predictedFundings: expected array"};
            }
            std::vector<PredictedFunding> out;
            for (auto entry : root.array()) {
                std::string coin;
                std::size_t position = 0;
                for (auto part : entry.array()) {
                    if (position == 0) {
                        coin = std::string{part.asString()};
                    } else {
                        for (auto venueEntry : part.array()) {
                            PredictedFunding pf;
                            pf.coin = coin;
                            std::size_t inner = 0;
                            for (auto field : venueEntry.array()) {
                                if (inner == 0) {
                                    pf.venue = std::string{field.asString()};
                                } else {
                                    pf.rate = field.field("fundingRate").asDecimal();
                                    pf.nextFundingTimeMs = field.field("nextFundingTime").asInt();
                                    pf.intervalHours = static_cast<int>(field.field("fundingIntervalHours").asInt());
                                }
                                ++inner;
                            }
                            if (!pf.venue.empty()) {
                                out.push_back(std::move(pf));
                            }
                        }
                    }
                    ++position;
                }
            }
            return out;
        });
}

void InfoClient::candles(std::string_view coin, std::string_view interval, std::int64_t startTimeMs,
                         std::int64_t endTimeMs, Callback<std::vector<Candle>> callback) {
    std::string body = R"({"type":"candleSnapshot","req":{"coin":")" + std::string{coin} + R"(","interval":")" +
                       std::string{interval} + R"(","startTime":)" + std::to_string(startTimeMs);
    if (endTimeMs > 0) {
        body += R"(,"endTime":)" + std::to_string(endTimeMs);
    }
    body += "}}";
    request<std::vector<Candle>>(http_, std::move(body), std::move(callback),
                                 [](const json::Value& root) -> Result<std::vector<Candle>> {
                                     if (!root.isArray()) {
                                         return Error{Error::Kind::Parse, 0, "candleSnapshot: expected array"};
                                     }
                                     std::vector<Candle> out;
                                     for (auto e : root.array()) {
                                         Candle c;
                                         c.openTimeMs = e.field("t").asInt();
                                         c.closeTimeMs = e.field("T").asInt();
                                         c.coin = std::string{e.field("s").asString()};
                                         c.interval = std::string{e.field("i").asString()};
                                         c.open = e.field("o").asDecimal();
                                         c.close = e.field("c").asDecimal();
                                         c.high = e.field("h").asDecimal();
                                         c.low = e.field("l").asDecimal();
                                         c.volume = e.field("v").asDecimal();
                                         c.trades = e.field("n").asUint();
                                         out.push_back(std::move(c));
                                     }
                                     return out;
                                 });
}

void InfoClient::raw(std::string requestJson, Callback<std::string> callback) {
    http_.postJson(kInfoPath, std::move(requestJson),
                   [callback = std::move(callback)](const Error& err, const HttpResponse& resp) {
                       if (err) {
                           Error e = err;
                           e.message += ": " + resp.body.substr(0, 300);
                           callback(Result<std::string>{e});
                           return;
                       }
                       callback(Result<std::string>{resp.body});
                   });
}

}  // namespace hl
