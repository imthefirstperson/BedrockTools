#include "veinminer.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Level.hpp>
#include <bedrocktools/sdk/world/HitResult.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>
#include "core/GameHooks.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

using namespace bedrocktools;
using namespace bedrocktools::sdk;
using namespace bedrocktools::memory;

// ─── Ore / log allow-list ─────────────────────────────────────────────────────
// Matches the full Bedrock 1.26 block name string as returned by BlockType::mNameInfo.

static constexpr std::array<const char*, 27> kAllowedBlocks {{
    // ── Overworld ores ───────────────────────────────────────────────────────
    "minecraft:coal_ore",              "minecraft:deepslate_coal_ore",
    "minecraft:iron_ore",              "minecraft:deepslate_iron_ore",
    "minecraft:copper_ore",            "minecraft:deepslate_copper_ore",
    "minecraft:gold_ore",              "minecraft:deepslate_gold_ore",
    "minecraft:redstone_ore",          "minecraft:deepslate_redstone_ore",
    "minecraft:lit_redstone_ore",      "minecraft:lit_deepslate_redstone_ore",
    "minecraft:lapis_ore",             "minecraft:deepslate_lapis_ore",
    "minecraft:diamond_ore",           "minecraft:deepslate_diamond_ore",
    "minecraft:emerald_ore",           "minecraft:deepslate_emerald_ore",
    // ── Nether ores ──────────────────────────────────────────────────────────
    "minecraft:nether_gold_ore",
    "minecraft:nether_quartz_ore",
    "minecraft:ancient_debris",
    // ── Logs (all 6 axes are the same block name; state is ignored) ──────────
    "minecraft:oak_log",               "minecraft:spruce_log",
    "minecraft:birch_log",             "minecraft:jungle_log",
    "minecraft:acacia_log",            "minecraft:dark_oak_log",
    "minecraft:mangrove_log",
}};

bool VeinMinerModule::isAllowedBlock(const std::string& name) {
    for (const char* allowed : kAllowedBlocks)
        if (name == allowed) return true;
    return false;
}

// ─── Block name reader ────────────────────────────────────────────────────────
//
// Walk: Block* → BlockType* (+0x68) → NameInfo* (+0x88)
//       → HashedString (full name) at +0x40 → std::string at HashedString+0x8
//
// Returns empty string on any null pointer in the chain.

std::string VeinMinerModule::blockNameAt(void* blockSource, int x, int y, int z) {
    using BlockSourceGetBlockFn = void*(*)(void*, int, int, int);
    static BlockSourceGetBlockFn sGetBlock = nullptr;
    if (!sGetBlock)
        sGetBlock = reinterpret_cast<BlockSourceGetBlockFn>(
            resolve(SignatureId::BlockSourceGetBlock));
    if (!sGetBlock || !blockSource) return {};

    void* block = sGetBlock(blockSource, x, y, z);
    if (!block) return {};

    void* blockType = field<void*>(block, offsets::Block::mBlockType);
    if (!blockType) return {};

    void* nameInfo = field<void*>(blockType, offsets::BlockType::mNameInfo);
    if (!nameInfo) return {};

    // mFullName is a HashedString embedded at +0x40 inside nameInfo;
    // the std::string member lives at HashedString+0x8.
    auto* hashedBase = reinterpret_cast<std::uint8_t*>(nameInfo)
                       + offsets::NameInfo::mFullName;

    return field<std::string>(hashedBase, offsets::HashedString::mString);
}

// ─── BFS vein finder ─────────────────────────────────────────────────────────
//
// Starts from `origin` (the block the player just broke — already air in the
// world, so the origin is NOT re-added to the result), fans out to all 6-face
// neighbours, collecting positions whose block name matches `targetName`.
// Stops when `limit` is reached or the frontier is exhausted.

static constexpr std::array<IPos, 6> kFaceOffsets {{
    IPos{ 1, 0, 0}, IPos{-1, 0, 0},
    IPos{ 0, 1, 0}, IPos{ 0,-1, 0},
    IPos{ 0, 0, 1}, IPos{ 0, 0,-1},
}};

std::vector<IPos> VeinMinerModule::bfsVein(void* blockSource,
                                            const IPos& origin,
                                            const std::string& targetName,
                                            int limit) {
    if (!blockSource || targetName.empty()) return {};

    std::vector<IPos>          result;
    std::queue<IPos>           frontier;
    std::unordered_set<IPos, IPosHash> visited;

    // Seed with the six neighbours of the broken block.
    // The origin itself is already gone; we don't queue it.
    visited.insert(origin);
    for (const auto& face : kFaceOffsets) {
        IPos nb{ origin.x + face.x, origin.y + face.y, origin.z + face.z };
        if (visited.insert(nb).second) {
            std::string name = blockNameAt(blockSource, nb.x, nb.y, nb.z);
            if (name == targetName) {
                frontier.push(nb);
                result.push_back(nb);
            }
        }
    }

    while (!frontier.empty() && static_cast<int>(result.size()) < limit) {
        IPos cur = frontier.front();
        frontier.pop();

        for (const auto& face : kFaceOffsets) {
            if (static_cast<int>(result.size()) >= limit) break;

            IPos nb{ cur.x + face.x, cur.y + face.y, cur.z + face.z };
            if (!visited.insert(nb).second) continue;

            std::string name = blockNameAt(blockSource, nb.x, nb.y, nb.z);
            if (name != targetName) continue;

            frontier.push(nb);
            result.push_back(nb);
        }
    }

    return result;
}

// ─── Packet helpers ───────────────────────────────────────────────────────────
//
// Follows the same pattern as AutoReQ — CommandRequestPacket (ID 77) with
// "/setblock x y z air destroy".
//
// This fires a server-side block removal that drops loot, triggers block update
// events, and works identically to the player breaking the block manually.
// Requires the world to have cheats enabled (i.e. the player can run /setblock).
//
// Field layout (relative to packet base pointer):
//   payload         = pkt + Packet::Size (0x30)
//   mCommand        = payload + 0          std::string
//   mOrigin         = payload + 24         CommandOriginData
//   CommandOriginData::mType = +0          uint8_t (0 = player)
//   mInternalSource = payload + 84         bool

void VeinMinerModule::sendSetblock(void* clientInstance, const IPos& pos) {
    using CreatePacketFn      = std::shared_ptr<void>(*)(int);
    using GetPacketSenderFn   = void*(*)(void*);
    using SendToServerFn      = void*(*)(void*, void*);

    static CreatePacketFn    sCreatePacket    = nullptr;
    static GetPacketSenderFn sGetPacketSender = nullptr;
    static SendToServerFn    sSendToServer    = nullptr;

    if (!sCreatePacket)
        sCreatePacket = reinterpret_cast<CreatePacketFn>(
            resolve(SignatureId::MinecraftPacketsCreatePacket));
    if (!sGetPacketSender)
        sGetPacketSender = reinterpret_cast<GetPacketSenderFn>(
            resolve(SignatureId::ClientInstanceGetPacketSender));
    if (!sSendToServer)
        sSendToServer = reinterpret_cast<SendToServerFn>(
            resolve(SignatureId::LoopbackPacketSenderSendToServer));

    if (!sCreatePacket || !sGetPacketSender || !sSendToServer || !clientInstance)
        return;

    void* sender = sGetPacketSender(clientInstance);
    if (!sender) return;

    // Build command string: "/setblock X Y Z air destroy"
    // "destroy" mode drops the loot, triggers block updates, plays the sound —
    // identical to a player break. Use "replace" for silent removal.
    std::string cmd = "/setblock "
        + std::to_string(pos.x) + " "
        + std::to_string(pos.y) + " "
        + std::to_string(pos.z)
        + " air destroy";

    // Packet ID 77 = CommandRequestPacket
    std::shared_ptr<void> pktSp = sCreatePacket(77);
    void* pkt = pktSp.get();
    if (!pkt) return;

    auto* payload = reinterpret_cast<std::uint8_t*>(pkt)
                    + offsets::Packet::Size;

    // mCommand (std::string at payload+0)
    *reinterpret_cast<std::string*>(
        payload + offsets::CommandRequestPacketPayload::mCommand) = cmd;

    // CommandOriginData::mType = 0 (player origin)
    *reinterpret_cast<std::uint8_t*>(
        payload
        + offsets::CommandRequestPacketPayload::mOrigin
        + offsets::CommandOriginData::mType) = 0;

    // mInternalSource = true (internal / trusted)
    *reinterpret_cast<bool*>(
        payload + offsets::CommandRequestPacketPayload::mInternalSource) = true;

    sSendToServer(sender, pkt);
}

// ─── Module lifecycle ─────────────────────────────────────────────────────────

VeinMinerModule::VeinMinerModule()
    : Module("Vein Miner",
             "Break an ore or log and all connected blocks of the same type "
             "are mined automatically. Requires cheats enabled on the world.") {}

VeinMinerModule::~VeinMinerModule() {
    if (m_tickSub) events::bus().unsubscribe(m_tickSub);
}

void VeinMinerModule::onInit() {}

void VeinMinerModule::onEnable() {
    m_watching  = false;
    m_watchName.clear();
    m_queue.clear();
    m_queueIdx  = 0;
    m_timerMs   = 0.0f;

    m_tickSub = events::bus().subscribe<events::LocalPlayerTickEvent>(
        [this](events::LocalPlayerTickEvent& ev) {

        auto* player = ev.player;
        if (!player) return;

        // ── Sneak gate ────────────────────────────────────────────────────────
        // Categories bitmask: bit 0x40 is the sneak flag in Bedrock 1.26.
        if (requireSneaking && !(player->categories() & 0x40u))
            return;

        // ── Resolve block source ──────────────────────────────────────────────
        auto* dim = player->dimension();
        void* blockSource = dim
            ? field<void*>(dim, offsets::Dimension::mBlockSource)
            : nullptr;

        // ── Break detection ───────────────────────────────────────────────────
        // Read stored hit result from the Level each tick.
        auto* level      = player->level();
        auto* hitResult  = level ? level->storedHitResult() : nullptr;

        if (m_watching && blockSource && !m_watchName.empty()) {
            // Check whether the block we were watching has become air.
            std::string current = blockNameAt(blockSource,
                                              m_watchPos.x, m_watchPos.y, m_watchPos.z);
            bool isAir = current.empty()
                      || current == "minecraft:air"
                      || current.find("air") != std::string::npos;

            if (isAir && m_queue.empty()) {
                // Block is gone — run the BFS from its position.
                bool allowed = !oresOnly || isAllowedBlock(m_watchName);
                if (allowed) {
                    m_queue    = bfsVein(blockSource, m_watchPos, m_watchName, maxBlocks);
                    m_queueIdx = 0;
                    m_timerMs  = 0.0f;
                }
                // Stop watching whether we queued anything or not.
                m_watching = false;
                m_watchName.clear();
            }
        }

        // ── Update watch target ───────────────────────────────────────────────
        // type() == 1  →  block hit
        // type() == 2  →  entity hit
        // type() == 0  →  miss
        if (hitResult && hitResult->type() == 1 && blockSource) {
            const Vec3& fp = hitResult->position();
            IPos bp{
                static_cast<int>(std::floor(fp.x)),
                static_cast<int>(std::floor(fp.y)),
                static_cast<int>(std::floor(fp.z))
            };

            std::string name = blockNameAt(blockSource, bp.x, bp.y, bp.z);
            bool isAir = name.empty() || name.find("air") != std::string::npos;

            if (!isAir) {
                // A real block is under the crosshair — start/update the watch.
                m_watching  = true;
                m_watchPos  = bp;
                m_watchName = name;
            } else if (!m_watching) {
                // Hit result is on air (can happen during transition) — nothing to watch.
                m_watchName.clear();
            }
        } else if (!m_watching) {
            // Not targeting a block and nothing previously watched.
            m_watchName.clear();
        }

        // ── Process break queue ───────────────────────────────────────────────
        if (!m_queue.empty() && m_queueIdx < m_queue.size()) {
            // Approximate tick delta: LocalPlayerTickEvent fires at ~20 Hz (50 ms/tick).
            m_timerMs += 50.0f;

            if (m_timerMs >= breakDelayMs) {
                m_timerMs = 0.0f;

                void* ci = bedrocktools::core::gamehooks::clientInstance();
                if (ci) {
                    // Verify the block is still there before sending the command.
                    // Skips already-broken blocks (e.g. TNT chain or another player).
                    const IPos& target = m_queue[m_queueIdx];
                    if (blockSource) {
                        std::string cur = blockNameAt(blockSource,
                                                      target.x, target.y, target.z);
                        bool stillThere = !cur.empty() && cur.find("air") == std::string::npos;
                        if (stillThere) {
                            sendSetblock(ci, target);
                        }
                    } else {
                        sendSetblock(ci, m_queue[m_queueIdx]);
                    }
                }

                ++m_queueIdx;
                if (m_queueIdx >= m_queue.size()) {
                    m_queue.clear();
                    m_queueIdx = 0;
                }
            }
        }
    });
}

void VeinMinerModule::onDisable() {
    events::bus().unsubscribe(m_tickSub);
    m_tickSub = 0;
    m_watching = false;
    m_watchName.clear();
    m_queue.clear();
    m_queueIdx = 0;
    m_timerMs  = 0.0f;
}

// ─── Config ───────────────────────────────────────────────────────────────────

void VeinMinerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("maxBlocks"))       maxBlocks       = j["maxBlocks"].get<int>();
    if (j.contains("oresOnly"))        oresOnly        = j["oresOnly"].get<bool>();
    if (j.contains("requireSneaking")) requireSneaking = j["requireSneaking"].get<bool>();
    if (j.contains("breakDelayMs"))    breakDelayMs    = j["breakDelayMs"].get<float>();
}

void VeinMinerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["maxBlocks"]       = maxBlocks;
    j["oresOnly"]        = oresOnly;
    j["requireSneaking"] = requireSneaking;
    j["breakDelayMs"]    = breakDelayMs;
}
