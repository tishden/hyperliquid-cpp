// hl_testnet_quoter — end-to-end demo of hyperliquid-cpp on Hyperliquid TESTNET.
//
// Builds an L2 order book from the public WebSocket (l2Book + bbo) and runs a
// two-sided post-only market maker with inventory skew through ExchangeClient.
//
//   export HL_PRIVATE_KEY=0x...        # API (agent) wallet key — see docs/TESTNET.md
//   export HL_ACCOUNT_ADDRESS=0x...    # master account the agent trades for
//   ./hl_testnet_quoter --coin ETH --notional 20 --half-spread-bps 8 --duration 600
//
//   ./hl_testnet_quoter --dry-run      # market data + quote computation only, no key needed

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include "common/Credentials.h"
#include "hl/hyperliquid.h"
#include "testnet_quoter/Quoter.h"

namespace {

hl::EventLoop* gLoop = nullptr;

void onSignal(int /*sig*/) {
    if (gLoop != nullptr) {
        gLoop->stop();  // async-signal-safe: atomic store + write(2) to an eventfd
    }
}

void usage() {
    std::printf(
        "usage: hl_testnet_quoter [options]\n"
        "  --coin SYMBOL            coin to quote (default ETH)\n"
        "  --notional USD           order size in USDC notional (default 20; venue minimum is 10)\n"
        "  --half-spread-bps N      quote distance from fair value (default 8)\n"
        "  --skew-bps N             extra shift at full inventory (default 6)\n"
        "  --max-position-usd N     inventory limit per side (default 100)\n"
        "  --requote-bps N          amend when target moves this far (default 3)\n"
        "  --duration SEC           stop after SEC seconds (default: run until Ctrl-C)\n"
        "  --transport ws|http      action transport (default ws)\n"
        "  --presign N              precomputed ECDSA nonces kept ready (default 256, 0 = RFC 6979 only)\n"
        "  --dead-man-switch        maintain scheduleCancel(now+90s)\n"
        "  --dry-run                no orders; print computed quotes (no key required)\n"
        "  --key-file PATH          file with HL_PRIVATE_KEY=/HL_ACCOUNT_ADDRESS=/HL_VAULT_ADDRESS= lines\n"
        "  --mainnet --i-understand-this-trades-real-money   use mainnet instead of testnet\n"
        "  --verbose                debug logging\n"
        "environment: HL_PRIVATE_KEY, HL_ACCOUNT_ADDRESS, HL_VAULT_ADDRESS\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    example::QuoterSettings settings;
    std::int64_t durationSec = 0;
    bool mainnet = false;
    bool mainnetAck = false;
    bool httpTransport = false;
    std::size_t presign = 256;
    example::Credentials credentials = example::credentialsFromEnv();

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", argv[i]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--coin") {
            settings.coin = next();
        } else if (a == "--notional") {
            settings.quote.orderNotionalUsd = hl::Decimal::parseOrZero(next());
        } else if (a == "--half-spread-bps") {
            settings.quote.halfSpreadBps = std::stod(next());
        } else if (a == "--skew-bps") {
            settings.quote.skewBps = std::stod(next());
        } else if (a == "--max-position-usd") {
            settings.quote.maxPositionUsd = hl::Decimal::parseOrZero(next());
        } else if (a == "--requote-bps") {
            settings.requoteBps = std::stod(next());
        } else if (a == "--duration") {
            durationSec = std::stoll(next());
        } else if (a == "--transport") {
            httpTransport = next() == "http";
        } else if (a == "--presign") {
            presign = static_cast<std::size_t>(std::stoull(next()));
        } else if (a == "--dead-man-switch") {
            settings.deadManSwitch = true;
        } else if (a == "--dry-run") {
            settings.dryRun = true;
        } else if (a == "--key-file") {
            if (!example::loadCredentialsFile(next(), credentials)) {
                std::fprintf(stderr, "cannot read the key file\n");
                return 2;
            }
        } else if (a == "--mainnet") {
            mainnet = true;
        } else if (a == "--i-understand-this-trades-real-money") {
            mainnetAck = true;
        } else if (a == "--verbose") {
            hl::setLogLevel(hl::LogLevel::Debug);
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (mainnet && !mainnetAck) {
        std::fprintf(stderr, "refusing to trade on mainnet without --i-understand-this-trades-real-money\n");
        return 2;
    }
    if (settings.quote.orderNotionalUsd < hl::Decimal::fromInt(10)) {
        std::fprintf(stderr, "--notional must be at least 10 USDC (venue minimum order value)\n");
        return 2;
    }
    if (!settings.dryRun && credentials.privateKey.empty()) {
        std::fprintf(stderr, "HL_PRIVATE_KEY is not set (use --dry-run to run without a key)\n");
        return 2;
    }
    const hl::Network network = mainnet ? hl::Network::Mainnet : hl::Network::Testnet;

    hl::EventLoop loop;
    gLoop = &loop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    example::Quoter quoter(loop, settings);
    hl::MarketDataConfig mdConfig;
    mdConfig.network = network;
    hl::MarketDataClient md(loop, quoter, mdConfig);
    quoter.attach(md);
    md.subscribeBook(settings.coin);

    std::unique_ptr<hl::ExchangeClient> exchange;
    std::unique_ptr<hl::InfoClient> info;
    if (settings.dryRun) {
        info = std::make_unique<hl::InfoClient>(loop, network);
        info->assets(false, [&](const hl::Result<hl::AssetRegistry>& r) {
            if (!r) {
                std::fprintf(stderr, "failed to load asset metadata: %s\n", r.error().message.c_str());
                loop.stop();
                return;
            }
            if (r->find(settings.coin) == nullptr) {
                std::fprintf(stderr, "unknown coin %s on %s\n", settings.coin.c_str(), mainnet ? "mainnet" : "testnet");
                loop.stop();
                return;
            }
            quoter.setAssets(r.value());
        });
    } else {
        hl::ExchangeConfig cfg;
        cfg.network = network;
        cfg.privateKey = credentials.privateKey;
        cfg.accountAddress = credentials.accountAddress;
        cfg.vaultAddress = credentials.vaultAddress;
        cfg.transport = httpTransport ? hl::ActionTransport::Http : hl::ActionTransport::WebSocket;
        cfg.precomputedNonces = presign;
        try {
            exchange = std::make_unique<hl::ExchangeClient>(loop, quoter, cfg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "configuration error: %s\n", e.what());
            return 2;
        }
        quoter.attach(*exchange);
        exchange->start();
    }

    std::printf("hyperliquid-cpp %s — %s quoter on %s (%s)\n", hl::kVersionString, settings.coin.c_str(),
                mainnet ? "MAINNET" : "testnet", settings.dryRun ? "dry run" : "live orders");
    md.start();
    if (durationSec > 0) {
        loop.addTimer(durationSec * 1000, [&loop] { loop.stop(); });
    }
    loop.run();

    std::printf("\nstopping…\n");
    quoter.shutdown(5'000);
    quoter.printSummary();
    if (exchange) {
        exchange->stop();
    }
    md.stop();
    return 0;
}
