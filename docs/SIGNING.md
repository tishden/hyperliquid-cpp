# Hyperliquid L1-action signing — specification

Hyperliquid authenticates every trading action (`order`, `cancel`, `cancelByCloid`, `batchModify`,
`scheduleCancel`, `updateLeverage`, …) with an ECDSA signature over an EIP-712 typed-data digest of a
*phantom agent*. This document specifies the procedure exactly as implemented in `hl::actions`,
`hl::actionHash`, `hl::agentDigest` and `hl::Signer`, and as verified byte-for-byte against the official
`hyperliquid-python-sdk`.

- [1. Pipeline](#1-pipeline)
- [2. Action encoding (MessagePack)](#2-action-encoding-messagepack)
- [3. Action hash (connectionId)](#3-action-hash-connectionid)
- [4. EIP-712 digest](#4-eip-712-digest)
- [5. ECDSA](#5-ecdsa)
- [6. Request body](#6-request-body)
- [7. Worked example](#7-worked-example)
- [8. Pitfalls](#8-pitfalls)
- [9. Fast signing with precomputed nonces](#9-fast-signing-with-precomputed-nonces)

## 1. Pipeline

```
action (map)  ──msgpack──►  bytes
bytes ‖ nonce_be64 ‖ vaultFlag[‖vault20] [‖ 0x00 ‖ expiresAfter_be64]  ──keccak256──►  connectionId
Agent{ source: "a"|"b", connectionId }  ──EIP-712 (domain Exchange/1/1337/0x0)──►  digest
digest  ──secp256k1 ECDSA (RFC 6979, low-s)──►  (r, s, v = 27 + recid)
POST /exchange {"action": <JSON>, "nonce": n, "signature": {"r","s","v"}, "vaultAddress": …}
```

## 2. Action encoding (MessagePack)

The signed bytes are `msgpack.packb(action)` as produced by `msgpack-python`:

- maps keep **insertion order** — the key order below is normative;
- integers use the **smallest** encoding (`fixint`, `uint8`, `uint16`, `uint32`, `uint64`);
- strings use `fixstr` (< 32 bytes), then `str8`/`str16`/`str32`;
- prices and sizes are **strings** in canonical form: no exponent, no trailing zeros, no trailing dot
  (`"50000"`, `"0.001"`, `"1670.1"`) — `Decimal::toString()`.

| Action | Map (key order) |
|---|---|
| order | `{type:"order", orders:[wire…], grouping:"na"}` |
| order wire | `{a:asset, b:isBuy, p:px, s:sz, r:reduceOnly, t:type, [c:cloid]}` |
| limit type | `{limit:{tif:"Alo"\|"Ioc"\|"Gtc"}}` |
| trigger type | `{trigger:{isMarket:bool, triggerPx:str, tpsl:"tp"\|"sl"}}` |
| cancel | `{type:"cancel", cancels:[{a:asset, o:oid}…]}` |
| cancelByCloid | `{type:"cancelByCloid", cancels:[{asset:asset, cloid:"0x…"}…]}` |
| batchModify | `{type:"batchModify", modifies:[{oid: oid(uint) \| cloid(str), order: wire}…]}` |
| scheduleCancel | `{type:"scheduleCancel"[, time:ms]}` |
| updateLeverage | `{type:"updateLeverage", asset, isCross:bool, leverage}` |

`asset` is the index in the perp universe (`meta`) or `10000 + index` for spot pairs (`spotMeta`).
Asset ids differ between mainnet and testnet — always resolve by name.

The JSON sent on the wire must describe the same action; the venue re-serialises it to msgpack to
verify, so the JSON key order is not load-bearing, but the values (notably the number strings) are.

## 3. Action hash (connectionId)

```
data = msgpack(action)
     ‖ uint64_be(nonce)
     ‖ (0x00                      if no vault
        0x01 ‖ vault[20 bytes]    otherwise)
     ‖ (nothing                   if no expiresAfter
        0x00 ‖ uint64_be(expiresAfter))
connectionId = keccak256(data)
```

`keccak256` is the original Keccak with `0x01` domain padding (Ethereum), **not** NIST SHA3-256.
The library ships its own Keccak-f[1600] so it does not depend on OpenSSL ≥ 3.2's `KECCAK-256`.

## 4. EIP-712 digest

```
domainSeparator = keccak256( keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
                           ‖ keccak256("Exchange")
                           ‖ keccak256("1")
                           ‖ uint256_be(1337)
                           ‖ uint256_be(0) )                       # verifyingContract = 0x000…0

structHash      = keccak256( keccak256("Agent(string source,bytes32 connectionId)")
                           ‖ keccak256(source)                     # "a" mainnet, "b" testnet
                           ‖ connectionId )

digest          = keccak256( 0x19 ‖ 0x01 ‖ domainSeparator ‖ structHash )
```

Note that the chain id is always 1337 for L1 actions; mainnet and testnet differ only by `source`.
The domain separator and type hashes are constants and are computed once.

## 5. ECDSA

- Curve secp256k1, deterministic nonce (RFC 6979), **low-s** normalised (EIP-2) — libsecp256k1's
  defaults, identical to `eth-account`.
- `v = 27 + recoveryId` (no EIP-155 chain id).
- `r` and `s` are sent as `0x` + 64 hex digits (the SDK strips leading zeros; the venue accepts both).
- The signer is identified by recovery: the venue derives the address from `(digest, r, s, v)`. If that
  address is neither an account nor an approved agent wallet, the response is
  `User or API Wallet 0x… does not exist.` — a convenient end-to-end check that signing is correct
  (see [TESTNET.md](TESTNET.md#verifying-signing-without-funds)).

## 6. Request body

```json
{
  "action": { "type": "order", "orders": [ { "a": 0, "b": true, "p": "50000", "s": "0.001", "r": false,
              "t": { "limit": { "tif": "Gtc" } } } ], "grouping": "na" },
  "nonce": 1700000000000,
  "signature": { "r": "0x624d…7399", "s": "0x4c0f…0cd0", "v": 27 },
  "vaultAddress": null
}
```

`"expiresAfter": <ms>` is appended when used. Over WebSocket the same object is wrapped:
`{"method":"post","id":<n>,"request":{"type":"action","payload":<body>}}`.

**Nonces** must be unique among the signer's 100 highest nonces and lie within (now − 2 days,
now + 1 day). `hl::NonceGenerator` returns `max(nowMs, last + 1)`.

## 7. Worked example

Key `0x0123456789012345678901234567890123456789012345678901234567890123`
(address `0x14791697260e4c9a71f18484c9f997b308e59325`), nonce `1700000000000`, mainnet, no vault,
action: buy 0.001 of asset 0 at 50000, GTC.

| Step | Value |
|---|---|
| msgpack | `83a474797065a56f72646572a66f72646572739186a16100a162c3a170a53530303030a173a5302e303031a172c2a17481a56c696d697481a3746966a3477463a867726f7570696e67a26e61` |
| connectionId | `323b547050d98eb76afbf8d096d1c5340512df18a3e4179990a666cc32fba0ce` |
| r | `624d2f9a91c88a28ee5c3e1448f3cb387be99dd18ba4bdd7b3ea229c10c57399` |
| s | `4c0f16c217ad058ace0e6aaae3291ad850308041a26dc9df4e002aa8874b0cd0` |
| v | `27` |

More vectors (batches, triggers, cancels, modifies by oid and cloid, scheduleCancel, updateLeverage,
vault on testnet, `expiresAfter`) are in `tests/actions_test.cpp`.

## 8. Pitfalls

| Symptom | Cause |
|---|---|
| `User or API Wallet 0x<random> does not exist` with an address you do not recognise | msgpack bytes differ from the venue's (key order, integer width, `"50000.0"` instead of `"50000"`) — the signature is valid but over different data, so a different address is recovered |
| Works on testnet, fails on mainnet | `source` must be `"a"` on mainnet, `"b"` on testnet |
| Intermittent `Invalid nonce` | nonce reused across processes sharing one key, or wall clock far off |
| Orders land on the wrong coin | hard-coded asset id; ids differ between networks and change when assets are listed |
| `Order has invalid price` | more than 5 significant figures or too many decimals — use `AssetInfo::roundPx` |

## 9. Fast signing with precomputed nonces

The venue only verifies `(digest, r, s, v)`; it does not require RFC 6979. That allows splitting ECDSA into an
expensive message-independent part done ahead of time and a cheap online part
(`Signer::enableNoncePool`, `ExchangeConfig::precomputedNonces`).

**Offline** (refill thread), for each pool entry:

```
k      = HMAC-SHA256(d, rand32 ‖ counter_le64)  interpreted big-endian; reject if k = 0 or k ≥ n
R      = k·G                                    (libsecp256k1, constant time)
reject if R.x ≥ n                               (would need v ∈ {29, 30}; probability ≈ 2⁻¹²⁷)
store  r = R.x,  k⁻¹ mod n,  r·d mod n,  parity = R.y mod 2
wipe   k
```

**Online** (`signDigest`), per signature:

```
z  = digest mod n
s  = k⁻¹ · (z + r·d) mod n
if s > n/2:  s = n − s,  parity ^= 1            (low-s, EIP-2)
v  = 27 + parity
wipe the entry
```

Correctness is the standard ECDSA identity `s·k = z + r·d`; the recovery id of `(r, s)` is the parity of `R.y`,
flipped when `s` is negated (negating `s` corresponds to `−R`).

| Risk | Mitigation |
|---|---|
| Nonce reuse leaks the private key (two signatures with the same `k`) | each entry is popped exactly once under a lock and wiped; concurrent signers are tested under ThreadSanitizer; a `pthread_atfork` generation counter disables the pool in child processes |
| Predictable nonces leak the key | `k` is HMAC-keyed with the private key over CSPRNG output and a counter (hedged): unpredictable if either the RNG or the key is secret |
| Biased nonces (lattice attacks) | rejection sampling of `k` in `[1, n)` — no modular reduction bias |
| Timing side channels | `k·G` uses libsecp256k1's constant-time multiplication; the mod-n arithmetic has data-independent loops and masked conditional subtraction; the online step handles no `k` |
| Memory disclosure | entries hold `k⁻¹` and `r·d` in process memory, the same exposure class as the private key itself; they are wiped on use and on `disableNoncePool` |

Signatures from the pool are randomised: the same action signed twice yields different `(r, s)`, so golden-vector
comparisons (tests, SDK cross-checks) must run with the pool disabled. Verification by the venue is unaffected —
confirmed live on testnet over both WebSocket and HTTP (`signingStats().precomputed` > 0, venue recovered the
signer address).

