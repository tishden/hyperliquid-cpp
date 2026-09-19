# Running on Hyperliquid testnet

- [1. Account and funds](#1-account-and-funds)
- [2. Create an API (agent) wallet](#2-create-an-api-agent-wallet)
- [3. Provide the credentials](#3-provide-the-credentials)
- [4. Acceptance check](#4-acceptance-check)
- [5. Run the demo](#5-run-the-demo)
- [6. Reading the output](#6-reading-the-output)
- [7. Verifying signing without funds](#7-verifying-signing-without-funds)
- [8. Troubleshooting](#8-troubleshooting)
- [9. Going to mainnet](#9-going-to-mainnet)

## 1. Account and funds

1. Open <https://app.hyperliquid-testnet.xyz> and connect the wallet you use on Hyperliquid.
2. Claim mock USDC from the faucet (`Faucet` / <https://app.hyperliquid-testnet.xyz/drip>). Hyperliquid
   currently only serves the faucet to addresses that have a mainnet account; check the testnet UI for
   the current conditions.
3. The testnet has its own asset list and asset ids — the library resolves them from `meta`, so nothing
   needs to be configured per network.

## 2. Create an API (agent) wallet

1. In the testnet app go to **More → API** (<https://app.hyperliquid-testnet.xyz/API>).
2. Enter a name, click **Generate**, then **Authorize API Wallet** and sign the approval in your wallet.
3. Copy the **private key** shown once. This key can place and cancel orders for your account but
   cannot withdraw funds, and you can revoke it on the same page at any time.

### The account address is the master account, not the API wallet

An API wallet has its own address, but it **acts for the master account**: orders are booked on the master,
and `orderUpdates`, `userFills`, positions and balances belong to the master. Configuring the agent's own
address as `HL_ACCOUNT_ADDRESS` produces a confusing state — orders are accepted and rest, while the client
reports no fills, no positions and a zero balance.

Ask the venue which is which:

```bash
curl -s -X POST https://api.hyperliquid-testnet.xyz/info -H 'Content-Type: application/json' \
     -d '{"type":"userRole","user":"0x<api wallet address>"}'
# {"role":"agent","data":{"user":"0x<master account>"}}   ← use data.user as HL_ACCOUNT_ADDRESS
```

The client performs the same check at start-up: with no `accountAddress` configured it adopts the master the
venue reports, and with a wrong one it logs an error and reports it through `ExchangeListener::onError`.

## 3. Provide the credentials

Either environment variables:

```bash
export HL_PRIVATE_KEY=0x<api wallet private key>
export HL_ACCOUNT_ADDRESS=0x<your main account address>   # the account the API wallet trades for
# export HL_VAULT_ADDRESS=0x…                              # only when trading a vault / sub-account
```

or a key file (keep it out of version control, `chmod 600`):

```
# ~/.config/hl/testnet.env
HL_PRIVATE_KEY=0x…
HL_ACCOUNT_ADDRESS=0x…
```

```bash
build/release/examples/hl_testnet_quoter --key-file ~/.config/hl/testnet.env
```

`HL_ACCOUNT_ADDRESS` matters: order updates, fills and positions belong to the master account, not to
the API wallet. If it is omitted the client assumes the key *is* the account.

## 4. Acceptance check

`hl_live_check` runs the whole order-management contract against the live venue and prints a pass/fail table
(exit code 0 only if every step passed). It places small orders far from the mid, and with `--taker` also
executes one IOC order and closes the position again.

```bash
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --notional 12 --taker
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --transport http --taker
build/release/examples/hl_live_check --key-file secrets/testnet.env --coin ETH --flatten   # cancel + close only
```

Real output (2026-09-19, ETH on testnet, abridged):

```
hyperliquid-cpp 1.2.0 — live acceptance check on testnet (ETH, WebSocket transport, presign 64)
▶ connect, load metadata, subscribe user streams
   PASS — account 0xMASTER…, 212 assets, ETH asset=4 szDecimals=4
▶ market data: order book
   PASS — bid 2639.2 / ask 2639.6, spread 1.52 bps, size to use 0.0046
▶ place post-only order far from mid → Open with oid
   PASS — px 2586.6, state Open, oid 60515103656
▶ info: orderStatus and frontendOpenOrders see the order
   PASS — orderStatus: status open, oid 60515103656 | frontendOpenOrders: listed: … tif Alo
▶ modify price and size in place (cloid preserved)
   PASS — px 2560.2, sz 0.0092, oid 60515103656 → 60515104764, state Open
▶ cancel by cloid → Canceled                        PASS — state Canceled
▶ batch of 2 orders in one action, then cancelAll   PASS — Open Open → cleared all
▶ post-only order that crosses → rejected by the venue
   PASS — Rejected: Post only order would have immediately matched, bbo was 2639.2@2639.6
▶ local validation rejects invalid price/size/coin before signing
   PASS — unknown coin 'NOSUCHCOIN' | invalid price 1234.56789 for ETH (nearest valid 1234.6) | …
▶ IOC order that crosses → fill, position and fees
      · fill Buy 0.0046 @ 2639.6 (taker, fee 0.005463)
   PASS — Filled, filled 0.0046 @ 2639.6, position 0.0046, fills 1, fee 0.005463 USDC
▶ close the position with a reduce-only IOC         PASS — Filled, position now 0
▶ scheduleCancel (dead-man's switch): arm and clear
   PASS — Cannot set scheduled cancel time until enough volume traded. Required: $1000000. Traded: $168.41.
▶ updateLeverage                                    PASS — ok
▶ reconnect: drop the private socket, reconcile a live order
   PASS — reconnected, reconciles 0 → 1, order Open
▶ no orders left on the venue                       PASS — 0 open orders on the venue
▶ no leftover position                              PASS — flat
  ---------------------------------------------------
  actions 13 (0 via HTTP), errors 2, timeouts 0, reconciles 1
  signatures 13 precomputed-nonce / 0 deterministic
  order updates 28, fills 2, md messages 12 (parse errors 0)
  0 of 17 steps failed
```

The two "errors" are the expected `scheduleCancel` refusals: Hyperliquid enables the dead-man's switch only
after $1 000 000 of traded volume on the account.

## 5. Run the demo

```bash
scripts/build.sh release

# 1) market data + quote computation only (no key needed)
build/release/examples/hl_testnet_quoter --dry-run --coin ETH

# 2) live quoting on testnet for 10 minutes
build/release/examples/hl_testnet_quoter --coin ETH --notional 20 --half-spread-bps 8 \
    --skew-bps 6 --max-position-usd 100 --requote-bps 3 --duration 600

# tighter quotes to get fills quickly, HTTP transport, dead-man's switch
build/release/examples/hl_testnet_quoter --coin BTC --half-spread-bps 1 --transport http --dead-man-switch
```

| Option | Default | Meaning |
|---|---|---|
| `--coin` | `ETH` | coin to quote |
| `--notional` | `20` | order value in USDC (venue minimum 10) |
| `--half-spread-bps` | `8` | distance of each quote from fair value (microprice) |
| `--skew-bps` | `6` | additional shift at full inventory; long → quotes move down |
| `--max-position-usd` | `100` | inventory limit; the side that would increase it is not quoted |
| `--requote-bps` | `3` | amend (batchModify) when the target moved this far |
| `--duration` | ∞ | run time in seconds (Ctrl-C stops earlier) |
| `--transport` | `ws` | `ws` = WebSocket `post`, `http` = `POST /exchange` |
| `--presign` | `256` | precomputed ECDSA nonces kept ready (`0` = deterministic RFC 6979 signing only) |
| `--dead-man-switch` | off | keep `scheduleCancel(now + 90 s)` refreshed every 30 s |
| `--dry-run` | off | no orders, prints computed quotes |
| `--mainnet --i-understand-this-trades-real-money` | off | mainnet (both flags required) |
| `--verbose` | off | library debug logging |

On Ctrl-C / `--duration` expiry the quoter cancels its orders, waits for confirmation (≤ 5 s), clears
the dead-man's switch and prints a summary.

## 6. Reading the output

Real 70-second run quoting at the touch (`--half-spread-bps 0.2 --requote-bps 0.5`), 2026-09-19:

```
hyperliquid-cpp 1.2.0 — ETH quoter on testnet (live orders)
[hl][INFO] exchange: 212 assets loaded
[hl][INFO] exchange: account value 998.96397 USDC, 0 open positions
[hl][INFO] exchange: ready
[ex] ready: account 0xMASTER…, signer 0xAGENT…, ETH asset=4 szDecimals=4, position 0
[status] ETH mid=2639.75 spread=0.38bps bid=2639.7(Open) ask=2639.9(Open) pos=0 fills=0 vol=$0 pnl≈$0
[fill] Sell 0.0045 ETH @ 2639.8 (maker, fee 0.001781 USDC) → position -0.0045
[fill] Sell 0.0045 ETH @ 2639.9 (maker, fee 0.001781 USDC) → position -0.009
[status] ETH mid=2640.45 spread=1.14bps bid=2640.3(Open) ask=2641.6(Open) pos=-0.009 fills=2 vol=$23.75865 pnl≈$-0.008962
…
══ summary ══════════════════════════════════════════════
  coin            ETH
  orders placed   3   amendments 4   rejects 0
  fills           2 (maker 2)   volume $23.75865
  position        0 → -0.009
  pnl (mark@mid)  $-0.011212
  actions         8 sent, 0 via HTTP, 0 errors, 0 timeouts, 0 reconciles
  signatures      8 precomputed-nonce, 0 deterministic
  md messages     29 (parse errors 0, reconnects 0)
```

Quoting 1.5 bps away from mid on the same market produced no fills in five minutes (the spread itself is
0.4–1.5 bps, so those quotes sat behind the touch) — quote at or inside the touch to be filled. Note the
inventory skew after the fills: short inventory moved both quotes up.


- `bid=…(Open)` shows the resting quote and its `OrderState`.
- Testnet fees observed in these runs: maker 0.001781 USDC on $11.88 (1.5 bps), taker 0.005463 USDC on
  $12.14 (4.5 bps).
- `pnl` is cash flow from fills (fees included) plus the position change marked at mid — a quick
  sanity figure, not accounting.
- `reconciles > 0` means an action outcome was unknown (timeout / disconnect) and was resolved via
  `orderStatus`.

## 7. Verifying signing without funds

Signing can be validated end-to-end before any account exists: run the live quoter with a random key.

```bash
HL_PRIVATE_KEY=0x$(openssl rand -hex 32) build/release/examples/hl_testnet_quoter --coin ETH --duration 10
```

```
[hl][INFO] exchange: starting (testnet, account 0xe80310e4…cf1b, signer 0xe80310e4…cf1b)
[ex] Buy 0.0083 @ 2390.8 rejected: User or API Wallet 0xe80310e44d1fbfa941593965509e292a9c96cf1b does not exist.
```

The venue recovers the signer from the ECDSA signature over the EIP-712 digest of the msgpack action.
The address in the rejection is **exactly** the locally derived signer address — so the action bytes,
hash, digest and signature all match what the venue computes. With a wrong encoding the venue would
recover a different, random-looking address. Repeat with `--transport http` to check the HTTP path, and with
`--presign 0` to check deterministic signing (the summary line `signatures` shows which path was used).
Note the demo backs off exponentially on consecutive rejections.

## 8. Troubleshooting

| Message | Meaning / fix |
|---|---|
| `User or API Wallet 0x… does not exist` (your signer address) | API wallet not authorised on **testnet** (authorisations are per network), or the account has never been funded |
| `… does not exist` (unknown address) | signing mismatch — should not happen with this library; report with the request body |
| `Post only order would have immediately matched` | quote crossed between computation and arrival; harmless for a market maker (the next book update requotes) |
| `Insufficient margin to place order` | not enough testnet USDC / leverage too low |
| `Order must have minimum value of $10` | raise `--notional` |
| `Order has invalid price` / `invalid size` | only possible when bypassing `AssetInfo` rounding |
| `Cannot set scheduled cancel time until enough volume traded. Required: $1000000` | the dead-man's switch needs $1 M of traded volume on the account; run without `--dead-man-switch` |
| orders rest but no fills, positions or balance are reported | `HL_ACCOUNT_ADDRESS` is the API wallet instead of the master account (see [§2](#the-account-address-is-the-master-account-not-the-api-wallet)); the client logs this explicitly |
| `TLS handshake failed … certificate` | CA bundle not found: `export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt` (Debian/Alpine) or `/etc/pki/tls/certs/ca-bundle.crt` (RHEL) |
| repeated `ws: … closed` | network/proxy issue; the client reconnects with back-off and reconciles orders automatically |

## 9. Going to mainnet

1. Create a **separate** API wallet on mainnet (<https://app.hyperliquid.xyz/API>).
2. Start with `--dry-run` using `--mainnet --i-understand-this-trades-real-money`, then live with a small
   `--max-position-usd`, `--dead-man-switch`, and supervision.
3. The example is a demonstration of the API, not a profitable strategy.

---

© 2026 Denis Tishkov <denis8825@ya.ru>. hyperliquid-cpp is licensed, not sold — see [LICENSE](../LICENSE).
