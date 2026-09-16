// Golden vectors generated with the official hyperliquid-python-sdk
// (msgpack.packb(action), action_hash, sign_l1_action), key kKey, nonce kNonce.
#include <gtest/gtest.h>

#include <array>

#include "hl/core/Hex.h"
#include "hl/om/Actions.h"

using hl::Decimal;

namespace {

constexpr std::string_view kKey = "0x0123456789012345678901234567890123456789012345678901234567890123";
constexpr std::uint64_t kNonce = 1700000000000ULL;

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

std::string hexOf(const std::vector<std::uint8_t>& v) { return hl::toHex(v.data(), v.size()); }

std::string pad64(std::string_view h) {
    h = hl::stripHexPrefix(h);
    return std::string(64 - h.size(), '0') + std::string{h};
}

struct Golden {
    std::string_view msgpack;
    std::string_view connId;
    std::string_view r;
    std::string_view s;
    int v;
};

void expectGolden(const hl::EncodedAction& action, const Golden& g, bool mainnet = true,
                  std::optional<hl::Address> vault = std::nullopt,
                  std::optional<std::uint64_t> expiresAfter = std::nullopt) {
    EXPECT_EQ(hexOf(action.msgpack), g.msgpack);
    const auto connId = hl::actionHash(action.msgpack, vault, kNonce, expiresAfter);
    EXPECT_EQ(hl::toHex(connId.data(), 32), g.connId);
    hl::Signer signer{kKey};
    const auto sig = signer.signL1Action(action.msgpack, vault, kNonce, expiresAfter, mainnet);
    EXPECT_EQ(hl::toHex(sig.r.data(), 32), pad64(g.r));
    EXPECT_EQ(hl::toHex(sig.s.data(), 32), pad64(g.s));
    EXPECT_EQ(sig.v, g.v);
}

hl::OrderWire btcAlo() {
    hl::OrderWire o;
    o.asset = 0;
    o.isBuy = true;
    o.px = d("50000");
    o.sz = d("0.001");
    o.tif = hl::Tif::Alo;
    o.cloid = hl::Cloid::parse("0x0000000000000000000000000000abcd");
    return o;
}

hl::OrderWire ethIocReduceOnly() {
    hl::OrderWire o;
    o.asset = 1;
    o.isBuy = false;
    o.px = d("1670.1");
    o.sz = d("0.0147");
    o.tif = hl::Tif::Ioc;
    o.reduceOnly = true;
    return o;
}

constexpr std::string_view kBatch2Msgpack =
    "83a474797065a56f72646572a66f72646572739287a16100a162c3a170a53530303030a173a5302e303031a172c2a17481a56c696d6974"
    "81a3746966a3416c6fa163d9223078303030303030303030303030303030303030303030303030303030306162636486a16101a162c2a170"
    "a6313637302e31a173a6302e30313437a172c3a17481a56c696d697481a3746966a3496f63a867726f7570696e67a26e61";

}  // namespace

TEST(Actions, BatchOfTwoOrdersMainnet) {
    const std::array<hl::OrderWire, 2> orders{btcAlo(), ethIocReduceOnly()};
    expectGolden(hl::actions::order(orders),
                 {kBatch2Msgpack, "2e02b4c411d30eac48983ae30b217c39f468d5aa190934de1dab83a61bb5fd40",
                  "0x2651e444ef6d1a25a6514acebe87c7bb47e29dacd8678c4229809dd3f9cb71b1",
                  "0x5f343f467e7c38f42ca7d3acb79bc46e3efed372ff008f9212cc572406f86894", 28});
}

TEST(Actions, BatchOfTwoOrdersTestnet) {
    const std::array<hl::OrderWire, 2> orders{btcAlo(), ethIocReduceOnly()};
    expectGolden(hl::actions::order(orders),
                 {kBatch2Msgpack, "2e02b4c411d30eac48983ae30b217c39f468d5aa190934de1dab83a61bb5fd40",
                  "0xf5863c327472fa9f7923947ebf4a6b0b98739507f23cddf3bde4c159bbfa594b",
                  "0x5d3c58f64046b8cf258128731d2951f4f84399d56fe4cf8e73596b27b91a2ca0", 27},
                 /*mainnet=*/false);
}

TEST(Actions, SingleOrderMatchesSdk) {
    hl::OrderWire o;
    o.asset = 0;
    o.isBuy = true;
    o.px = d("50000");
    o.sz = d("0.001");
    o.tif = hl::Tif::Gtc;
    const std::array<hl::OrderWire, 1> orders{o};
    expectGolden(hl::actions::order(orders),
                 {"83a474797065a56f72646572a66f72646572739186a16100a162c3a170a53530303030a173a5302e303031a172c2a17481"
                  "a56c696d697481a3746966a3477463a867726f7570696e67a26e61",
                  "323b547050d98eb76afbf8d096d1c5340512df18a3e4179990a666cc32fba0ce",
                  "0x624d2f9a91c88a28ee5c3e1448f3cb387be99dd18ba4bdd7b3ea229c10c57399",
                  "0x4c0f16c217ad058ace0e6aaae3291ad850308041a26dc9df4e002aa8874b0cd0", 27});
}

TEST(Actions, TriggerOrderMatchesSdk) {
    hl::OrderWire o;
    o.asset = 0;
    o.isBuy = false;
    o.px = d("61000");
    o.sz = d("0.5");
    o.reduceOnly = true;
    o.trigger = hl::TriggerSpec{d("60000.5"), true, hl::TriggerSpec::Kind::StopLoss};
    const std::array<hl::OrderWire, 1> orders{o};
    const auto action = hl::actions::order(orders);
    EXPECT_EQ(hexOf(action.msgpack),
              "83a474797065a56f72646572a66f72646572739186a16100a162c2a170a53631303030a173a3302e35a172c3a17481a774726967"
              "67657283a869734d61726b6574c3a9747269676765725078a736303030302e35a47470736ca2736ca867726f7570696e67a26e61");
    hl::Signer signer{kKey};
    const auto sig = signer.signL1Action(action.msgpack, std::nullopt, kNonce, std::nullopt, true);
    EXPECT_EQ(hl::toHex(sig.r.data(), 32), "6450c9027f2df9075de9cbc253e7388faf178bf92336784064fce8d3b26c2c5d");
    EXPECT_EQ(hl::toHex(sig.s.data(), 32), "7fcb295ee5c0f4563bb80fbce17c1c6feed830dc04b076e54e92e7dd88d25ba1");
    EXPECT_EQ(sig.v, 28);
    EXPECT_EQ(action.json,
              R"({"type":"order","orders":[{"a":0,"b":false,"p":"61000","s":"0.5","r":true,)"
              R"("t":{"trigger":{"isMarket":true,"triggerPx":"60000.5","tpsl":"sl"}}}],"grouping":"na"})");
}

TEST(Actions, CancelBatch) {
    const std::array<hl::CancelWire, 2> cancels{{{0, 1}, {1, 4294967296ULL}}};
    expectGolden(hl::actions::cancel(cancels),
                 {"82a474797065a663616e63656ca763616e63656c739282a16100a16f0182a16101a16fcf0000000100000000",
                  "b1eb16ecf91536bec0ac7aeeba4fc1bbbdc501dac377d3d3503be8bc09f2e2f2",
                  "0x19fb0d5f1d3285ca2818c8f1a7541988a5661202038b4ddbc24b767c2c70d826",
                  "0x59d229d55e84122eea62a107b6a6ee072d12014d5716648c0f832e4b434a298a", 27});
}

TEST(Actions, CancelByCloid) {
    const std::array<hl::CancelByCloidWire, 1> cancels{{{0, hl::Cloid::fromU64(1)}}};
    const auto action = hl::actions::cancelByCloid(cancels);
    EXPECT_EQ(hexOf(action.msgpack),
              "82a474797065ad63616e63656c4279436c6f6964a763616e63656c739182a5617373657400a5636c6f6964d922307830"
              "30303030303030303030303030303030303030303030303030303030303031");
    EXPECT_EQ(hl::toHex(hl::actionHash(action.msgpack, std::nullopt, kNonce, std::nullopt).data(), 32),
              "1d854324d277d0ec7ab94e7d490f930b8cabb29049f2563b612afb8d0394cd50");
    EXPECT_EQ(action.json, R"({"type":"cancelByCloid","cancels":[{"asset":0,"cloid":"0x00000000000000000000000000000001"}]})");
}

TEST(Actions, ScheduleCancelSetAndClear) {
    expectGolden(hl::actions::scheduleCancel(1700000060000ULL),
                 {"82a474797065ae7363686564756c6543616e63656ca474696d65cf0000018bcfe65260",
                  "543d84c42876132614aca907b0dbd95399b942229d79e0f1c5eb81f1592e2ddf",
                  "0x63f82e27ce8ba37e7374cd5991215dd814a032644ed03d0480b7a6902bd0194c",
                  "0x17e5777edf89919c48c2999d1c591ca7fb228cf4b8341705a4a585d1c269a990", 27});
    expectGolden(hl::actions::scheduleCancel(std::nullopt),
                 {"81a474797065ae7363686564756c6543616e63656c",
                  "3203332b8f438c09b3d96dc86b924df85640505711dacbbe2ba373d7f16ef09c",
                  "0xe4445c6da2737dcb3fee835b5068bea48a0eb959aa42cb60cd56c561d8ff2637",
                  "0x7ebbe36befd7fe54b247b1c848cc030e84f6b01515effcd9ea71b298e7039fda", 27});
    EXPECT_EQ(hl::actions::scheduleCancel(1700000060000ULL).json, R"({"type":"scheduleCancel","time":1700000060000})");
}

TEST(Actions, UpdateLeverage) {
    const auto action = hl::actions::updateLeverage(3, false, 7);
    expectGolden(action, {"84a474797065ae7570646174654c65766572616765a5617373657403a7697343726f7373c2a86c6576657261676507",
                          "ed098b6f8ca1307a1e5598d983d2e0cacfd5a38091f6a336b24ca863cc186649",
                          "0x1b59cbba0e61cd1acb3cfd7550956b26ab166b8476526854fc9c0bea90d5d038",
                          "0x287f7b7b6f7979e59bdb52b9303f6f89a8fc572dfbd7e4c6f9f10bccca4ed5b4", 28});
    EXPECT_EQ(action.json, R"({"type":"updateLeverage","asset":3,"isCross":false,"leverage":7})");
}

TEST(Actions, BatchModifyByOidAndCloid) {
    std::array<hl::ModifyWire, 2> modifies;
    modifies[0].target = std::uint64_t{11};
    modifies[0].order = btcAlo();
    modifies[1].target = *hl::Cloid::parse("0x0000000000000000000000000000abcd");
    modifies[1].order = ethIocReduceOnly();
    expectGolden(hl::actions::batchModify(modifies),
                 {"82a474797065ab62617463684d6f64696679a86d6f6469666965739282a36f69640ba56f7264657287a16100a162c3a170a5"
                  "3530303030a173a5302e303031a172c2a17481a56c696d697481a3746966a3416c6fa163d92230783030303030303030303030"
                  "30303030303030303030303030303030306162636482a36f6964d9223078303030303030303030303030303030303030303030"
                  "3030303030303061626364a56f7264657286a16101a162c2a170a6313637302e31a173a6302e30313437a172c3a17481a56c69"
                  "6d697481a3746966a3496f63",
                  "39cdaca5cb52da69981c490e21c4cb091574a75170911a89d5c99616e513c1b3",
                  "0x403c95caaa841a053c656a2ce1c860f53cd74af4e90b67a9263f651f45c63e2b",
                  "0x48ca4af2c44f03232591b8a717cdd72c929a3c022b7c70867c0c387cc2aa4f2b", 28});
}

TEST(Actions, ExpiresAfterIsPartOfTheHash) {
    const std::array<hl::OrderWire, 1> orders{btcAlo()};
    expectGolden(hl::actions::order(orders),
                 {"83a474797065a56f72646572a66f72646572739187a16100a162c3a170a53530303030a173a5302e303031a172c2a17481"
                  "a56c696d697481a3746966a3416c6fa163d9223078303030303030303030303030303030303030303030303030303030306162"
                  "6364a867726f7570696e67a26e61",
                  "518868249a9a35ce5d8722615c4f475dfd5e63560758cd1214b43ecc4055dd03",
                  "0x7bd05ccd613ef09ad707e3ef8ef42a6ecb386ae4461db9199168d06fc45cb886",
                  "0x22658eecbcc7a63a13f99b04a861aa89e63b8a05675fd419f362a7969fb8e613", 27},
                 true, std::nullopt, 1700000100000ULL);
}

TEST(Actions, VaultOnTestnet) {
    const std::array<hl::OrderWire, 1> orders{btcAlo()};
    const auto vault = hl::parseAddress("0x1719884eb866cb12b2287399b15f7db5e7d775ea");
    expectGolden(hl::actions::order(orders),
                 {"83a474797065a56f72646572a66f72646572739187a16100a162c3a170a53530303030a173a5302e303031a172c2a17481"
                  "a56c696d697481a3746966a3416c6fa163d9223078303030303030303030303030303030303030303030303030303030306162"
                  "6364a867726f7570696e67a26e61",
                  "e8611b789f95a3ba0a8098f08b677e64db958c7e5531883d4bf6b812f68b15f8",
                  "0x7d866975e5e80602a19eb80f024122a33694b484633ed2ccb160ac39c9b9b8ab",
                  "0x2ae8f5672bf0d6359885a656ec29c2bb233e20008f3c90bd6745e16812502702", 27},
                 false, vault);
}

TEST(RequestBuilder, FullPayloadWithoutVault) {
    hl::Signer signer{kKey};
    hl::RequestBuilder builder{signer, hl::Network::Mainnet};
    hl::OrderWire o;
    o.asset = 0;
    o.isBuy = true;
    o.px = d("50000");
    o.sz = d("0.001");
    const std::array<hl::OrderWire, 1> orders{o};
    EXPECT_EQ(builder.payload(hl::actions::order(orders), kNonce),
              R"({"action":{"type":"order","orders":[{"a":0,"b":true,"p":"50000","s":"0.001","r":false,)"
              R"("t":{"limit":{"tif":"Gtc"}}}],"grouping":"na"},"nonce":1700000000000,"signature":)"
              R"({"r":"0x624d2f9a91c88a28ee5c3e1448f3cb387be99dd18ba4bdd7b3ea229c10c57399",)"
              R"("s":"0x4c0f16c217ad058ace0e6aaae3291ad850308041a26dc9df4e002aa8874b0cd0","v":27},)"
              R"("vaultAddress":null})");
}

TEST(RequestBuilder, PayloadWithVaultAndExpiry) {
    hl::Signer signer{kKey};
    const auto vault = hl::parseAddress("0x1719884EB866cb12b2287399b15f7db5e7d775ea");
    hl::RequestBuilder builder{signer, hl::Network::Testnet, vault};
    const auto body = builder.payload(hl::actions::scheduleCancel(std::nullopt), 5, 99);
    EXPECT_NE(body.find(R"("vaultAddress":"0x1719884eb866cb12b2287399b15f7db5e7d775ea","expiresAfter":99})"),
              std::string::npos)
        << body;
    EXPECT_EQ(body.rfind(R"({"action":{"type":"scheduleCancel"},"nonce":5,"signature":{"r":"0x)", 0), 0U);
}

TEST(RequestBuilder, WebSocketPostFrame) {
    EXPECT_EQ(hl::RequestBuilder::wsPost(42, R"({"x":1})"),
              R"({"method":"post","id":42,"request":{"type":"action","payload":{"x":1}}})");
    EXPECT_EQ(hl::RequestBuilder::wsInfo(7, R"({"type":"meta"})"),
              R"({"method":"post","id":7,"request":{"type":"info","payload":{"type":"meta"}}})");
}

TEST(NonceGenerator, StrictlyIncreasing) {
    hl::NonceGenerator gen;
    std::uint64_t prev = gen.next();
    EXPECT_GT(prev, 1'600'000'000'000ULL);
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t n = gen.next();
        ASSERT_GT(n, prev);
        prev = n;
    }
}

TEST(Cloid, ParseAndFormat) {
    const auto c = hl::Cloid::parse("0x0123456789ABCDEF0011223344556677");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->high(), 0x0123456789abcdefULL);
    EXPECT_EQ(c->low(), 0x0011223344556677ULL);
    EXPECT_EQ(c->toString(), "0x0123456789abcdef0011223344556677");
    EXPECT_FALSE(hl::Cloid::parse("0x1234"));
    EXPECT_FALSE(hl::Cloid::parse("0x0123456789abcdef001122334455667g"));
    EXPECT_EQ(hl::Cloid::fromU64(1).toString(), "0x00000000000000000000000000000001");
}
