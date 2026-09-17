#pragma once

#include "../Module.hpp"
#include <cstdint>

namespace bedrocktools::sdk { class Player; }

class ChestPreviewModule : public Module {
public:
    ChestPreviewModule();
    ~ChestPreviewModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onTick(bedrocktools::sdk::Player* player);
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Rendered item panel position (ShulkerPreview uses a fixed HUD anchor too).
    float m_posX = 60.0f;
    float m_posY = 380.0f;

private:
    void installHooks();

    bool m_hooksInstalled = false;
};
