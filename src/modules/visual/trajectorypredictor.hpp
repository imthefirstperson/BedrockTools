#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <vector>

namespace bedrocktools::sdk { class Player; }

class TrajectoryPredictorModule : public Module {
public:
    TrajectoryPredictorModule();
    ~TrajectoryPredictorModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onTick(bedrocktools::sdk::Player* player);
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    uint32_t trajectoryColor = 0x80FFFFFF;  // translucent white
    uint32_t landingColor = 0xFFFF4040;     // red
    int mode = 0;        // 0 = throwable (snowball/egg), 1 = arrow
    int maxSteps = 120;  // simulation ticks
    bool showLandingMarker = true;

private:
    void applyPatch();

    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMeshAddr = nullptr;
    void* m_renderMesh2Addr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;
};
