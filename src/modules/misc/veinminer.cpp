#include "veinminer.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Level.hpp>
#include <bedrocktools/sdk/world/HitResult.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
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

// -----------------------------------------------------------------------------
// Local ABI-compatible BlockPos.
//
// IMPORTANT:
// Do not replace this with bedrocktools::sdk::BlockPos. Some BedrockTools
// versions do not expose that type in the SDK headers, while the native
// function still expects the same three-int layout.
// -----------------------------------------------------------------------------

struct VeinMinerBlockPos {
    int x;
    int y;
    int z;
};

// BlockSource::getBlock(BlockPos const&)
using BlockSourceGetBlockFn =
    void* (*)(void*, const VeinMinerBlockPos&);

// -----------------------------------------------------------------------------
// Allowed blocks
// -----------------------------------------------------------------------------

static constexpr std::array<const char*, 28> kAllowedBlocks{{
    "minecraft:coal_ore",
    "minecraft:deepslate_coal_ore",

    "minecraft:iron_ore",
    "minecraft:deepslate_iron_ore",

    "minecraft:copper_ore",
    "minecraft:deepslate_copper_ore",

    "minecraft:gold_ore",
    "minecraft:deepslate_gold_ore",

    "minecraft:redstone_ore",
    "minecraft:deepslate_redstone_ore",

    "minecraft:lit_redstone_ore",
    "minecraft:lit_deepslate_redstone_ore",

    "minecraft:lapis_ore",
    "minecraft:deepslate_lapis_ore",

    "minecraft:diamond_ore",
    "minecraft:deepslate_diamond_ore",

    "minecraft:emerald_ore",
    "minecraft:deepslate_emerald_ore",

    "minecraft:nether_gold_ore",
    "minecraft:nether_quartz_ore",
    "minecraft:ancient_debris",

    "minecraft:oak_log",
    "minecraft:spruce_log",
    "minecraft:birch_log",
    "minecraft:jungle_log",
    "minecraft:acacia_log",
    "minecraft:dark_oak_log",
    "minecraft:mangrove_log"
}};

bool VeinMinerModule::isAllowedBlock(const std::string& name) {
    for (const char* allowed : kAllowedBlocks) {
        if (name == allowed)
            return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Block name
// -----------------------------------------------------------------------------

std::string VeinMinerModule::blockNameAt(
    void* blockSource,
    int x,
    int y,
    int z
) {
    static BlockSourceGetBlockFn sGetBlock = nullptr;

    if (!sGetBlock) {
        const uintptr_t address =
            resolve(SignatureId::BlockSourceGetBlock);

        if (!address)
            return {};

        sGetBlock =
            reinterpret_cast<BlockSourceGetBlockFn>(address);
    }

    if (!sGetBlock || !blockSource)
        return {};

    VeinMinerBlockPos pos{
        x,
        y,
        z
    };

    void* block = sGetBlock(blockSource, pos);

    if (!block)
        return {};

    void* blockType =
        field<void*>(
            block,
            offsets::Block::mBlockType
        );

    if (!blockType)
        return {};

    void* nameInfo =
        field<void*>(
            blockType,
            offsets::BlockType::mNameInfo
        );

    if (!nameInfo)
        return {};

    auto* fullNameBase =
        reinterpret_cast<std::uint8_t*>(nameInfo)
        + offsets::NameInfo::mFullName;

    return field<std::string>(
        fullNameBase,
        offsets::HashedString::mString
    );
}

// -----------------------------------------------------------------------------
// BFS
// -----------------------------------------------------------------------------

static constexpr std::array<IPos, 6> kFaceOffsets{{
    IPos{ 1, 0, 0},
    IPos{-1, 0, 0},
    IPos{ 0, 1, 0},
    IPos{ 0,-1, 0},
    IPos{ 0, 0, 1},
    IPos{ 0, 0,-1}
}};

std::vector<IPos> VeinMinerModule::bfsVein(
    void* blockSource,
    const IPos& origin,
    const std::string& targetName,
    int limit
) {
    std::vector<IPos> result;

    if (!blockSource ||
        targetName.empty() ||
        limit <= 0) {
        return result;
    }

    std::queue<IPos> queue;
    std::unordered_set<IPos, IPosHash> visited;

    visited.reserve(static_cast<std::size_t>(limit) * 2 + 16);

    visited.insert(origin);

    for (const IPos& offset : kFaceOffsets) {
        IPos next{
            origin.x + offset.x,
            origin.y + offset.y,
            origin.z + offset.z
        };

        if (!visited.insert(next).second)
            continue;

        const std::string name =
            blockNameAt(
                blockSource,
                next.x,
                next.y,
                next.z
            );

        if (name == targetName) {
            queue.push(next);
            result.push_back(next);

            if (static_cast<int>(result.size()) >= limit)
                return result;
        }
    }

    while (!queue.empty() &&
           static_cast<int>(result.size()) < limit) {

        const IPos current = queue.front();
        queue.pop();

        for (const IPos& offset : kFaceOffsets) {
            if (static_cast<int>(result.size()) >= limit)
                break;

            IPos next{
                current.x + offset.x,
                current.y + offset.y,
                current.z + offset.z
            };

            if (!visited.insert(next).second)
                continue;

            const std::string name =
                blockNameAt(
                    blockSource,
                    next.x,
                    next.y,
                    next.z
                );

            if (name != targetName)
                continue;

            queue.push(next);
            result.push_back(next);
        }
    }

    return result;
}

// -----------------------------------------------------------------------------
// Command packet
// -----------------------------------------------------------------------------

void VeinMinerModule::sendSetblock(
    void* clientInstance,
    const IPos& pos
) {
    using CreatePacketFn =
        std::shared_ptr<void> (*)(int);

    using GetPacketSenderFn =
        void* (*)(void*);

    using SendToServerFn =
        void* (*)(void*, void*);

    static CreatePacketFn createPacket = nullptr;
    static GetPacketSenderFn getPacketSender = nullptr;
    static SendToServerFn sendToServer = nullptr;

    if (!createPacket) {
        createPacket =
            reinterpret_cast<CreatePacketFn>(
                resolve(
                    SignatureId::MinecraftPacketsCreatePacket
                )
            );
    }

    if (!getPacketSender) {
        getPacketSender =
            reinterpret_cast<GetPacketSenderFn>(
                resolve(
                    SignatureId::ClientInstanceGetPacketSender
                )
            );
    }

    if (!sendToServer) {
        sendToServer =
            reinterpret_cast<SendToServerFn>(
                resolve(
                    SignatureId::LoopbackPacketSenderSendToServer
                )
            );
    }

    if (!createPacket ||
        !getPacketSender ||
        !sendToServer ||
        !clientInstance) {
        return;
    }

    void* sender =
        getPacketSender(clientInstance);

    if (!sender)
        return;

    std::string command =
        "/setblock "
        + std::to_string(pos.x)
        + " "
        + std::to_string(pos.y)
        + " "
        + std::to_string(pos.z)
        + " air destroy";

    std::shared_ptr<void> packet =
        createPacket(77);

    if (!packet)
        return;

    void* packetPtr = packet.get();

    if (!packetPtr)
        return;

    auto payload =
        reinterpret_cast<std::uintptr_t>(packetPtr)
        + offsets::Packet::Size;

    *reinterpret_cast<std::string*>(
        payload +
        offsets::CommandRequestPacketPayload::mCommand
    ) = command;

    *reinterpret_cast<std::uint8_t*>(
        payload +
        offsets::CommandRequestPacketPayload::mOrigin +
        offsets::CommandOriginData::mType
    ) = 0;

    *reinterpret_cast<bool*>(
        payload +
        offsets::CommandRequestPacketPayload::mInternalSource
    ) = true;

    sendToServer(
        sender,
        packetPtr
    );
}

// -----------------------------------------------------------------------------
// Constructor / destructor
// -----------------------------------------------------------------------------

VeinMinerModule::VeinMinerModule()
    : Module(
        "Vein Miner",
        "Break an ore or log and automatically mine connected blocks of the same type."
      ) {

    showInMenu = true;
}

VeinMinerModule::~VeinMinerModule() {
    if (m_tickSub) {
        events::bus().unsubscribe(m_tickSub);
        m_tickSub = 0;
    }
}

// -----------------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------------

void VeinMinerModule::onInit() {
}

// -----------------------------------------------------------------------------
// Enable
// -----------------------------------------------------------------------------

void VeinMinerModule::onEnable() {
    m_watching = false;
    m_watchPos = {};
    m_watchName.clear();

    m_queue.clear();
    m_queueIdx = 0;
    m_timerMs = 0.0f;

    if (m_tickSub) {
        events::bus().unsubscribe(m_tickSub);
        m_tickSub = 0;
    }

    m_tickSub =
        events::bus().subscribe<
            events::LocalPlayerTickEvent
        >(
            [this](events::LocalPlayerTickEvent& event) {

                auto* player = event.player;

                if (!player)
                    return;

                // -------------------------------------------------------------
                // Optional sneak requirement
                // -------------------------------------------------------------

                if (requireSneaking) {
                    if (!(player->categories() & 0x40u))
                        return;
                }

                // -------------------------------------------------------------
                // Dimension / BlockSource
                // -------------------------------------------------------------

                auto* dimension =
                    player->dimension();

                void* blockSource = nullptr;

                if (dimension) {
                    blockSource =
                        field<void*>(
                            dimension,
                            offsets::Dimension::mBlockSource
                        );
                }

                if (!blockSource)
                    return;

                // -------------------------------------------------------------
                // Level / hit result
                // -------------------------------------------------------------

                auto* level =
                    player->level();

                auto* hitResult =
                    level
                    ? level->storedHitResult()
                    : nullptr;

                // -------------------------------------------------------------
                // Detect completion of the watched block
                // -------------------------------------------------------------

                if (m_watching &&
                    !m_watchName.empty()) {

                    const std::string current =
                        blockNameAt(
                            blockSource,
                            m_watchPos.x,
                            m_watchPos.y,
                            m_watchPos.z
                        );

                    const bool isAir =
                        current.empty() ||
                        current == "minecraft:air" ||
                        current.find("air") != std::string::npos;

                    if (isAir) {

                        const bool allowed =
                            !oresOnly ||
                            isAllowedBlock(m_watchName);

                        if (allowed) {

                            m_queue =
                                bfsVein(
                                    blockSource,
                                    m_watchPos,
                                    m_watchName,
                                    maxBlocks
                                );

                            m_queueIdx = 0;
                            m_timerMs = 0.0f;
                        }

                        m_watching = false;
                        m_watchName.clear();
                    }
                }

                // -------------------------------------------------------------
                // Find the block currently under the crosshair
                // -------------------------------------------------------------

                if (hitResult &&
                    hitResult->type() == 1) {

                    /*
                     * HitResult::position() is a floating-point hit location,
                     * not necessarily the integer block origin.
                     *
                     * We use the ray direction and move the hit point a tiny
                     * amount back toward the player before flooring it. This
                     * prevents a hit exactly on a block face from selecting
                     * the neighbouring block.
                     */

                    const auto& start =
                        hitResult->startPosition();

                    const auto& hit =
                        hitResult->position();

                    float dx =
                        hit.x - start.x;

                    float dy =
                        hit.y - start.y;

                    float dz =
                        hit.z - start.z;

                    const float length =
                        std::sqrt(
                            dx * dx +
                            dy * dy +
                            dz * dz
                        );

                    float sampleX = hit.x;
                    float sampleY = hit.y;
                    float sampleZ = hit.z;

                    if (length > 0.0001f) {

                        constexpr float epsilon = 0.001f;

                        dx /= length;
                        dy /= length;
                        dz /= length;

                        sampleX -= dx * epsilon;
                        sampleY -= dy * epsilon;
                        sampleZ -= dz * epsilon;
                    }

                    IPos blockPos{
                        static_cast<int>(
                            std::floor(sampleX)
                        ),
                        static_cast<int>(
                            std::floor(sampleY)
                        ),
                        static_cast<int>(
                            std::floor(sampleZ)
                        )
                    };

                    const std::string name =
                        blockNameAt(
                            blockSource,
                            blockPos.x,
                            blockPos.y,
                            blockPos.z
                        );

                    const bool isAir =
                        name.empty() ||
                        name.find("air") != std::string::npos;

                    if (!isAir) {

                        /*
                         * Only replace the watched target when the target
                         * actually changed. This prevents the watch state from
                         * constantly resetting while the player is mining.
                         */

                        if (!m_watching ||
                            m_watchPos.x != blockPos.x ||
                            m_watchPos.y != blockPos.y ||
                            m_watchPos.z != blockPos.z ||
                            m_watchName != name) {

                            m_watching = true;
                            m_watchPos = blockPos;
                            m_watchName = name;
                        }
                    }
                }
                else if (!m_watching) {
                    m_watchName.clear();
                }

                // -------------------------------------------------------------
                // Process queued vein blocks
                // -------------------------------------------------------------

                if (m_queue.empty())
                    return;

                if (m_queueIdx >= m_queue.size()) {
                    m_queue.clear();
                    m_queueIdx = 0;
                    m_timerMs = 0.0f;
                    return;
                }

                /*
                 * LocalPlayerTickEvent is approximately 20 Hz.
                 *
                 * Do not send every block in the same tick. Apart from being
                 * unnecessary, that can make the client/server command queue
                 * unstable on larger veins.
                 */

                m_timerMs += 50.0f;

                const float delay =
                    std::max(
                        0.0f,
                        breakDelayMs
                    );

                if (m_timerMs < delay)
                    return;

                m_timerMs = 0.0f;

                void* clientInstance =
                    bedrocktools::core::gamehooks::clientInstance();

                if (!clientInstance)
                    return;

                const IPos target =
                    m_queue[m_queueIdx];

                // -------------------------------------------------------------
                // Make sure the target still exists.
                // -------------------------------------------------------------

                const std::string current =
                    blockNameAt(
                        blockSource,
                        target.x,
                        target.y,
                        target.z
                    );

                const bool stillThere =
                    !current.empty() &&
                    current.find("air") == std::string::npos;

                if (stillThere) {

                    /*
                     * Only mine the exact same block type.
                     *
                     * This is important for redstone, lit redstone and
                     * deepslate variants.
                     */

                    if (!m_watchName.empty() &&
                        current != m_watchName) {

                        ++m_queueIdx;
                    }
                    else {
                        sendSetblock(
                            clientInstance,
                            target
                        );

                        ++m_queueIdx;
                    }
                }
                else {
                    ++m_queueIdx;
                }

                // -------------------------------------------------------------
                // Queue finished
                // -------------------------------------------------------------

                if (m_queueIdx >= m_queue.size()) {
                    m_queue.clear();
                    m_queueIdx = 0;
                    m_timerMs = 0.0f;
                }
            }
        );
}

// -----------------------------------------------------------------------------
// Disable
// -----------------------------------------------------------------------------

void VeinMinerModule::onDisable() {
    if (m_tickSub) {
        events::bus().unsubscribe(m_tickSub);
        m_tickSub = 0;
    }

    m_watching = false;
    m_watchPos = {};
    m_watchName.clear();

    m_queue.clear();
    m_queueIdx = 0;
    m_timerMs = 0.0f;
}

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

void VeinMinerModule::loadConfig(
    const nlohmann::json& j
) {
    Module::loadConfig(j);

    if (j.contains("maxBlocks"))
        maxBlocks =
            j["maxBlocks"].get<int>();

    if (j.contains("oresOnly"))
        oresOnly =
            j["oresOnly"].get<bool>();

    if (j.contains("requireSneaking"))
        requireSneaking =
            j["requireSneaking"].get<bool>();

    if (j.contains("breakDelayMs"))
        breakDelayMs =
            j["breakDelayMs"].get<float>();
}

void VeinMinerModule::saveConfig(
    nlohmann::json& j
) {
    Module::saveConfig(j);

    j["maxBlocks"] =
        maxBlocks;

    j["oresOnly"] =
        oresOnly;

    j["requireSneaking"] =
        requireSneaking;

    j["breakDelayMs"] =
        breakDelayMs;
}
