# Licensing — in plain language

This page explains the licence in ordinary words, for a buyer deciding whether the terms work and for a
seller preparing the deal. The licence itself exists in two equal versions — [English](../LICENSE) and
[Russian](../LICENSE.ru) — and the parties sign or accept one of them; that version governs.
**The licence governs; this page does not.** Neither is legal advice: have your counsel read the licence
before signing.

- [1. What you get](#1-what-you-get)
- [2. What you may do](#2-what-you-may-do)
- [3. What you may not do](#3-what-you-may-not-do)
- [4. What the seller keeps](#4-what-the-seller-keeps)
- [5. What the seller warrants](#5-what-the-seller-warrants)
- [6. Third-party components](#6-third-party-components)
- [7. FAQ](#7-faq)
- [8. Before signing — checklist for both sides](#8-before-signing--checklist-for-both-sides)

## 1. What you get

A **perpetual, worldwide, non-exclusive source-code licence** to this library: the code, tests, benchmarks,
examples and documentation, delivered once. No seats, no per-developer count, no per-machine count, no royalty
on your trading results or on the revenue of products you build with it. You do not get ownership, and you do
not get exclusivity.

## 2. What you may do

| | |
|---|---|
| Use it commercially | Yes — including proprietary trading for your own account |
| Read, build, test, debug the source | Yes |
| Modify it, fork it internally, strip parts out | Yes, and you never have to show those changes to anyone |
| Ship it inside your own product | Yes, **compiled** (static/shared library, binary, container running your binary) |
| Run it behind a hosted or managed service for clients | Yes, as long as no client receives the source |
| Share the source inside your company | Yes — employees, group companies, contractors and advisers under confidentiality |
| Keep using it if the seller later open-sources the library | Yes; publication does not touch your rights |
| Transfer the licence when your company is acquired | Yes, with written notice within 30 days |

## 3. What you may not do

| | |
|---|---|
| Resell, rent, sublicense or otherwise pass the code to a third party | No |
| Publish the source — public repo, package registry, article, talk, paste site, ML training set | No |
| Ship a container image or package that contains the source | No (ship your compiled binary instead) |
| Sell or give away a connector, SDK or client library built from this code | No — that is the seller's market |
| Patent the code, or any algorithm or technique it embodies | No, and no asserting patents against the seller or other licensees over it |
| Remove the copyright and licence notices, or claim authorship | No |

Two clarifications that matter in practice:

- **Your strategies stay yours.** Anything of yours that merely *calls* the library — strategy logic, models,
  data, risk systems — is outside the Agreement entirely. You may patent your own inventions as long as they
  do not read on the library.
- **"Licensee Product"** means a product whose main purpose is something other than being a connector or SDK.
  A trading system, a market-making service, an execution platform, a risk tool: all fine. A "Hyperliquid C++
  SDK" you distribute: not fine.

## 4. What the seller keeps

The licence is **non-exclusive** and the seller says so explicitly. The seller may keep selling the same code
to others, and may publish it or release it under an open-source licence at any time, without notice.

What that means for you as a buyer:

- Your rights in the copy you received are unaffected by anything the seller does later (clause 4.3).
- If the seller publishes the code, your confidentiality obligation and the resale/publication restrictions
  fall away **for the material that was published** — you are not left holding obligations over something
  that is public. They continue for anything not published, and for your own modifications.
- You are not buying a moat. If exclusivity matters to you, negotiate it in the Order: an exclusivity period,
  a right of first refusal, or a notice period before open-sourcing are the usual shapes, and they change the
  price.

## 5. What the seller warrants

The seller warrants, for its own code:

1. it owns it or otherwise has the right to license it;
2. it wrote it (or had it written for it) — no third-party code is baked in beyond the components listed in
   `THIRD_PARTY_NOTICES.md`;
3. to its knowledge it infringes nobody's copyright, trade secrets or other IP;
4. **no hidden copyleft**: nothing in it can force you to publish your source, or your product's source;
5. no back doors, no time bombs, no licence phone-home, no exfiltration of your keys, orders or data.

The seller also **defends you** against a third-party IP claim about the code as delivered, subject to the
usual conditions (prompt notice, seller controls the defence) and to the liability cap. That indemnity does
not cover your own modifications, your combinations with other software, or the third-party components.

Everything else is "as is": no promise of profit, of latency on your hardware, of the Hyperliquid API not
changing, or of the venue behaving. The benchmark numbers in [BENCHMARKS.md](BENCHMARKS.md) describe one
specific machine.

## 6. Third-party components

The library links three third-party components, all permissive, listed with versions and licences in
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md): OpenSSL (Apache-2.0), libsecp256k1 (MIT) and simdjson
(Apache-2.0). GoogleTest and Google Benchmark are build-time only and are not shipped in your product.

The seller's clean-provenance warranty (clause 5.1) covers *its* code, not those components — nobody can
warrant code they did not write. What the seller does warrant (clause 5.3) is that the list is accurate and
that, to its knowledge, those licences permit this use. When you ship a product, you carry their attribution requirements (a notices file in
your distribution is the usual way).

Two more provenance facts, stated so there are no surprises:

- The signing test vectors in `tests/actions_test.cpp` and `tests/signer_test.cpp` were generated with the
  official `hyperliquid-python-sdk` (MIT) and are facts about the venue's protocol, not copied code.
- The fixtures in `tests/fixtures/` are captures of public Hyperliquid market data.

## 7. FAQ

**Can I use it in production the day I receive it?** Yes. The licence is perpetual and starts on delivery.

**Do I have to publish my changes?** No. There is no copyleft anywhere in the stack, and the seller warrants
that (clause 5.1(d)).

**Can I give the source to a contractor who builds my trading system?** Yes, if they are bound by
confidentiality and use it only for you.

**Can I show it to an auditor, an investor's technical due-diligence team, or a regulator?** Auditors and
advisers acting for you: yes, under confidentiality. A regulator or court: yes, tell the seller first if you
lawfully can.

**Can I open-source my fork?** No. You can open-source your own code around it, but not the library or a
derivative of it.

**What if the seller open-sources it next year — did I overpay?** You bought delivery, a clean-provenance
warranty, an indemnity and the head start. Your rights do not shrink, and your obligations over the published
material fall away. If that risk is unacceptable, buy an exclusivity or notice clause in the Order.

**Can I resell my licence when I stop trading?** Not by itself. It transfers only with a change of control of
your company.

**What happens if I breach?** The seller must give 30 days' notice to cure a curable breach. After
termination you stop using the source, but products you already shipped in compiled form keep working for
their users.

**Is support included?** No, unless the Order says so. Delivery is one-off; updates and support are separate
line items.

## 8. Before signing — checklist for both sides

The licence needs no blanks filled in: the parties, the price and the delivery live in the Order, and the
governing law defaults to the licensor's country unless the Order says otherwise.

**Seller, put in the Order:**

- [ ] Both parties' legal names, the price and the payment terms
- [ ] The exact version delivered — tag, commit hash and a hash of the archive — so "the Software as
      delivered" in the warranty is unambiguous later
- [ ] Delivery method (repository access, archive) and the date
- [ ] Whether support, maintenance or updates are included, and for how long
- [ ] Any variation from the default: another governing law or arbitration, exclusivity or a notice period
      before open-sourcing, a different liability cap
- [ ] Which language version is being signed
- [ ] Before signing, re-confirm what you are warranting: the code is yours, the third-party list is complete,
      and no employer's or former client's IP is mixed in

**Buyer, check:**

- [ ] The Order names the exact version delivered (tag and commit hash)
- [ ] Clause 4.3 leaves room for your business model (it forbids selling connectors/SDKs, not trading systems)
- [ ] The liability cap is one you can live with given how you will use it
- [ ] Your own patent strategy is compatible with clause 4.4
- [ ] You are comfortable with a non-exclusive licence, or you negotiate exclusivity in the Order
- [ ] Your compliance team has seen the third-party licence list
