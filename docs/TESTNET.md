# Running on Hyperliquid testnet

- [1. Account and funds](#1-account-and-funds)
- [2. Create an API (agent) wallet](#2-create-an-api-agent-wallet)
- [3. Provide the credentials](#3-provide-the-credentials)
- [4. Run the demo](#4-run-the-demo)
- [5. Reading the output](#5-reading-the-output)
- [6. Verifying signing without funds](#verifying-signing-without-funds)
- [7. Troubleshooting](#7-troubleshooting)
- [8. Going to mainnet](#8-going-to-mainnet)

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

## 4. Run the demo

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
| `--dead-man-switch` | off | keep `scheduleCancel(now + 90 s)` refreshed every 30 s |
| `--dry-run` | off | no orders, prints computed quotes |
| `--mainnet --i-understand-this-trades-real-money` | off | mainnet (both flags required) |
| `--verbose` | off | library debug logging |

On Ctrl-C / `--duration` expiry the quoter cancels its orders, waits for confirmation (≤ 5 s), clears
the dead-man's switch and prints a summary.

## 5. Reading the output

Illustrative, abridged output of a live session (values are examples):

```
hyperliquid-cpp 1.0.0 — ETH quoter on testnet (live orders)
[hl][INFO] exchange: 212 assets loaded
[hl][INFO] exchange: account value 999.12 USDC, 0 open positions
[hl][INFO] exchange: ready
[ex] ready: account 0x…, signer 0x…, ETH asset=4 szDecimals=4, position 0
[status] ETH mid=2395.65 spread=0.42bps bid=2393.7(Open) ask=2397.7(Open) pos=0 fills=0 vol=$0 pnl≈$0
[fill] Buy 0.0083 ETH @ 2393.7 (maker, fee -0.0002 USDC) → position 0.0083
[status] ETH mid=2391.10 spread=0.42bps bid=2389.2(Open) ask=2393.1(Open) pos=0.0083 fills=1 vol=$19.87 pnl≈$-0.02
…
══ summary ══════════════════════════════════════════════
  orders placed   14   amendments 37   rejects 0
  fills           6 (maker 6)   volume $119.2
  position        0 → 0.0083
  pnl (mark@mid)  $0.04
  actions         58 sent, 0 via HTTP, 0 errors, 0 timeouts, 0 reconciles
  md messages     4121 (parse errors 0, reconnects 0)
```

- `bid=…(Open)` shows the resting quote and its `OrderState`.
- `pnl` is cash flow from fills (fees included) plus the position change marked at mid — a quick
  sanity figure, not accounting.
- `reconciles > 0` means an action outcome was unknown (timeout / disconnect) and was resolved via
  `orderStatus`.

## Verifying signing without funds

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
recover a different, random-looking address. Repeat with `--transport http` to check the HTTP path.
Note the demo backs off exponentially on consecutive rejections.

## 7. Troubleshooting

| Message | Meaning / fix |
|---|---|
| `User or API Wallet 0x… does not exist` (your signer address) | API wallet not authorised on **testnet** (authorisations are per network), or the account has never been funded |
| `… does not exist` (unknown address) | signing mismatch — should not happen with this library; report with the request body |
| `Post only order would have immediately matched` | quote crossed between computation and arrival; harmless for a market maker (the next book update requotes) |
| `Insufficient margin to place order` | not enough testnet USDC / leverage too low |
| `Order must have minimum value of $10` | raise `--notional` |
| `Order has invalid price` / `invalid size` | only possible when bypassing `AssetInfo` rounding |
| `scheduleCancel not accepted: …` | the venue can refuse the dead-man's switch (e.g. for accounts without trading history, or more than 10 triggers per day); run without `--dead-man-switch` |
| `TLS handshake failed … certificate` | CA bundle not found: `export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt` (Debian/Alpine) or `/etc/pki/tls/certs/ca-bundle.crt` (RHEL) |
| repeated `ws: … closed` | network/proxy issue; the client reconnects with back-off and reconciles orders automatically |

## 8. Going to mainnet

1. Create a **separate** API wallet on mainnet (<https://app.hyperliquid.xyz/API>).
2. Start with `--dry-run` using `--mainnet --i-understand-this-trades-real-money`, then live with a small
   `--max-position-usd`, `--dead-man-switch`, and supervision.
3. The example is a demonstration of the API, not a profitable strategy.
