#include "oreesp.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using bedrocktools::sdk::Vec3;

struct BlockPos {
    int x, y, z;
};

using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPos*, int);

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}
    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }
private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

struct OreResult {
    int32_t x, y, z;
    int8_t group;
};

constexpr int kMaxResults = 4096;
constexpr float kBoxExpand = 0.06f;
constexpr int kGroupCoal = 0;
constexpr int kGroupIron = 1;
constexpr int kGroupCopper = 2;
constexpr int kGroupGold = 3;
constexpr int kGroupRedstone = 4;
constexpr int kGroupDiamond = 5;
constexpr int kGroupEmerald = 6;
constexpr int kGroupLapis = 7;
constexpr int kGroupQuartz = 8;
constexpr int kGroupDebris = 9;

BlockSourceGetBlockFn g_getBlock = nullptr;

Tessellator_begin_t s_tessBegin = nullptr;
Tessellator_color_t s_tessColor = nullptr;
Tessellator_vertex_t s_tessVertex = nullptr;
MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

MaterialPtr s_matSelection;
uintptr_t s_renderMaterialGroup = 0;

uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

std::mutex s_resultsMutex;
std::vector<OreResult> s_results;

OreEspModule* g_oreEsp = nullptr;

// Menu-facing names for each ore group; also used as config key fragments
// ("show<Name>" toggle / "<Name>Color" color picker).
constexpr const char* kGroupNames[OreEspModule::GroupCount] = {
    "Coal", "Iron", "Copper", "Gold", "Redstone", "Diamond", "Emerald", "Lapis", "Quartz", "Netherite",
};

struct OreGroupDef {
    const char* fragment;
    int8_t group;
    int8_t dimension;  // -1 = all dimensions
};

constexpr OreGroupDef kGroupDefs[OreEspModule::GroupCount] = {
    {"coal",     kGroupCoal,     -1},
    {"iron",     kGroupIron,     -1},
    {"copper",   kGroupCopper,   -1},
    {"gold",     kGroupGold,     -1},
    {"redstone", kGroupRedstone,  0},
    {"diamond",  kGroupDiamond,  -1},
    {"emerald",  kGroupEmerald,   0},
    {"lapis",    kGroupLapis,     0},
    {"quartz",   kGroupQuartz,    1},
    {"debris",   kGroupDebris,    1},
};

bool classifyOre(std::string_view name, int dimId, int8_t& outGroup) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());

    // Only real ore blocks: names ending in "_ore", plus ancient debris.
    if (!name.ends_with("_ore") && name != "ancient_debris") return false;

    for (const auto& def : kGroupDefs) {
        if (def.dimension >= 0 && def.dimension != dimId) continue;
        if (name.find(def.fragment) != std::string_view::npos) {
            outGroup = def.group;
            return true;
        }
    }
    return false;
}

MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return {};
    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])(
        (void*)s_renderMaterialGroup, &hs);
}

void ensureMaterials() {
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;
    s_matSelection = getMaterial("selection_box");
}

}  // namespace

void OreEspModule::onTick(bedrocktools::sdk::Player* player) {
    if (!enabled || !g_getBlock || !player) return;

    ++m_tickCounter;
    if (m_tickCounter < scanInterval) return;
    m_tickCounter = 0;

    auto* actor = reinterpret_cast<bedrocktools::sdk::Actor*>(player);
    const Vec3 playerPos = actor->position();
    if (playerPos.x == 0.0f && playerPos.y == 0.0f && playerPos.z == 0.0f) return;

    auto* dimension = actor->dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    if (!region) return;

    const int dimId = region->dimensionId();

    const int rh = std::clamp(scanRadiusH, 1, 32);
    const int rv = std::clamp(scanRadiusV, 1, 32);

    const int px = static_cast<int>(std::floor(playerPos.x));
    const int py = static_cast<int>(std::floor(playerPos.y));
    const int pz = static_cast<int>(std::floor(playerPos.z));

    std::vector<OreResult> results;
    results.reserve(256);

    const int yMin = std::max(py - rv, -64);
    const int yMax = std::min(py + rv, 319);
    bool overflow = false;

    for (int x = px - rh; x <= px + rh && !overflow; ++x) {
        for (int y = yMin; y <= yMax && !overflow; ++y) {
            for (int z = pz - rh; z <= pz + rh && !overflow; ++z) {
                BlockPos bp{x, y, z};
                const void* block = g_getBlock(region, &bp, 0);
                if (!block) continue;

                const auto* blockObj = static_cast<const bedrocktools::sdk::Block*>(block);
                const std::string* fullName = blockObj->fullName();
                if (!fullName || fullName->empty() || fullName->size() > 256) continue;

                int8_t group = -1;
                if (!classifyOre(*fullName, dimId, group)) continue;
                if (!groupEnabled(group)) continue;

                results.push_back(OreResult{x, y, z, group});
                if (results.size() >= kMaxResults) overflow = true;
            }
        }
    }

    std::lock_guard<std::mutex> lock(s_resultsMutex);
    s_results = std::move(results);
}

namespace {

void emitLine(void* tessellator, float ax, float ay, float az, float bx, float by, float bz) {
    s_tessVertex(tessellator, ax, ay, az);
    s_tessVertex(tessellator, bx, by, bz);
}

void drawBox(void* tessellator, float x1, float y1, float z1, float x2, float y2, float z2) {
    // bottom face
    emitLine(tessellator, x1, y1, z1, x2, y1, z1);
    emitLine(tessellator, x2, y1, z1, x2, y1, z2);
    emitLine(tessellator, x2, y1, z2, x1, y1, z2);
    emitLine(tessellator, x1, y1, z2, x1, y1, z1);
    // top face
    emitLine(tessellator, x1, y2, z1, x2, y2, z1);
    emitLine(tessellator, x2, y2, z1, x2, y2, z2);
    emitLine(tessellator, x2, y2, z2, x1, y2, z2);
    emitLine(tessellator, x1, y2, z2, x1, y2, z1);
    // vertical edges
    emitLine(tessellator, x1, y1, z1, x1, y2, z1);
    emitLine(tessellator, x2, y1, z1, x2, y2, z1);
    emitLine(tessellator, x2, y1, z2, x2, y2, z2);
    emitLine(tessellator, x1, y1, z2, x1, y2, z2);
}

}  // namespace

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    OreEspModule* mod = g_oreEsp;
    if (!mod || !mod->enabled) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000) return;

    // Copy only the results that are currently enabled so the tessellator
    // vertex count always matches what we actually emit.
    std::vector<OreResult> results;
    {
        std::lock_guard<std::mutex> lock(s_resultsMutex);
        results.reserve(s_results.size());
        for (const OreResult& r : s_results) {
            if (mod->groupEnabled(r.group)) results.push_back(r);
        }
    }
    if (results.empty()) return;

    const uintptr_t tessellatorPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;
    void* tessellator = reinterpret_cast<void*>(tessellatorPtr);

    const uintptr_t lrpPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(_this) + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    const float camX = *reinterpret_cast<const float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *reinterpret_cast<const float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *reinterpret_cast<const float*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matOutline = s_matSelection
        ? reinterpret_cast<void*>(&s_matSelection)
        : reinterpret_cast<void*>(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    const uintptr_t colorHolderPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(screenContext) + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);

    const float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    s_tessBegin(tessellator, nullptr, 4, static_cast<int>(results.size()) * 24, 0);

    for (const OreResult& r : results) {
        const uint32_t color = mod->groupColor(r.group);
        const float rC = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        const float gC = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        const float bC = static_cast<float>(color & 0xFF) / 255.0f;
        const float aC = static_cast<float>((color >> 24) & 0xFF) / 255.0f;

        s_tessColor(tessellator, rC, gC, bC, aC);

        const float bx = static_cast<float>(r.x) - kBoxExpand;
        const float by = static_cast<float>(r.y) - kBoxExpand;
        const float bz = static_cast<float>(r.z) - kBoxExpand;
        const float ex = 1.0f + kBoxExpand * 2.0f;

        drawBox(tessellator,
                bx - camX, by - camY, bz - camZ,
                bx + ex - camX, by + ex - camY, bz + ex - camZ);
    }

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, matOutline, pad);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

OreEspModule::OreEspModule()
    : Module("Ore ESP", "Highlights nearby ores with colored boxes through walls.") {
    showInMenu = true;
    g_oreEsp = this;
}

OreEspModule::~OreEspModule() {
    if (g_oreEsp == this) g_oreEsp = nullptr;
}

void OreEspModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = reinterpret_cast<void*>(addr);
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = reinterpret_cast<void*>(tb); s_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb); }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = reinterpret_cast<void*>(tc); s_tessColor = reinterpret_cast<Tessellator_color_t>(tc); }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = reinterpret_cast<void*>(tv); s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv); }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        m_renderMesh2Addr = reinterpret_cast<void*>(rm);
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm5);
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = reinterpret_cast<void*>(rmg);
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    uintptr_t gb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (gb) g_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(gb);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& event) {
        onTick(event.player);
    });
}

void OreEspModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(_renderLevel_hook),
                                 reinterpret_cast<void**>(&_renderLevel_orig));
    m_patched = true;
}

void OreEspModule::onEnable() {
    applyPatch();
    m_tickCounter = scanInterval;  // scan immediately on the next tick
}

void OreEspModule::onDisable() {
    {
        std::lock_guard<std::mutex> lock(s_resultsMutex);
        s_results.clear();
    }
    m_tickCounter = 0;
}

bool OreEspModule::groupEnabled(int group) const {
    if (group < 0 || group >= GroupCount) return false;
    return showGroups[group];
}

uint32_t OreEspModule::groupColor(int group) const {
    if (group < 0 || group >= GroupCount) return 0xFFFFFFFF;
    return groupColors[group];
}

void OreEspModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    scanRadiusH = j.value("scanRadiusH", scanRadiusH);
    scanRadiusV = j.value("scanRadiusV", scanRadiusV);
    scanInterval = j.value("scanInterval", scanInterval);

    for (int i = 0; i < GroupCount; ++i) {
        const std::string showKey = "show" + std::string(kGroupNames[i]);
        showGroups[i] = j.value(showKey, showGroups[i]);

        const std::string colorKey = std::string(kGroupNames[i]) + "Color";
        if (j.contains(colorKey) && j[colorKey].is_string()) {
            std::string hexStr = j[colorKey].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { groupColors[i] = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    }
}

void OreEspModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["scanRadiusH"] = scanRadiusH;
    j["scanRadiusV"] = scanRadiusV;
    j["scanInterval"] = scanInterval;

    for (int i = 0; i < GroupCount; ++i) {
        j["show" + std::string(kGroupNames[i])] = showGroups[i];

        char hex[12];
        snprintf(hex, sizeof(hex), "#%08X", groupColors[i]);
        j[std::string(kGroupNames[i]) + "Color"] = std::string(hex);
    }
}
