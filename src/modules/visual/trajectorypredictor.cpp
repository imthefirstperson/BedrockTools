#include "trajectorypredictor.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using bedrocktools::sdk::Vec3;
using bedrocktools::sdk::Vec2;

struct BlockPos {
    int x, y, z;
};

using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPos*, int);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const BlockPos*);

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

constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr int kMaxPoints = 512;
constexpr float kGravity = 0.05f;
constexpr float kDrag = 0.99f;

BlockSourceGetBlockFn g_getBlock = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;

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

struct TrajPoint {
    float x, y, z;
};

std::mutex s_pointsMutex;
std::vector<TrajPoint> s_points;
TrajPoint s_landing{0.0f, 0.0f, 0.0f};
bool s_hasLanding = false;

TrajectoryPredictorModule* g_trajMod = nullptr;

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

bool positionBlocked(void* region, const Vec3& pos) {
    BlockPos bp{
        static_cast<int>(std::floor(pos.x)),
        static_cast<int>(std::floor(pos.y)),
        static_cast<int>(std::floor(pos.z)),
    };
    if (g_isSolidBlockingBlock) return g_isSolidBlockingBlock(region, &bp);
    if (g_getBlock) {
        const void* block = g_getBlock(region, &bp, 0);
        if (!block) return false;
        auto* blockObj = static_cast<const bedrocktools::sdk::Block*>(block);
        const std::string* fullName = blockObj->fullName();
        // Fallback without the signature: treat common non-air blocks as solid.
        return fullName && !fullName->empty() && *fullName != "minecraft:air";
    }
    return false;
}

// Bedrock projectile physics: gravity applied before drag, per tick.
void simulateStep(Vec3& pos, Vec3& vel, float gravity, float drag) {
    vel.y -= gravity;
    vel.x *= drag;
    vel.y *= drag;
    vel.z *= drag;
    pos.x += vel.x;
    pos.y += vel.y;
    pos.z += vel.z;
}

Vec3 launchVelocity(float pitch, float yaw, float power) {
    // Base throw speed 1.5, fully drawn bow adds up to 1.5 more.
    const float speed = 1.5f + power * 1.5f;
    const float pitchRad = pitch * kDegToRad;
    const float yawRad = yaw * kDegToRad;
    const float h = std::cos(pitchRad);
    return Vec3{
        -std::sin(yawRad) * h * speed,
        -std::sin(pitchRad) * speed,
        std::cos(yawRad) * h * speed,
    };
}

}  // namespace

void TrajectoryPredictorModule::onTick(bedrocktools::sdk::Player* player) {
    if (!enabled || !player) return;
    if (!g_isSolidBlockingBlock && !g_getBlock) return;
    if (maxSteps <= 0) return;

    auto* dimension = player->dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    if (!region) return;

    const Vec2 rot = player->rotation();
    Vec3 pos = player->position();
    // Launch from eye height.
    pos.y += 1.62f;

    const int modeSafe = std::clamp(mode, 0, 1);
    const float power = (modeSafe == 1) ? 1.0f : 0.0f;
    Vec3 vel = launchVelocity(rot.y, rot.x, power);

    // Bedrock per-tick constants: arrows fall faster than throwables;
    // horizontal/vertical drag 0.99 is shared.
    const float gravity = (modeSafe == 1) ? 0.05f : 0.03f;
    const float drag = kDrag;

    std::vector<TrajPoint> points;
    points.reserve(static_cast<size_t>(std::clamp(maxSteps, 1, kMaxPoints)));

    for (int step = 0; step < maxSteps && static_cast<int>(points.size()) < kMaxPoints; ++step) {
        simulateStep(pos, vel, gravity, drag);
        points.push_back(TrajPoint{pos.x, pos.y, pos.z});

        if (positionBlocked(region, pos)) {
            std::lock_guard<std::mutex> lock(s_pointsMutex);
            s_points = std::move(points);
            s_landing = TrajPoint{pos.x, pos.y, pos.z};
            s_hasLanding = true;
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_pointsMutex);
        s_points = std::move(points);
        s_hasLanding = false;
    }
}

namespace {

void emitSegment(void* tessellator, const TrajPoint& a, const TrajPoint& b,
                 float camX, float camY, float camZ) {
    s_tessVertex(tessellator, a.x - camX, a.y - camY, a.z - camZ);
    s_tessVertex(tessellator, b.x - camX, b.y - camY, b.z - camZ);
}

void emitCross(void* tessellator, const TrajPoint& c, float size,
               float camX, float camY, float camZ) {
    emitSegment(tessellator, {c.x - size, c.y, c.z}, {c.x + size, c.y, c.z}, camX, camY, camZ);
    emitSegment(tessellator, {c.x, c.y - size, c.z}, {c.x, c.y + size, c.z}, camX, camY, camZ);
    emitSegment(tessellator, {c.x, c.y, c.z - size}, {c.x, c.y, c.z + size}, camX, camY, camZ);
}

}  // namespace

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    TrajectoryPredictorModule* mod = g_trajMod;
    if (!mod || !mod->enabled) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || reinterpret_cast<uintptr_t>(screenContext) < 0x1000) return;

    std::vector<TrajPoint> points;
    TrajPoint landing{0.0f, 0.0f, 0.0f};
    bool hasLanding = false;
    {
        std::lock_guard<std::mutex> lock(s_pointsMutex);
        points = s_points;
        landing = s_landing;
        hasLanding = s_hasLanding;
    }
    if (points.empty()) return;

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

    auto colorParts = [](uint32_t color, float& r, float& g, float& b, float& a) {
        r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        b = static_cast<float>(color & 0xFF) / 255.0f;
        a = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
    };

    // Path polyline: (points-1) segments, each 2 vertices.
    const bool drawPath = points.size() >= 2;
    const bool drawMarker = hasLanding && mod->showLandingMarker;
    if (!drawPath && !drawMarker) return;

    int vertexCount = drawPath ? static_cast<int>(points.size() - 1) * 2 : 0;
    if (drawMarker) vertexCount += 6;

    s_tessBegin(tessellator, nullptr, 4, vertexCount, 0);

    float rC, gC, bC, aC;
    colorParts(mod->trajectoryColor, rC, gC, bC, aC);
    s_tessColor(tessellator, rC, gC, bC, aC);

    for (size_t i = 1; drawPath && i < points.size(); ++i) {
        emitSegment(tessellator, points[i - 1], points[i], camX, camY, camZ);
    }

    if (drawMarker) {
        colorParts(mod->landingColor, rC, gC, bC, aC);
        s_tessColor(tessellator, rC, gC, bC, aC);
        emitCross(tessellator, landing, 0.3f, camX, camY, camZ);
    }

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, matOutline, pad);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

TrajectoryPredictorModule::TrajectoryPredictorModule()
    : Module("Trajectory Predictor", "Predicts throwable projectile arcs from your view direction.") {
    showInMenu = true;
    g_trajMod = this;
}

TrajectoryPredictorModule::~TrajectoryPredictorModule() {
    if (g_trajMod == this) g_trajMod = nullptr;
}

void TrajectoryPredictorModule::onInit() {
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

    uintptr_t isb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(isb);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& event) {
        onTick(event.player);
    });
}

void TrajectoryPredictorModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(_renderLevel_hook),
                                 reinterpret_cast<void**>(&_renderLevel_orig));
    m_patched = true;
}

void TrajectoryPredictorModule::onEnable() {
    applyPatch();
}

void TrajectoryPredictorModule::onDisable() {
    std::lock_guard<std::mutex> lock(s_pointsMutex);
    s_points.clear();
    s_hasLanding = false;
}

void TrajectoryPredictorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    mode = j.value("mode", mode);
    maxSteps = std::clamp(j.value("maxSteps", maxSteps), 10, kMaxPoints);
    showLandingMarker = j.value("showLandingMarker", showLandingMarker);

    auto parseColor = [&](const std::string& key, uint32_t& out) {
        if (j.contains(key) && j[key].is_string()) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { out = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };
    parseColor("trajectoryColor", trajectoryColor);
    parseColor("landingColor", landingColor);
}

void TrajectoryPredictorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["mode"] = mode;
    j["maxSteps"] = maxSteps;
    j["showLandingMarker"] = showLandingMarker;

    char hexT[12], hexL[12];
    snprintf(hexT, sizeof(hexT), "#%08X", trajectoryColor);
    snprintf(hexL, sizeof(hexL), "#%08X", landingColor);
    j["trajectoryColor"] = std::string(hexT);
    j["landingColor"] = std::string(hexL);
}
