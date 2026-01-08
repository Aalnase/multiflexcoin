#pragma once

#include <consensus/amount.h>
#include <consensus/params.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class CBlock;
class CScript;
class ChainstateManager;

/**
 * In-memory PoL tracking state per miner-tag.
 *
 * The PoL rules in Multiflex are derived from on-chain blocks (coinbase OP_RETURN
 * plus per-month activity). This struct is what we store per tag.
 */
struct MinerTagStatus
{
    bool seen{false};

    int first_seen_height{-1};
    int last_seen_height{-1};

    uint32_t blocks_seen{0};
    int64_t last_seen_time{0};

    // PoL month-based points (0..24)
    int points{0};
    int last_seen_month{-1};
};

namespace pol {

// We standardize on a 12-byte tag (96-bit) for PoL identity.
static constexpr size_t POL_TAG_LEN = 12;

// Config
int GetPolStartHeight();     // default 1
int GetPolEnforceHeight();   // default 110 (test), normally start + 4320
int GetPolMonthBlocks();     // default 4320 (Multiflex-month)
int GetConfiguredExtraNonce1Size(); // informational (RPC)

// Tag extract (from coinbase OP_RETURN "MFLEXID"+tag)
std::optional<std::vector<unsigned char>> ExtractMinerTagFromBlock(const CBlock& block);

// Tracking hook (call from ConnectBlock)
void OnConnectBlock(const CBlock& block, int height, int64_t block_time);

// Query current in-memory status for a tag
std::optional<MinerTagStatus> GetMinerTagStatus(const std::vector<unsigned char>& tag);

// Allowed subsidy (sats) for (height, tag)
CAmount GetAllowedSubsidy(const std::vector<unsigned char>& tag, int height, const Consensus::Params& consensus);

// Base subsidy (S_base) at height (currently defined as 50% of the block subsidy)
CAmount GetBaseSubsidy(int height, const Consensus::Params& consensus);

// Derive PoL tag from a scriptPubKey: first 12 bytes of SHA256(scriptPubKey-bytes).
std::vector<unsigned char> Tag12FromScriptPubKey(const CScript& script);

// Sum of coinbase outputs that pay to scripts whose Tag12FromScriptPubKey(...) matches tag.
CAmount CoinbaseValueToTagScript(const CBlock& block, const std::vector<unsigned char>& tag);

// Rebuild PoL in-memory state by scanning the active chain (needed after restarts).
void RebuildFromActiveChain(ChainstateManager& chainman, const Consensus::Params& consensus);

} // namespace pol
