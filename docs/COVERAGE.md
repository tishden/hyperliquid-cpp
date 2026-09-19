# hyperliquid-cpp — API coverage

What this library covers of the Hyperliquid API, what it deliberately leaves out, and how each claim was
verified. It is the companion to [API.md](API.md), which documents the C++ surface; this document is about the
**venue** surface.

- [1. Summary and how to read the statuses](#1-summary-and-how-to-read-the-statuses)
- [2. `/info` requests](#2-info-requests)
- [3. `/exchange` actions](#3-exchange-actions)
- [4. WebSocket channels](#4-websocket-channels)
- [5. Deliberately excluded](#5-deliberately-excluded)
- [6. Known gaps](#6-known-gaps)
- [7. How this was verified](#7-how-this-was-verified)

---

## 1. Summary and how to read the statuses

The library targets one job: **run a trading strategy on Hyperliquid perps and spot** — quote, amend, cancel,
follow the order table, read the market. Everything on that path is typed, tested and exercised against the live
testnet. Everything off it — deployments, transfers, governance, staking — is reachable but unwrapped, or
excluded on purpose (see [§5](#5-deliberately-excluded)).

| Status | Meaning |
|---|---|
| **Typed** | A dedicated method with parsed, owning result types. Errors map onto `Error::Kind`; nothing is handed back as raw JSON. |
| **Raw** | Reachable without new code, but you handle the JSON (or the msgpack) yourself. Three escape hatches: `InfoClient::raw(body, cb)` for any `/info` request; `MarketDataClient::subscribeRaw(json)` with frames arriving in `WsMessageHandler::onUnhandled(channel, frame)` for any subscription; `ExchangeClient::submitAction(action, cb)` with an `EncodedAction` you build using `MsgPackWriter` for any L1 action. |
| **Not supported** | Cannot be done with this library as it stands — it needs code that does not exist yet. In practice this is exactly the set of EIP-712 *user-signed* actions ([§5](#5-deliberately-excluded)), because the signer only implements the L1 `actionHash` → `agentDigest` path. |

Two things follow from the **Raw** row that are worth stating plainly:

- A raw `/info` request is a first-class citizen. It shares the `InfoClient`'s keep-alive connection, its request
  ordering and its timeout handling; only the parsing is yours.
- A raw *action* is not. `submitAction` signs and sends whatever msgpack you hand it, so the field order,
  types and key names must match the venue's schema exactly or the recovered signer address will be wrong and
  the venue will answer `User or API Wallet 0x… does not exist.` Build new actions against a golden vector, the
  way `tests/actions_test.cpp` does.

---

## 2. `/info` requests

| Request | Status | Method |
|---|---|---|
| `meta` | Typed | `InfoClient::assets(false, cb)` → `AssetRegistry` |
| `spotMeta` | Typed | `InfoClient::assets(true, cb)` — merged into the same registry, asset ids `10000 + index` |
| `metaAndAssetCtxs` | Typed | `perpContexts(cb)` → `std::vector<PerpContext>`; the venue's two parallel arrays are zipped into one row per perp |
| `clearinghouseState` | Typed | `clearinghouseState(user, cb)` → `AccountState` (margin summary + positions) |
| `spotClearinghouseState` | Typed | `spotBalances(user, cb)` → `std::vector<SpotBalance>` |
| `frontendOpenOrders` | Typed | `openOrders(user, cb)` → `std::vector<OpenOrder>` (includes tif, reduceOnly, trigger info) |
| `orderStatus` | Typed | `orderStatus(user, cloid, cb)` and `orderStatus(user, oid, cb)` → `OrderStatusInfo` |
| `historicalOrders` | Typed | `historicalOrders(user, cb)` → terminal orders, most recent first |
| `userFills` | Typed | `userFills(user, cb)` → up to 2 000 most recent fills |
| `userFillsByTime` | Typed | `userFillsByTime(user, start, end, cb)` → oldest first; the way to recover fills missed while disconnected |
| `userFunding` | Typed | `userFunding(user, start, end, cb)` → `std::vector<FundingPayment>` |
| `userRateLimit` | Typed | `rateLimit(user, cb)` → `RateLimitStatus`; also polled automatically by `ExchangeClient` |
| `userRole` | Typed | `userRole(user, cb)` → `UserRole`; used internally for agent-wallet detection |
| `l2Book` | Typed | `l2Book(coin, cb)` → `L2Snapshot` (one-shot; prefer the `l2Book` subscription) |
| `allMids` | Typed | `allMids(cb)` → `(coin, mid)` pairs |
| `candleSnapshot` | Typed | `candles(coin, interval, start, end, cb)` → `std::vector<Candle>` |
| `fundingHistory` | Typed | `fundingHistory(coin, start, end, cb)` → `std::vector<FundingRate>` |
| `predictedFundings` | Typed | `predictedFundings(cb)` → one `PredictedFunding` per (coin, venue) |
| `openOrders` (the terse variant) | Raw | `frontendOpenOrders` is strictly richer and is what the typed method uses |
| `userNonFundingLedgerUpdates` | Raw | Deposits, withdrawals, transfers — not part of the trading loop |
| `userTwapSliceFills`, `twapHistory` | Raw | See the TWAP gap in [§6](#6-known-gaps) |
| `subAccounts`, `vaultDetails`, `userVaultEquities` | Raw | |
| `delegations`, `delegatorSummary`, `delegatorHistory`, `delegatorRewards` | Raw | Staking; out of scope, see [§5](#5-deliberately-excluded) |
| `spotDeployState`, `perpDeployAuctionStatus`, `perpDexs` | Raw | Deployment; out of scope |
| `tokenDetails`, `spotMetaAndAssetCtxs`, `maxBuilderFee`, `referral`, `extraAgents`, `isVip`, `portfolio`, `userToMultiSigSigners`, … | Raw | Anything not listed above is a `raw()` call away; the list of `/info` types is the venue's, not ours, and it grows |

All typed requests share one keep-alive connection, complete in submission order, and surface transport,
timeout, HTTP and parse failures through `Result<T>` ([API.md §5.6](API.md#56-infoclient)).

---

## 3. `/exchange` actions

Statuses here mean the same thing, with one addition: an action is only **Typed** when the library both builds
the wire form (`hl::actions::*`) and folds the response into the order table where that applies.

| Action | Status | Where |
|---|---|---|
| `order` | Typed | `actions::order(orders, grouping, builder)`; `ExchangeClient::placeOrder` / `placeOrders`. Covers limit and trigger orders, all four tifs, reduce-only, cloids, `normalTpsl` / `positionTpsl` grouping, the `{"p":rate}` priority fee and builder codes. |
| `cancel` (by oid) | Typed | `actions::cancel(cancels, fast)`; `cancelByOid`, `cancelAll` |
| `cancelByCloid` | Typed | `actions::cancelByCloid(cancels, fast)`; `cancel(cloid)`, `cancelAll` |
| `batchModify` | Typed | `actions::batchModify(modifies)`; `ExchangeClient::modify(cloid, px, sz, newTrigger)` |
| `scheduleCancel` | Typed | `actions::scheduleCancel(timeMs)`; `ExchangeClient::scheduleCancel`. Venue preconditions ($1 M volume, ≤ 10 triggers/day) are reported as `Error{Venue}`. |
| `updateLeverage` | Typed | `actions::updateLeverage(asset, isCross, leverage)` |
| `updateIsolatedMargin` | Typed | `actions::updateIsolatedMargin(asset, usdc)` — the only builder returning a `Result`, because sub-micro-USDC amounts are rejected locally |
| `noop` | Typed | `actions::noop()`; `ExchangeClient::noop()` — signing health check / nonce advance |
| `reserveRequestWeight` | Typed | `actions::reserveRequestWeight(weight)` |
| `modify` (single-order variant) | Raw | `batchModify` with one entry is equivalent and is what the client sends |
| `twapOrder`, `twapCancel` | Raw | The wire form must be built by hand; `parseExchangeResponse` already understands their single-`status` response shape ([§6](#6-known-gaps)) |
| `setReferrer`, `createSubAccount`, `subAccountTransfer`, `vaultTransfer`, `evmUserModify`, … | Raw | L1 actions — buildable with `MsgPackWriter` + `submitAction` |
| `approveAgent`, `approveBuilderFee`, `usdSend`, `spotSend`, `withdraw3`, `usdClassTransfer`, `tokenDelegate`, `cSignerAction`, `multiSig`, … | **Not supported** | EIP-712 user-signed actions — see [§5](#5-deliberately-excluded) |

**Signing and rate-limit notes that apply to every action.** The msgpack is what gets hashed, so every key name,
key order and integer type is load-bearing; `expiresAfter` is part of the hash when `actionExpiryMs` is set.
The venue charges one **IP-weight** unit per 40 order/cancel entries but one **address-budget** unit per entry
([API.md §5.10](API.md#510-rate-limits-and-request-budget)).

**Golden vectors and live tests.** `tests/actions_test.cpp` freezes the msgpack bytes of every typed action.
The expected bytes were generated with the official `hyperliquid-python-sdk`, with two exceptions: the
**priority fee** and the cancel **`fast` flag** postdate the pinned SDK version, so their vectors come from
`msgpack-python` applied to the documented action shape. Both were then accepted by the live testnet — the
priority fee is rejected *semantically* (no staking balance), which is itself proof that the action
deserialised. The distinction matters: an SDK-matched vector proves the encoding is what the reference
implementation produces, a hand-built one proves only that it is stable and plausible, and a live run proves
the venue agrees. [§7](#7-how-this-was-verified) says which applies per feature.

---

## 4. WebSocket channels

`MarketDataClient` and `ExchangeClient` share one decoder (`WsMessageParser`), so a channel that is typed is
typed for both.

| Subscription | Delivered on channel | Status | Callback / helper |
|---|---|---|---|
| `l2Book` | `l2Book` | Typed | `subscribeL2Book(coin, options)` → `onL2Book` + a maintained `OrderBook`; `nSigFigs` / `mantissa` aggregation supported |
| `bbo` | `bbo` | Typed | `subscribeBbo(coin)` → `onBbo`, overlaid onto the book when `applyBboToBooks` |
| `trades` | `trades` | Typed | `subscribeTrades(coin)` → `onTrades` (liquidations arrive here too) |
| `activeAssetCtx` | `activeAssetCtx` / `activeSpotAssetCtx` | Typed | `subscribeAssetCtx(coin)` → `onAssetCtx` |
| `allMids` | `allMids` | Typed | `subscribeAllMids()` → `onAllMids` |
| `candle` | `candle` | Typed | `subscribeCandle(coin, interval)` → `onCandle` |
| `orderUpdates` | `orderUpdates` | Typed | Subscribed by `ExchangeClient`; drives the order state machine |
| `userFills` | `userFills` | Typed | Subscribed by `ExchangeClient`; drives fills and positions |
| `userEvents` | **`user`** | Typed | `subscribeUserEvents(user)` / `ExchangeConfig::subscribeUserEvents`. One subscription, four payloads: `fills` → `onUserFills`, `funding` → `onUserFundings` / `ExchangeListener::onFunding`, `liquidation` → `onLiquidation`, `nonUserCancel` → `onNonUserCancels`. The channel name differing from the subscription name is a venue quirk, not a typo. |
| `userFundings` | `userFundings` | Typed | `subscribeUserFundings(user)` → `onUserFundings` |
| `activeAssetData` | `activeAssetData` | Typed | `subscribeActiveAssetData(user, coin)` → `onActiveAssetData` (leverage, max tradable size) |
| `notification` | `notification` | Typed | `subscribeNotifications(user)` → `onNotification` |
| `post` (request/response over WS) | `post` | Typed | `onPostResponse`; used internally for actions. **Info responses are parsed but not delivered** — see [§6](#6-known-gaps). |
| `subscriptionResponse`, `pong`, `error` | same | Typed | `onSubscriptionResponse`, `onPong`, `onVenueError` |
| `webData2` | `webData2` | Raw | The UI's aggregate frame; large and unstable in shape |
| `userNonFundingLedgerUpdates`, `userTwapSliceFills`, `userTwapHistory`, `userHistoricalOrders` | same | Raw | `subscribeRaw(json)` → `onUnhandled` |
| `fastAssetCtxs` | — | **Not supported** | Requires per-message DEFLATE — see [§5](#5-deliberately-excluded) |

A raw subscription is replayed on reconnect like any other; only decoding is left to you. Note that typed
helpers validate the coin name with `isValidCoinName` before building the JSON, while `subscribeRaw` does not —
you own that string.

---

## 5. Deliberately excluded

These are absent by decision, not by omission. Each is cheap to add later; none belongs in the hot path of a
trading process.

**EIP-712 user-signed actions.** Transfers (`usdSend`, `spotSend`, `usdClassTransfer`), withdrawals
(`withdraw3`), staking (`tokenDelegate`, `cDeposit`, `cWithdraw`) and the approval actions use a *different*
signing scheme from L1 actions: a full EIP-712 typed-data struct per action type, with its own domain and field
list, instead of the L1 `msgpack → keccak → phantom agent` path that `Signer` implements. Supporting them means
a typed-data encoder plus a hand-verified struct definition per action — and every one of them moves money.

The reasoning for leaving them out is not only cost:

- **Blast radius.** An agent (API) wallet — the recommended way to run this library — *cannot* withdraw or
  transfer at all. Implementing these actions would only be useful with a master key in the process, which is
  exactly the configuration a trading process should never have. Keeping them out keeps the worst outcome of a
  compromised trading host bounded by what an agent key can do: place and cancel orders.
- **Frequency.** They are operational, not algorithmic. Moving collateral happens at human cadence, and the UI
  or the Python SDK does it safely.

**`approveAgent` and `approveBuilderFee`** deserve a separate mention because the library *depends* on them:
an agent wallet must be approved before it can trade, and a builder code must be approved before
`ExchangeConfig::builderFee` is accepted. Both are one-time, per-account setup steps done in the Hyperliquid UI
(or with the Python SDK), and both are user-signed. Requiring them as a precondition is correct — a trading
library that could silently grant itself a new agent wallet would be a worse library.

**Multi-sig (`multiSig`, `convertToMultiSigUser`).** A multi-sig action wraps another action together with
signatures from several authorised signers. It implies key custody across processes or machines, which is an
orchestration problem rather than an SDK one, and it is incompatible with the single-`Signer` model here.

**Deployment, validator and HIP-4 actions** (`spotDeploy`, `perpDeploy`, `cSignerAction`,
`cValidatorAction`, HIP-4 builder-deployed perpetual management). These configure markets rather than trade on
them. They are governance-cadence operations with schemas that change as the feature evolves, and no strategy
needs them at runtime.

**`fastAssetCtxs`.** A lower-latency asset-context feed. Per the venue's documentation it is delivered
**DEFLATE-compressed** (WebSocket `permessage-deflate`) — *unverified here: nothing in this repository has
negotiated that channel*. The WebSocket stack is deliberately dependency-free and neither negotiates nor
implements compression, so supporting it means linking zlib or writing an inflater, and it changes frame
handling for every channel. `activeAssetCtx` carries the same fields uncompressed.

---

## 6. Known gaps

Things a user could reasonably expect that are missing. Effort estimates are for implementation **plus** tests
and docs, by someone already familiar with the codebase.

| Gap | What is missing | Effort | Notes |
|---|---|---|---|
| **HIP-3 `dex` parameter** | Builder-deployed perp DEXes put a `dex` field in `meta`, `clearinghouseState` and the order action's asset resolution. `AssetRegistry` only knows the primary perp universe and spot. | ~1 day | Needs a `dex` field on `AssetInfo` and `ExchangeConfig`, a second `meta` load, and asset-id namespacing. Worth doing once a HIP-3 market matters to the strategy. |
| **TWAP actions** | No `actions::twapOrder` / `twapCancel`, and no order-table integration for TWAP slices. | ~1 day | Two complications, not one. The *action* is a straightforward msgpack builder. The *response* has a different shape — a single `data.status` instead of a `statuses` array, carrying `{"running":{"twapId":…}}` — which `parseExchangeResponse` already handles ([API.md §5.9](API.md#59-exchangeresponse)), surfacing as a `Success` status with the object kept verbatim in `ActionStatus::error`. A real implementation wants a typed `twapId` and slice tracking through `userTwapSliceFills`. |
| **No `openOrders` / `clearinghouseState` / `spotClearinghouseState` push channels** | Position and balance state is seeded once at start-up and then maintained from fills. There is no subscription that re-synchronises it. | ~0.5 day per channel | The venue offers `webData2` (unstable shape) and periodic polling as the alternatives. In practice the fill-driven position is correct — the stale-fill rule in [API.md §5.2](API.md#52-orderstate-and-the-order-state-machine) exists precisely to keep it so — but a long-running process has no cheap way to *verify* it. A periodic `clearinghouseState` reconcile would close this. |
| **No pagination helpers** | `userFillsByTime`, `userFunding`, `fundingHistory` and `candles` are all row-capped by the venue (`userFills` is documented at 2 000; the other caps are the venue's and are not asserted here) and return a truncated window with no cursor handling. | ~0.5 day | The caller must page by advancing `startTime` past the last returned row and watching for a full page. A small `paginate(...)` helper that loops until a short page arrives would remove a footgun that silently produces incomplete history. |

---

## 7. How this was verified

Three independent levels of evidence. A feature is only claimed as working here if at least one applies, and the
strongest available is listed.

| Level | What it proves | Where |
|---|---|---|
| **Golden vector** | The bytes we sign are the bytes the venue expects. `…MatchesSdk` vectors were generated with the reference Python SDK; the others are frozen in-repo against a hand-checked encoding. | `tests/actions_test.cpp`, `tests/signer_test.cpp`, `tests/keccak_test.cpp` |
| **Mock venue** | The client's behaviour — state machine, reconciliation, batching, error fan-out — against a scripted server, including failure injection that is impractical live. | `tests/exchange_client_test.cpp`, `tests/info_client_test.cpp`, `tests/market_data_client_test.cpp`, `tests/ws_message_parser_test.cpp`, `tests/http_client_test.cpp` |
| **Live testnet** | The venue actually accepts it. Step-by-step, each with a pass/fail verdict and a deadline. | `examples/live_check` (see [TESTNET.md](TESTNET.md)) |

| Feature | Golden vector | Mock venue | Live testnet |
|---|---|---|---|
| `order` — limit, all tifs, cloid | ✅ `SingleOrderMatchesSdk`, `BatchOfTwoOrders{Mainnet,Testnet}` | ✅ `OrderLifecycleRestingPartialFilled`, `BatchPlacementMapsStatusesInOrder` | ✅ post-only resting order, batch of two |
| `order` — trigger (TP/SL) | ✅ `TriggerOrderMatchesSdk` | ✅ `ModifyKeepsTheTriggerSpecification` | ❌ not exercised |
| `order` — `normalTpsl` / `positionTpsl` grouping | ✅ `TpSlGroupingMatchesSdk` | ❌ | ❌ |
| `order` — `{"p":rate}` priority fee | ⚠️ `PriorityFeeGrouping` — hand-built from `msgpack-python`, no SDK golden | ❌ | ✅ optional step, run when a priority rate is configured; the venue deserialises it and rejects it for lack of a staking balance |
| `order` — builder code | ✅ `BuilderFeeMatchesSdk` | ❌ | ❌ needs an approved builder on the account |
| `cancel` / `cancelByCloid` | ✅ `CancelBatch`, `CancelByCloid` | ✅ `CancelByCloid`, `FailedCancelIsReconciled`, `CancelAllForCoin`, `CancelByOidForAnUnknownOrderTouchesNothing` | ✅ cancel by cloid, `cancelAll` |
| cancel `fast` flag (and its absence) | ⚠️ `FastCancelFlag` — hand-built from `msgpack-python`, no SDK golden | ✅ `CancelsCarryTheFastFlagExceptForTriggerOrders` | ⚠️ accepted by the venue, not separately asserted as a step |
| cancel chunking at 40 entries | ❌ | ✅ covered by `CancelAllForCoin` | ❌ |
| `batchModify` | ✅ `BatchModifyByOidAndCloid` | ✅ `ModifyKeepsCloidAndIgnoresStaleCancel`, `ModifyKeepsTheTriggerSpecification` | ✅ modify price and size in place |
| `scheduleCancel` | ✅ `ScheduleCancelSetAndClear` | ❌ | ✅ arm and clear |
| `updateLeverage` | ✅ `UpdateLeverage` | ❌ | ✅ |
| `updateIsolatedMargin` | ✅ `UpdateIsolatedMarginMatchesSdk` | ❌ | ❌ |
| `noop`, `reserveRequestWeight` | ✅ `NoopAndReserveRequestWeightMatchSdk` | ❌ | ❌ |
| `expiresAfter` in the action hash | ✅ `ExpiresAfterIsPartOfTheHash` | ❌ | ❌ |
| Vault / sub-account signing | ✅ `VaultOnTestnet`, `PayloadWithVaultAndExpiry` | ❌ | ❌ needs a testnet vault |
| Agent-wallet detection and stream adoption | ❌ | ✅ `AgentWalletUsesMasterAccountForStreams`, `AgentWalletWithoutAccountAddressAdoptsItsMaster`, `AgentWalletWithWrongAccountAddressIsReported`, `AgentAdoptionResubscribesEveryUserChannel` | ✅ implicitly — the check runs with an agent wallet |
| Adoption of pre-existing orders | ❌ | ✅ `AdoptsOrdersAlreadyOpenOnTheVenue` | ❌ |
| Batch-wide venue error fan-out | ❌ | ✅ `SingleVenueErrorAppliesToTheWholeBatch` | ❌ |
| Stale-fill protection | ❌ | ✅ `StaleFillReplayDoesNotRewindThePosition` | ❌ |
| Terminal-state stickiness | ❌ | ✅ `TerminalStatesAreSticky` | ❌ |
| Timeout / transport reconciliation | ❌ | ✅ `TimeoutTriggersOrderStatusReconcile`, `UnknownAfterTimeoutBecomesRejected`, `ActionInFlightWhenTheSocketDropsIsReconciled` | ✅ forced reconnect reconciles a live order |
| Reconnect reconciliation sweep | ❌ | ✅ `ReconnectReconcilesLiveOrders`, `FillsMissedWhileDisconnectedAreApplied` | ✅ |
| Liquidation and venue-initiated cancels | ❌ | ✅ `LiquidationAndVenueCancelsAreReported` | ❌ not reproducible on demand |
| Address request budget accounting | ❌ | ✅ `TracksTheAddressRequestBudget` | ❌ |
| Re-entrancy (acting from inside a callback) | ❌ | ✅ `PlacingFromInsideACallbackIsSafe` | ❌ |
| Local validation (price, size, coin, delisting) | ❌ | ✅ `ValidatesLocally`, `RejectsMalformedConfig` | ✅ dedicated step |
| Venue rejection of a crossing post-only order | ❌ | ✅ `PerOrderRejection`, `VenueErrorOnAction` | ✅ dedicated step |
| IOC fill, position and fees | ❌ | ✅ covered by the fill tests | ✅ fill + reduce-only close |
| HTTP transport fallback | ❌ | ✅ `HttpTransport` | ❌ |
| HTTP 429 handling (`Retry-After`, queue pause) | ❌ | ✅ `RateLimitPausesTheQueueForRetryAfter` | ❌ |
| `/info` — `metaAndAssetCtxs`, `candleSnapshot`, `fundingHistory`, `predictedFundings`, `userRateLimit`, `spotClearinghouseState`, `userFunding`, `historicalOrders`, `userFillsByTime`, `userRole` | ❌ | ✅ one `InfoClientTest` per group, against recorded response bodies | ⚠️ only `orderStatus` and `frontendOpenOrders` are exercised live |
| WS decoding — every typed channel | ❌ | ✅ `ws_message_parser_test.cpp`, including a full captured session, truncated frames and mutated frames | ✅ `l2Book` / `bbo` via the order-book step |
| Market-data subscriptions, reconnect replay, book drop on unsubscribe | ❌ | ✅ `market_data_client_test.cpp` (`UnsubscribingAnL2BookDropsTheMaintainedBook`, `UserAndCandleSubscriptions`, `ReconnectReplaysSubscriptionsAndClearsBooks`) | ✅ order-book step |

**Reading the gaps in this table.** A ❌ in the *live testnet* column usually means the feature cannot be
triggered on demand (liquidations), needs account state the test wallet does not have (vaults, approved
builders, $1 M volume), or costs money to exercise. A ❌ in the *golden vector* column for a behavioural feature
is expected — there is no wire encoding to freeze. The rows to be uneasy about are the ones with a single mark
in the golden-vector column and nothing else: `TpSlGroupingMatchesSdk`, `UpdateIsolatedMarginMatchesSdk`,
`NoopAndReserveRequestWeightMatchSdk` and `BuilderFeeMatchesSdk` prove the encoding matches the reference SDK,
but nothing in this repository has watched the venue accept them.
