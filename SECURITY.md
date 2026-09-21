# Security policy

hyperliquid-cpp is a research project. It is thoroughly tested — 200 tests, sanitizers, a live acceptance run
on testnet and mainnet — and benchmarked, but it comes with **no warranty and no commitment to support**.

## What is not promised

- **No warranty.** The software is provided "as is", without warranties or conditions of any kind, as stated
  in sections 7 and 8 of the [Apache License 2.0](LICENSE). You trade with it at your own risk, including the
  risk of losing funds.
- **No commitment to fix.** Reported bugs and vulnerabilities may be fixed, or may not; there is no response
  time and no guarantee of a fix or an advisory.
- **No commitment to maintain.** Further development, updates for changes in the Hyperliquid API and support
  of any version are not promised. Review the code and run your own tests before putting money behind it.

## Reporting a vulnerability

Report privately through GitHub: **Security → Report a vulnerability** on the repository page. Do not open a
public issue for a vulnerability.

Include the affected version or commit, how to reproduce it and the impact as you see it. Reports are
welcome and will be read, subject to the section above.

## What is worth reporting

Anything that could leak a key, sign something other than what the caller asked for, or make the client lose
track of live orders. By design the library cannot move funds — withdrawals, transfers and staking need
user-signed actions it does not implement ([docs/COVERAGE.md](docs/COVERAGE.md)); a way around that is the
most serious kind of report.

Out of scope: the Hyperliquid venue itself, and losses from strategy logic built on top of the library.
