# Security policy

hyperliquid-cpp holds a private key in memory and signs orders with real money behind them. Report
anything that could leak a key, sign something other than what the caller asked for, or make the
client lose track of live orders.

## Reporting a vulnerability

Do not open a public issue. Use GitHub's private reporting (**Security → Report a vulnerability** on the
repository page) or write to Denis Tishkov <denis8825@ya.ru>.

Please include the affected version or commit, how to reproduce it, and the impact as you see it. You
will get an answer within a week; a fix and an advisory follow once the fix is released.

## Supported versions

Only the latest release receives fixes.

## Scope

In scope: the library under `include/` and `src/`, and the examples insofar as they handle keys.

By design the library cannot move funds — withdrawals, transfers and staking need user-signed actions it
does not implement ([docs/COVERAGE.md](docs/COVERAGE.md)). A way around that is in scope and serious.

Out of scope: the Hyperliquid venue itself, and losses from strategy logic built on top of the library.
