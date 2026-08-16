#pragma once

#include "../Module.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

// ─── Hashable integer block position ─────────────────────────────────────────

struct IPos {
    int x, y, z;

    bool operator==(const IPos& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct IPosHash {
    std::size_t operator()(const IPos& p) const noexcept {
        // FNV-inspired mix — avoids clustering on aligned ore veins
        std::size_t h = 2166136261u;
        h = (h ^ static_cast<std::uint32_t>(p.x)) * 16777619u;
        h = (h ^ static_cast<std::uint32_t>(p.y)) * 16777619u;
        h = (h ^ static_cast<std::uint32_t>(p.z)) * 16777619u;
        return h;
    }
};

// ─── VeinMinerModule ──────────────────────────────────────────────────────────

class VeinMinerModule : public Module {
public:
    VeinMinerModule();
    ~VeinMinerModule() override;

    void onInit()    override;
    void onEnable()  override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j)       override;

    // ── Configurable parameters ──────────────────────────────────────────────
    int   maxBlocks       = 64;     // hard cap on BFS result size
    bool  oresOnly        = true;   // restrict to the built-in ore/log allow-list
    bool  requireSneaking = false;  // only activate while player is sneaking
    float breakDelayMs    = 50.0f;  // ms between each queued setblock command

private:
    // ── Event subscription ───────────────────────────────────────────────────
    bedrocktools::events::Subscription m_tickSub = 0;

    // ── Per-tick watch state ─────────────────────────────────────────────────
    bool        m_watching     = false;
    IPos        m_watchPos     = {};
    std::string m_watchName;          // block name at m_watchPos last tick

    // ── Break queue ──────────────────────────────────────────────────────────
    std::vector<IPos> m_queue;
    std::size_t       m_queueIdx = 0;
    float             m_timerMs  = 0.0f;

    // ── Helpers ──────────────────────────────────────────────────────────────
    static bool        isAllowedBlock(const std::string& name);
    static std::string blockNameAt(void* blockSource, int x, int y, int z);
    static std::vector<IPos> bfsVein(void* blockSource,
                                     const IPos& origin,
                                     const std::string& targetName,
                                     int limit);

    void sendSetblock(void* clientInstance, const IPos& pos);
};
