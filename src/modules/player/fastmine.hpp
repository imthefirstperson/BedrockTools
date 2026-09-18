#pragma once

#include "../Module.hpp"

class FastMineModule : public Module {
public:
    FastMineModule();

    void onInit()   override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j)       override;

    float m_boostSpeed = 0.35f;
    bool  m_instant = false;

private:
    void* m_gameMode = nullptr;
    bool  m_breaking = false;
};
