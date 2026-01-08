// Copyright (c) 2025 The Multiflex developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <pol/pol.h>

#include <chainparams.h>          // Params()
#include <core_io.h>              // ValueFromAmount
#include <crypto/sha256.h>         // CSHA256
#include <node/context.h>          // node::NodeContext
#include <rpc/server.h>            // CRPCTable, CRPCCommand, JSONRPCRequest
#include <rpc/server_util.h>       // EnsureAnyNodeContext, EnsureChainman
#include <rpc/util.h>              // RPCHelpMan, RPCArg, JSONRPCError
#include <sync.h>                  // WITH_LOCK
#include <util/strencodings.h>     // IsHex, ParseHex
#include <validation.h>            // cs_main, ChainstateManager

#include <univalue.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> ParseMinerTagHex(const std::string& tag_hex)
{
    // Accept 4/8/12 bytes == 8/16/24 hex chars.
    const size_t n = tag_hex.size();
    if (!(n == 8 || n == 16 || n == 24)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "miner_tag_hex must be 4, 8 or 12 bytes (8/16/24 hex chars)");
    }
    if (!IsHex(tag_hex)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "miner_tag_hex must be hex");
    }
    return ParseHex(tag_hex);
}

int GetTipHeight(ChainstateManager& chainman)
{
    return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip() ? chainman.ActiveChain().Tip()->nHeight : -1);
}


int PolLevelFromPoints(bool seen, int points)
{
    if (!seen || points <= 0) return 0;
    if (points > 24) points = 24;
    const int level = (points + 1) / 2; // 1-2->1, 3-4->2, ..., 23-24->12
    return std::min(12, std::max(0, level));
}

int ParseHeightFlexible(const UniValue& v)
{
    // Some CLI/builds pass parameters as strings. Accept NUM or STR.
    if (v.isNum()) {
        return v.getInt<int>();
    }
    if (v.isStr()) {
        const std::string s = v.get_str();
        if (s.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "height must be a number");
        long long x = 0;
        try {
            size_t idx = 0;
            x = std::stoll(s, &idx, 10);
            if (idx != s.size()) throw std::invalid_argument("trailing");
        } catch (...) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "height must be a number");
        }
        if (x < 0 || x > std::numeric_limits<int>::max()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "height out of range");
        }
        return (int)x;
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, "height must be a number");
}

std::string StripWorkerSuffix(std::string s)
{
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s.erase(dot);
    return s;
}

std::vector<unsigned char> Tag12FromAddressString(const std::string& addr_in)
{
    // Legacy / canonical PoL mapping: tag12 = SHA256(ASCII address)[:12]
    // This binds the PoL identity to the miner's payout address string (not the pool address).
    const std::string addr = StripWorkerSuffix(addr_in);

    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256 hasher;
    hasher.Write(reinterpret_cast<const unsigned char*>(addr.data()), addr.size());
    hasher.Finalize(hash);
    return std::vector<unsigned char>(hash, hash + pol::POL_TAG_LEN);
}


} // namespace

static RPCHelpMan getpolallowedtag()
{
    return RPCHelpMan{
        "getpolallowedtag",
        "Return PoL allowed subsidy for a given MFLEXID miner tag at a given height.\n"
        "The miner tag is the 4/8/12-byte hex string stored after the ASCII prefix 'MFLEXID' in the coinbase OP_RETURN.\n",
        {
            {"miner_tag_hex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Miner tag in hex (8/16/24 hex chars)."},
            // NOTE: Use STR (not NUM) so both positional and -named calls work across CLI variants.
            {"height", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "Block height to evaluate. If omitted, defaults to current tip height + 1. "
             "Pass as a decimal string (e.g. \"110\")."},
        },
        RPCResults{},
        RPCExamples{
            "multiflex-cli getpolallowedtag 714b9f7144591e13fb75d4d5 110\n"
            "multiflex-cli -named getpolallowedtag miner_tag_hex=714b9f7144591e13fb75d4d5 height=110\n"
        },
        [](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            node::NodeContext& node = EnsureAnyNodeContext(request.context);
            ChainstateManager& chainman = EnsureChainman(node);

            const std::string tag_hex = request.params[0].get_str();
            const std::vector<unsigned char> tag = ParseMinerTagHex(tag_hex);

            const int tip = GetTipHeight(chainman);
            const int height = request.params.size() > 1 ? ParseHeightFlexible(request.params[1]) : (tip + 1);

            const CAmount allowed = pol::GetAllowedSubsidy(tag, height, Params().GetConsensus());

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("tip_height", tip);
            obj.pushKV("height", height);
            obj.pushKV("miner_tag_hex", tag_hex);
            obj.pushKV("miner_tag_len", (int)tag.size());
            obj.pushKV("allowed_subsidy", allowed);
            obj.pushKV("allowed_subsidy_coin", ValueFromAmount(allowed));
            return obj;
        }
    };
}

RPCHelpMan getpoladdressstatus()
{
    return RPCHelpMan{
        "getpoladdressstatus",
        "\nReturn PoL (Proof-of-Loyalty) status for a miner payout address.\n"
        "The node derives the miner tag as SHA256(address)[:12] and returns\n"
        "both the current tag status (seen/points/last seen) and the allowed\n"
        "subsidy for a given height.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "Miner payout address (username in stratum). Worker suffix after '.' is ignored."},
            {"height", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Optional height (as string). If omitted, uses current tip height."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "tip_height", "Current chain tip height"},
                {RPCResult::Type::NUM, "height", "Height used for subsidy calculation"},
                {RPCResult::Type::STR, "address", "Address as provided"},
                {RPCResult::Type::STR, "miner_tag_hex", "Derived miner tag (hex)"},
                {RPCResult::Type::NUM, "miner_tag_len", "Miner tag length in bytes"},
                {RPCResult::Type::NUM, "miner_tag_u32", "Little-endian u32 of first 4 bytes of tag"},
                {RPCResult::Type::NUM, "extranonce1_size", "Configured extranonce1 size (bytes)"},
                {RPCResult::Type::BOOL, "seen", "Whether this tag was seen in the active chain"},
                {RPCResult::Type::NUM, "first_seen_height", "First height where the tag was seen (-1 if never)"},
                {RPCResult::Type::NUM, "last_seen_height", "Last height where the tag was seen (-1 if never)"},
                {RPCResult::Type::NUM, "blocks_seen", "How many blocks were mined with this tag"},
                {RPCResult::Type::NUM, "last_seen_time", "Last seen block time (unix epoch seconds, 0 if never)"},
                {RPCResult::Type::NUM, "points", "Current loyalty points/level"},
                {RPCResult::Type::NUM, "last_seen_month", "Internal month index of last seen (-1 if never)"},
                {RPCResult::Type::NUM, "allowed_subsidy", "Allowed coinbase subsidy in satoshis"},
                {RPCResult::Type::STR_AMOUNT, "allowed_subsidy_coin", "Allowed coinbase subsidy in whole coins"},
                {RPCResult::Type::NUM, "base_subsidy", "Base coinbase subsidy (S-Base) in satoshis"},
                {RPCResult::Type::STR_AMOUNT, "base_subsidy_coin", "Base coinbase subsidy (S-Base) in whole coins"},
                {RPCResult::Type::NUM, "bonus_subsidy", "Bonus above base subsidy in satoshis"},
                {RPCResult::Type::STR_AMOUNT, "bonus_subsidy_coin", "Bonus above base subsidy in whole coins"},
            }
        },
        RPCExamples{
            HelpExampleCli("getpoladdressstatus", "\"mflex1q2v22jra8zccm4h9dz9na2pcv57au5xkes6xefe\"") +
            HelpExampleCli("getpoladdressstatus", "\"mflex1q2v22jra8zccm4h9dz9na2pcv57au5xkes6xefe\" \"312\"") +
            HelpExampleRpc("getpoladdressstatus", "\"mflex1q2v22jra8zccm4h9dz9na2pcv57au5xkes6xefe\", \"312\"")
        },
        [](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            node::NodeContext& node = EnsureAnyNodeContext(request.context);
            ChainstateManager& chainman = EnsureChainman(node);

            const int tip_height = GetTipHeight(chainman);

            const std::string address_in = request.params[0].get_str();

            int height = tip_height;
            if (request.params.size() > 1 && !request.params[1].isNull()) {
                height = ParseHeightFlexible(request.params[1]);
            }
            if (height < 0) height = 0;

            const std::vector<unsigned char> tag = Tag12FromAddressString(address_in);

            MinerTagStatus st{false, -1, -1, 0, 0, 0, -1};
            if (const auto st_opt = pol::GetMinerTagStatus(tag); st_opt.has_value()) st = *st_opt;

            const Consensus::Params& consensus = Params().GetConsensus();
            const CAmount allowed = pol::GetAllowedSubsidy(tag, height, consensus);
            const CAmount base = pol::GetBaseSubsidy(height, consensus);
            const CAmount bonus = (allowed > base) ? (allowed - base) : 0;

            uint32_t tag_u32 = 0;
            if (tag.size() >= 4) {
                tag_u32 = (uint32_t)tag[0]
                        | ((uint32_t)tag[1] << 8)
                        | ((uint32_t)tag[2] << 16)
                        | ((uint32_t)tag[3] << 24);
            }

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("tip_height", tip_height);
            obj.pushKV("height", height);
            obj.pushKV("address", address_in);
            obj.pushKV("miner_tag_hex", HexStr(tag));
            obj.pushKV("miner_tag_len", (int)tag.size());
            obj.pushKV("miner_tag_u32", tag_u32);
            obj.pushKV("extranonce1_size", pol::GetConfiguredExtraNonce1Size());

            obj.pushKV("seen", st.seen);
            obj.pushKV("first_seen_height", st.first_seen_height);
            obj.pushKV("last_seen_height", st.last_seen_height);
            obj.pushKV("blocks_seen", st.blocks_seen);
            obj.pushKV("last_seen_time", st.last_seen_time);
            obj.pushKV("points", st.points);
const int level = PolLevelFromPoints(st.seen, st.points);
obj.pushKV("level", level);
obj.pushKV("level_text", level == 0 ? "No level" : ("Level " + std::to_string(level)));
            obj.pushKV("last_seen_month", st.last_seen_month);

            obj.pushKV("allowed_subsidy", allowed);
            obj.pushKV("allowed_subsidy_coin", ValueFromAmount(allowed));
            obj.pushKV("base_subsidy", base);
            obj.pushKV("base_subsidy_coin", ValueFromAmount(base));
            obj.pushKV("bonus_subsidy", bonus);
            obj.pushKV("bonus_subsidy_coin", ValueFromAmount(bonus));

            return obj;
        }
    };
}


void RegisterPoLRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[] = {
        {"blockchain", &getpolallowedtag},
        {"blockchain", &getpoladdressstatus},
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}
