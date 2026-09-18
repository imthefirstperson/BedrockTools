#include "fastmine.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/GameModeActionEvent.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

FastMineModule::FastMineModule()
    : Module("Fast Mine", "Mines blocks faster by accelerating break progress while you mine.") {
}

void FastMineModule::onInit() {
    // Track when the player is actively breaking a block and keep the GameMode pointer
    // (same proven source as Break Indicator, which reads mDestroyProgress the same way).
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>([this](auto& event) {
        if (event.action == bedrocktools::events::GameModeAction::StartDestroyBlock) {
            m_gameMode = event.gameMode;
            m_breaking = m_gameMode != nullptr;
        } else if (event.action == bedrocktools::events::GameModeAction::StopDestroyBlock &&
                   (!m_gameMode || m_gameMode == event.gameMode)) {
            m_gameMode = nullptr;
            m_breaking = false;
        }
    });

    // Accelerate the accumulated break progress every player tick while mining.
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto&) {
        if (!enabled) return;
        if (!m_breaking || !m_gameMode) return;

        float* progress = reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(m_gameMode) +
            bedrocktools::sdk::offsets::GameMode::mDestroyProgress);

        float value = *progress;
        if (!(value > 0.0f) || !std::isfinite(value)) return; // not actually breaking / creative / instant-break blocks

        if (m_instant) {
            *progress = 1.0f;
        } else {
            const float boost = std::clamp(m_boostSpeed, 0.01f, 0.95f);
            *progress = std::min(value + boost, 1.0f);
        }
    });
}

void FastMineModule::onEnable() {
    m_gameMode = nullptr;
    m_breaking = false;
}

void FastMineModule::onDisable() {
    m_gameMode = nullptr;
    m_breaking = false;
}

void FastMineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_boostSpeed")) m_boostSpeed = j["m_boostSpeed"].get<float>();
    if (j.contains("m_instant")) m_instant = j["m_instant"].get<bool>();
}

void FastMineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_boostSpeed"] = m_boostSpeed;
    j["m_instant"] = m_instant;
}
