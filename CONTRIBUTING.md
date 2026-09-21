# Contributing

Bug reports, reports of venue behaviour the library gets wrong, and pull requests are welcome. This is a
research project maintained without any commitment: issues may stay unanswered and pull requests unmerged
(see [SECURITY.md](SECURITY.md#what-is-not-promised)).

## Reporting a bug

Open an issue with:

- the version (`hl::kVersionString`, or the tag/commit), compiler and OS;
- network (testnet or mainnet) and transport (WebSocket `post` or HTTP);
- what you expected and what happened, with the log around it. Remove keys and, if you care,
  your account address — the library never logs a key, but your own code might.

A venue quirk — Hyperliquid answering or ordering messages differently from what the documentation or
this library assumes — is the most valuable kind of report. A captured frame sequence is ideal; a
regression test in `tests/` built on `MockVenue` is even better.

Vulnerabilities are reported privately through GitHub (**Security → Report a vulnerability**), not in public
issues — see [SECURITY.md](SECURITY.md).

## Pull requests

1. Build and test: `scripts/build.sh release && scripts/test.sh release`.
2. Before opening the PR run `scripts/ci.sh` — compilers, sanitizers, documentation links, the
   secret scan, licence headers and version/changelog consistency. Toolchains your machine lacks are
   skipped.
3. A fix comes with a test that fails without it. A behaviour change updates `docs/API.md` and, where
   it applies, `docs/ORDER_MANAGEMENT.md` or `docs/COVERAGE.md`.
4. Add a line under an `## [Unreleased]` section in [CHANGELOG.md](CHANGELOG.md).
5. Format with the repository's `.clang-format`. Keep the hot paths allocation-free and off floating
   point — `Decimal` on the wire, as the existing code does.
6. New files start with the licence header:

   ```cpp
   // SPDX-License-Identifier: Apache-2.0
   // Copyright 2026 <your name>
   ```

Contributions are accepted under the Apache License 2.0 (section 5 of [LICENSE](LICENSE)); no separate
CLA is required.

## Never commit

Private keys, `secrets/`, `.env` files, or anything captured from a private account stream. Use
`credentials.env.example` as the template and keep real files under the gitignored `secrets/`.
