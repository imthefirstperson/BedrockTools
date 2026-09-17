#pragma once

#include "../Module.hpp"
#include <cstdint>

namespace bedrocktools::sdk { class Player; }

class OreEspModule : public Module {
public:
    OreEspModule();
    ~OreEspModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onTick(bedrocktools::sdk::Player* player);
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int scanRadiusH = 4;
    int scanRadiusV = 3;
    int scanInterval = 10;

    static constexpr int GroupCount = 10;
    bool showGroups[GroupCount] = {true, true, true, true, true, true, true, true, true, true};
    uint32_t groupColors[GroupCount] = {
        0xFF545454,  // coal
        0xFFD8AF93,  // iron
        0xFFE77C56,  // copper
        0xFFFCEE4B,  // gold
        0xFFFF3030,  // redstone
        0xFF4AEDD9,  // diamond
        0xFF17DD62,  // emerald
        0xFF2145C7,  // lapis
        0xFFECECEC,  // quartz
        0xFF8A4B60,  // ancient debris
    };

    bool groupEnabled(int group) const;
    uint32_t groupColor(int group) const;

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

    int m_tickCounter = 0;
};
