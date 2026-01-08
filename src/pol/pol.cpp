#include <pol/pol.h>

#include <common/args.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <logging.h>
#include <chain.h>
#include <primitives/block.h>
#include <script/script.h>
#include <crypto/sha256.h>
#include <sync.h>
#include <util/strencodings.h>
#include <validation.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pol {

namespace {

Mutex g_pol_mutex;
std::unordered_map<std::string, MinerTagStatus> g_tag_state GUARDED_BY(g_pol_mutex);

static inline int MonthIndex(int height, int month_blocks)
{
    if (month_blocks <= 0) return 0;
    if (height < 0) return 0;
    return height / month_blocks;
}

} // namespace

int GetPolStartHeight()
{
    return gArgs.GetIntArg("-pol_startheight", 1);
}

int GetPolEnforceHeight()
{
    // Enforce PoL early by default. Can be overridden via -pol_enforceheight.
    return gArgs.GetIntArg("-pol_enforceheight", 1);
}

int GetPolMonthBlocks()
{
    return gArgs.GetIntArg("-pol_monthblocks", 4320);
}

int GetConfiguredExtraNonce1Size()
{
    const int n = gArgs.GetIntArg("-pol_extranonce1size", 4);
    if (n < 0) return 0;
    if (n > 16) return 16;
    return n;
}



std::optional<std::vector<unsigned char>> ExtractMinerTagFromBlock(const CBlock& block)
{
    if (block.vtx.empty() || !block.vtx[0]) return std::nullopt;

    const CTransaction& coinbase = *block.vtx[0];
    if (coinbase.vout.empty()) return std::nullopt;

    // Miningcore writes: OP_RETURN <push: "MFLEXID" + tag(4/8/12)>
    static const std::array<unsigned char, 7> kMflexId = { 'M','F','L','E','X','I','D' };

    for (const auto& out : coinbase.vout) {
        const CScript& spk = out.scriptPubKey;

        CScript::const_iterator it = spk.begin();
        opcodetype op;
        std::vector<unsigned char> push;

        if (!spk.GetOp(it, op)) continue;
        if (op != OP_RETURN) continue;

        if (!spk.GetOp(it, op, push)) continue;

        if (push.size() < kMflexId.size() + 4) continue;
        if (!std::equal(kMflexId.begin(), kMflexId.end(), push.begin())) continue;

        const size_t tag_len = push.size() - kMflexId.size();
        if (tag_len == 4 || tag_len == 8 || tag_len == 12) {
            return std::vector<unsigned char>(push.begin() + kMflexId.size(), push.end());
        }
    }

    return std::nullopt;
}

void OnConnectBlock(const CBlock& block, int height, int64_t block_time)
{
    if (height < GetPolStartHeight()) return;

    const auto tag_opt = ExtractMinerTagFromBlock(block);
    if (!tag_opt.has_value()) return;

    const std::vector<unsigned char>& tag = *tag_opt;
    const std::string key = HexStr(tag);

    const int month_blocks = GetPolMonthBlocks();
    const int cur_month = MonthIndex(height, month_blocks);

    LOCK(g_pol_mutex);
    MinerTagStatus& s = g_tag_state[key];

    if (!s.seen) {
        s.seen = true;
        s.first_seen_height = height;
        s.last_seen_month = cur_month;
        // First active month => +2 points (clamped). This is deterministic.
        s.points = std::clamp(s.points + 2, 0, 24);
    } else {
        // Month transitions: +2 per active month, -1 per missed month
        if (cur_month > s.last_seen_month) {
            const int missed = cur_month - s.last_seen_month - 1;
            if (missed > 0) s.points -= missed * 1;
            s.points += 2;
            s.points = std::clamp(s.points, 0, 24);
            s.last_seen_month = cur_month;
        }
    }

    s.last_seen_height = height;
    s.last_seen_time = block_time;
    s.blocks_seen++;

    LogPrintLevel(BCLog::VALIDATION, BCLog::Level::Debug,
                  "PoL-TAG connect height=%d tag=%s len=%u points=%d month=%d\n",
                  height, key, (unsigned)tag.size(), s.points, s.last_seen_month);
}

std::optional<MinerTagStatus> GetMinerTagStatus(const std::vector<unsigned char>& tag)
{
    const std::string key = HexStr(tag);
    LOCK(g_pol_mutex);

    auto it = g_tag_state.find(key);
    if (it == g_tag_state.end()) return std::nullopt;
    return it->second;
}

CAmount GetAllowedSubsidy(const std::vector<unsigned char>& tag, int height, const Consensus::Params& consensus)
{
    // Base subsidy from chain (sats)
    const CAmount S = GetBlockSubsidy(height, consensus);

    // Split in half: base + loyalty
    const CAmount S_base = S / 2;
    const CAmount S_loyal = S - S_base;

    int points = 0;
    if (auto st = GetMinerTagStatus(tag)) {
        points = std::clamp(st->points, 0, 24);
    } else {
        // not seen yet => 0 points (no bonus unlocked)
        points = 0;
    }

    // allowed = base + loyal * (points/24)
    const CAmount bonus = (S_loyal * points) / 24;
    return S_base + bonus;
}

CAmount GetBaseSubsidy(int height, const Consensus::Params& consensus)
{
    const CAmount full = GetBlockSubsidy(height, consensus);
    return full / 2;
}

std::vector<unsigned char> Tag12FromScriptPubKey(const CScript& script)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> hash{};
    CSHA256().Write(script.data(), script.size()).Finalize(hash.data());
    return std::vector<unsigned char>(hash.begin(), hash.begin() + POL_TAG_LEN);
}

CAmount CoinbaseValueToTagScript(const CBlock& block, const std::vector<unsigned char>& tag)
{
    if (block.vtx.empty() || !block.vtx[0]) return 0;

    const CTransaction& coinbase = *block.vtx[0];
    CAmount total = 0;

    for (const auto& out : coinbase.vout) {
        if (out.nValue <= 0) continue;
        if (out.scriptPubKey.IsUnspendable()) continue;

        const auto out_tag = Tag12FromScriptPubKey(out.scriptPubKey);
        if (out_tag == tag) {
            total += out.nValue;
        }
    }

    return total;
}




void RebuildFromActiveChain(ChainstateManager& chainman, const Consensus::Params& consensus)
{
    // NOTE: This is required so PoL tracking is deterministic after restarts.
    // The in-memory tag-state is normally built while blocks are CONNECTED.
    // When restarting from an already-built chainstate, old blocks are NOT
    // re-connected, so we must rebuild the tag-state from disk once.
    (void)consensus;

    LOCK(cs_main);

    const CChain& chain = chainman.ActiveChain();
    const CBlockIndex* tip = chain.Tip();
    if (!tip) {
        return;
    }

    {
        LOCK(g_pol_mutex);
        g_tag_state.clear();
    }

    const int start_height = std::max(0, GetPolStartHeight());
    LogPrintLevel(BCLog::VALIDATION, BCLog::Level::Info,
        "PoL-TAG rebuild: scanning blocks [%d..%d]\n", start_height, tip->nHeight);

    for (int height = start_height; height <= tip->nHeight; ++height) {
        const CBlockIndex* pindex = chain[height];
        if (!pindex) {
            continue;
        }

        CBlock block;
        if (!chainman.m_blockman.ReadBlock(block, *pindex)) {
            LogPrintLevel(BCLog::VALIDATION, BCLog::Level::Warning,
                "PoL-TAG rebuild: ReadBlock failed at height=%d\n", height);
            continue;
        }

        // Reuse the same hook logic so we don't duplicate business rules.
        OnConnectBlock(block, height, pindex->GetBlockTime());
    }

    {
        LOCK(g_pol_mutex);
        LogPrintLevel(BCLog::VALIDATION, BCLog::Level::Info,
            "PoL-TAG rebuild: done (tags=%u)\n", (unsigned) g_tag_state.size());
    }
}

} // namespace pol
