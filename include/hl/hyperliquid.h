// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

/**
 * @file hyperliquid.h
 * @brief Umbrella header for hyperliquid-cpp.
 *
 * | Layer            | Headers                                                        |
 * |------------------|----------------------------------------------------------------|
 * | Core types       | core/Decimal.h, core/Types.h, core/Result.h, core/Log.h, core/Hex.h |
 * | Market data      | md/MarketDataClient.h, md/OrderBook.h, WsMessages.h            |
 * | Order management | om/ExchangeClient.h, om/InfoClient.h, om/AssetRegistry.h       |
 * | Signing          | crypto/Signer.h, crypto/Keccak.h, om/Actions.h, om/MsgPack.h   |
 * | Transport        | net/EventLoop.h, net/WsSession.h, net/HttpClient.h, …          |
 */

#include "hl/Version.h"
#include "hl/WsMessageParser.h"
#include "hl/WsMessages.h"
#include "hl/core/Decimal.h"
#include "hl/core/Hex.h"
#include "hl/core/Int128.h"
#include "hl/core/Log.h"
#include "hl/core/Result.h"
#include "hl/core/Types.h"
#include "hl/crypto/Keccak.h"
#include "hl/crypto/Signer.h"
#include "hl/md/MarketDataClient.h"
#include "hl/md/OrderBook.h"
#include "hl/net/EventLoop.h"
#include "hl/net/HttpClient.h"
#include "hl/net/HttpCodec.h"
#include "hl/net/TlsStream.h"
#include "hl/net/Url.h"
#include "hl/net/WebSocketClient.h"
#include "hl/net/WsCodec.h"
#include "hl/net/WsSession.h"
#include "hl/om/Actions.h"
#include "hl/om/AssetRegistry.h"
#include "hl/om/ExchangeClient.h"
#include "hl/om/ExchangeResponse.h"
#include "hl/om/InfoClient.h"
#include "hl/om/MsgPack.h"
#include "hl/om/Types.h"
