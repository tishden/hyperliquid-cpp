#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hl/core/Result.h"
#include "hl/core/Types.h"
#include "hl/net/HttpClient.h"
#include "hl/om/AssetRegistry.h"
#include "hl/om/Types.h"

namespace hl {

/**
 * @brief Asynchronous client for the read-only `POST /info` endpoint.
 *
 * All methods queue a request and return immediately; the callback runs on
 * the event-loop thread. Requests share one keep-alive connection and are
 * answered in submission order. Info requests are weight-limited by HL
 * (1 200 weight / minute per IP; most requests weigh 2–20).
 */
class InfoClient {
public:
    template <typename T>
    using Callback = std::function<void(const Result<T>&)>;

    InfoClient(EventLoop& loop, Network network, HttpClientOptions options = {});
    /// Custom base URL (e.g. a proxy or a local mock), such as `http://127.0.0.1:8080`.
    InfoClient(EventLoop& loop, std::string baseUrl, HttpClientOptions options = {});

    /// Perp universe (`meta`) and optionally spot (`spotMeta`) merged into one registry.
    void assets(bool includeSpot, Callback<AssetRegistry> callback);
    /// Perp margin summary and positions of @p user.
    void clearinghouseState(const Address& user, Callback<AccountState> callback);
    /// All resting orders of @p user (`frontendOpenOrders`: includes tif, reduceOnly, trigger info).
    void openOrders(const Address& user, Callback<std::vector<OpenOrder>> callback);
    /// Status of one order by client id.
    void orderStatus(const Address& user, const Cloid& cloid, Callback<OrderStatusInfo> callback);
    /// Status of one order by exchange id.
    void orderStatus(const Address& user, std::uint64_t oid, Callback<OrderStatusInfo> callback);
    /// Most recent fills of @p user (up to 2 000).
    void userFills(const Address& user, Callback<std::vector<Fill>> callback);
    /// One-shot L2 snapshot.
    void l2Book(std::string_view coin, Callback<L2Snapshot> callback);
    /// Mid price of every coin.
    void allMids(Callback<std::vector<std::pair<std::string, Decimal>>> callback);
    /// Send any info request body (e.g. `{"type":"fundingHistory",...}`) and receive the raw JSON.
    void raw(std::string requestJson, Callback<std::string> callback);

    [[nodiscard]] HttpClient& http() noexcept { return http_; }

private:
    HttpClient http_;
};

}  // namespace hl
