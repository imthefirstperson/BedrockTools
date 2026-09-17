// Chest Preview module — shows the contents of the chest you are aiming at
// without opening it.

#include "chestpreview.hpp"
#include <pl/memory/Vtable.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace {

using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

constexpr const char* MinecraftLibrary = "libminecraftpe.so";

// Panel layout mirrors ShulkerPreview (9x3 chest grid).
constexpr int Columns = 9;
constexpr int Rows = 3;
constexpr int ChestSlotCount = Columns * Rows;
constexpr float SlotStride = 18.0f;
constexpr float SlotDrawSize = 17.5f;
constexpr float PanelPadding = 6.0f;
constexpr float ItemDrawSize = 16.0f;
constexpr float ItemInset = (SlotStride - ItemDrawSize) * 0.5f;
constexpr float CountTextHeight = 6.0f;
constexpr float PanelWidth = Columns * SlotStride + PanelPadding * 2.0f;
constexpr float PanelHeight = Rows * SlotStride + PanelPadding * 2.0f;
constexpr float MaxTargetDistance = 6.0f;
constexpr int MaxRaySteps = 24;

// --- ItemStack / container layout (see sdk/offsets/Inventory.hpp) ---
constexpr std::size_t ItemStackStorageSize = bedrocktools::sdk::offsets::Inventory::ItemStackSize; // 0x98
constexpr std::size_t ItemStackItemCounterOffset = bedrocktools::sdk::offsets::Inventory::ItemStackItemCounter; // 0x8
constexpr std::size_t ItemStackCountOffset = bedrocktools::sdk::offsets::Inventory::ItemStackCount;             // 0x22
constexpr std::size_t ItemStackValidOffset = bedrocktools::sdk::offsets::Inventory::ItemStackValid;             // 0x23
constexpr std::size_t FillingContainerItemsOffset = bedrocktools::sdk::offsets::Inventory::FillingContainerItems; // 0x140

struct ItemStackStorage {
    alignas(16) std::byte data[ItemStackStorageSize];
};

struct Item {};
struct Font {};

#pragma pack(push, 4)
struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct UiVec2 {
    float x;
    float y;
};

struct TextMeasureData {
    float fontSize;
    float linePadding;
    bool renderShadow;
    bool showColorSymbol;
    bool hideHyphen;
};

struct CaretMeasureData {
    int position;
    bool shouldRender;
};
#pragma pack(pop)

namespace mce {
struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct ClientTexture {
    std::byte storage[24]{};
};
}  // namespace mce

struct BedrockTextureData {
    mce::ClientTexture clientTexture;
};

enum class ResourceFileSystem : int {
    UserPackage = 0
};

class ResourceLocation {
public:
    ResourceFileSystem fileSystem;
    std::string path;
    std::uint64_t pathHash;
    std::uint64_t fullHash;

    explicit ResourceLocation(const char* value)
        : fileSystem(ResourceFileSystem::UserPackage),
          path(value ? value : ""),
          pathHash(computeHash(path)),
          fullHash(pathHash ^ static_cast<std::uint64_t>(fileSystem)) {}

private:
    static std::uint64_t computeHash(std::string_view value) {
        constexpr std::uint64_t Offset = 1469598103934665603ULL;
        constexpr std::uint64_t Prime = 1099511628211ULL;
        std::uint64_t hash = Offset;
        for (unsigned char ch : value) hash = static_cast<std::uint64_t>(ch) ^ (Prime * hash);
        return hash;
    }
};

namespace mce {
class TexturePtr {
public:
    std::shared_ptr<const BedrockTextureData> clientTexture;
    std::shared_ptr<ResourceLocation> resourceLocation;

    const ClientTexture& getClientTexture() const {
        static const ClientTexture empty{};
        return clientTexture ? clientTexture->clientTexture : empty;
    }
};
}  // namespace mce

class HashedString {
public:
    std::uint64_t hash;
    std::string value;
    mutable const HashedString* lastMatch;

    explicit HashedString(const char* text)
        : hash(computeHash(text ? std::string_view(text) : std::string_view())),
          value(text ? text : ""),
          lastMatch(nullptr) {}

private:
    static std::uint64_t computeHash(std::string_view text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t Offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t Prime = 0x100000001B3ULL;
        std::uint64_t result = Offset;
        for (char ch : text) result = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (Prime * result);
        return result;
    }
};

enum class TextAlignment : std::uint8_t {
    Left,
    Right,
    Center
};

// --- Block actor item access ----------------------------------------------

// ChestBlockActor (a FillingContainer subclass) stores items in a
// std::vector<ItemStack> at FillingContainerItems (0x140). Layout:
//   begin / end / capacity pointers, ItemStack entries of size 0x98.
// We validate every read so a wrong offset degrades to "no preview"
// instead of crashing the game.

struct BlockPos {
    int x, y, z;
};

using BlockSourceGetBlockEntityFn = void* (*)(void*, const BlockPos*);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const BlockPos*);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(void*, void*, void*, unsigned int, unsigned char, std::uint64_t, float, float, float, float, float);
using DrawTextFn = void (*)(void*, Font&, const RectangleArea&, const std::string&, const mce::Color&, TextAlignment, float, const TextMeasureData&, const CaretMeasureData&);

BlockSourceGetBlockEntityFn s_getBlockEntity = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;

bool isValidHeapPointer(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer) > 0x10000;
}

const std::byte* chestItemsBegin(const void* blockActor, int& slotCountOut) {
    slotCountOut = 0;
    if (!blockActor || !isValidHeapPointer(blockActor)) return nullptr;

    const auto* base = reinterpret_cast<const std::byte*>(blockActor);
    const auto* begin = *reinterpret_cast<const std::byte* const*>(base + FillingContainerItemsOffset);
    const auto* end = *reinterpret_cast<const std::byte* const*>(base + FillingContainerItemsOffset + sizeof(void*));
    const auto* capacity = *reinterpret_cast<const std::byte* const*>(base + FillingContainerItemsOffset + sizeof(void*) * 2);

    if (!isValidHeapPointer(begin) || !isValidHeapPointer(end) || !isValidHeapPointer(capacity)) return nullptr;
    if (begin >= end || end > capacity) return nullptr;

    const auto total = static_cast<std::size_t>(end - begin);
    const auto slotSize = static_cast<std::size_t>(ItemStackStorageSize);
    if (total % slotSize != 0) return nullptr;  // not an ItemStack vector
    const auto count = total / slotSize;
    if (count < 1 || count > 54) return nullptr;  // chest-like sizes only

    slotCountOut = static_cast<int>(count);
    return begin;
}

const ItemStackStorage* chestSlot(const void* blockActor, int slot) {
    int count = 0;
    const std::byte* items = chestItemsBegin(blockActor, count);
    if (!items || slot < 0 || slot >= count) return nullptr;
    return reinterpret_cast<const ItemStackStorage*>(items + static_cast<std::size_t>(slot) * ItemStackStorageSize);
}

// --- UI plumbing (same technique as ShulkerPreview) ------------------------

void** getVtable(void* instance) {
    return instance ? *reinterpret_cast<void***>(instance) : nullptr;
}

float uiGetLineLength(void* context, Font& font, const std::string& text, float size, bool unknown) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetLineLength]) return 0.0f;
    using Fn = float (*)(void*, Font&, const std::string&, float, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetLineLength])(context, font, text, size, unknown);
}

void uiDrawText(void* context, Font& font, const RectangleArea& rectangle, const std::string& text, const mce::Color& color, TextAlignment alignment, float alpha, const TextMeasureData& measure, const CaretMeasureData& caret) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText]) return;
    reinterpret_cast<DrawTextFn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText])(context, font, rectangle, text, color, alignment, alpha, measure, caret);
}

void uiFlushText(void* context, float value, std::optional<float> optionalValue) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushText]) return;
    using Fn = void (*)(void*, float, std::optional<float>);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushText])(context, value, optionalValue);
}

void uiDrawImage(void* context, const mce::ClientTexture& texture, const UiVec2& position, const UiVec2& size, const UiVec2& uv, const UiVec2& uvSize, bool tiled) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage]) return;
    using Fn = void (*)(void*, const mce::ClientTexture&, const UiVec2&, const UiVec2&, const UiVec2&, const UiVec2&, bool);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage])(context, texture, position, size, uv, uvSize, tiled);
}

void uiFlushImages(void* context, const mce::Color& color, float alpha, const HashedString& material) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const mce::Color&, float, const HashedString&);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, alpha, material);
}

void uiFillRectangle(void* context, const RectangleArea& rectangle, const mce::Color& color, float alpha) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle]) return;
    using Fn = void (*)(void*, const RectangleArea&, const mce::Color&, float);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle])(context, rectangle, color, alpha);
}

mce::TexturePtr uiGetTexture(void* context, const ResourceLocation& location, bool forceReload) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture]) return {};
    using Fn = mce::TexturePtr (*)(void*, const ResourceLocation&, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture])(context, location, forceReload);
}

bool hasTexture(const mce::TexturePtr& texture) {
    return static_cast<bool>(texture.clientTexture);
}

struct CachedUiTextures {
    bool loaded = false;
    mce::TexturePtr panel;
    mce::TexturePtr slot;
};

CachedUiTextures& getTextures(void* context) {
    static CachedUiTextures textures;
    if (!textures.loaded) {
        textures.panel = uiGetTexture(context, ResourceLocation("textures/ui/dialog_background_opaque"), false);
        textures.slot = uiGetTexture(context, ResourceLocation("textures/ui/item_cell"), false);
        textures.loaded = true;
    }
    return textures;
}

void drawNineSlice(void* context, const mce::ClientTexture& texture, const RectangleArea& rectangle) {
    constexpr float textureSize = 16.0f;
    constexpr float slice = 4.0f;
    float width = std::max(0.0f, rectangle.x1 - rectangle.x0);
    float height = std::max(0.0f, rectangle.y1 - rectangle.y0);
    float middleWidth = std::max(0.0f, width - slice * 2.0f);
    float middleHeight = std::max(0.0f, height - slice * 2.0f);
    float textureMiddle = textureSize - slice * 2.0f;
    UiVec2 positions[9] = {
        {rectangle.x0, rectangle.y0},
        {rectangle.x0 + slice, rectangle.y0},
        {rectangle.x1 - slice, rectangle.y0},
        {rectangle.x0, rectangle.y0 + slice},
        {rectangle.x0 + slice, rectangle.y0 + slice},
        {rectangle.x1 - slice, rectangle.y0 + slice},
        {rectangle.x0, rectangle.y1 - slice},
        {rectangle.x0 + slice, rectangle.y1 - slice},
        {rectangle.x1 - slice, rectangle.y1 - slice}
    };
    UiVec2 sizes[9] = {
        {slice, slice}, {middleWidth, slice}, {slice, slice},
        {slice, middleHeight}, {middleWidth, middleHeight}, {slice, middleHeight},
        {slice, slice}, {middleWidth, slice}, {slice, slice}
    };
    UiVec2 uvPositions[9] = {
        {0.0f, 0.0f},
        {slice / textureSize, 0.0f},
        {(textureSize - slice) / textureSize, 0.0f},
        {0.0f, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {(textureSize - slice) / textureSize, slice / textureSize},
        {0.0f, (textureSize - slice) / textureSize},
        {slice / textureSize, (textureSize - slice) / textureSize},
        {(textureSize - slice) / textureSize, (textureSize - slice) / textureSize}
    };
    UiVec2 uvSizes[9] = {
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {textureMiddle / textureSize, textureMiddle / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize}
    };
    for (int index = 0; index < 9; ++index) {
        if (sizes[index].x <= 0.0f || sizes[index].y <= 0.0f) continue;
        uiDrawImage(context, texture, positions[index], sizes[index], uvPositions[index], uvSizes[index], false);
    }
}

// --- Targeting ------------------------------------------------------------

// Updated by tick; guarded by mutex for the render thread.
std::mutex s_previewMutex;
void* s_targetBlockActor = nullptr;
bool s_targetValid = false;

ChestPreviewModule* g_module = nullptr;

bool isContainerBlockName(const std::string& name) {
    return name == "minecraft:chest"
        || name == "minecraft:trapped_chest"
        || name == "minecraft:barrel";
}

void* resolveLookTarget(bedrocktools::sdk::Player* player, bedrocktools::sdk::BlockSource* region, BlockPos& outPos) {
    if (!player || !region) return nullptr;

    const Vec2 rot = player->rotation();
    Vec3 origin = player->position();
    origin.y += 1.62f;

    const float pitchRad = rot.x * (3.14159265f / 180.0f);
    const float yawRad = rot.y * (3.14159265f / 180.0f);
    const float dirX = -std::sin(yawRad) * std::cos(pitchRad);
    const float dirY = -std::sin(pitchRad);
    const float dirZ = std::cos(yawRad) * std::cos(pitchRad);

    BlockPos lastPos{0, 0, 0};

    for (int step = 1; step <= MaxRaySteps; ++step) {
        const float distance = step * 0.25f;  // 0.25-block steps up to 6 blocks
        if (distance > MaxTargetDistance) break;

        Vec3 point{
            origin.x + dirX * distance,
            origin.y + dirY * distance,
            origin.z + dirZ * distance,
        };

        BlockPos pos{
            static_cast<int>(std::floor(point.x)),
            static_cast<int>(std::floor(point.y)),
            static_cast<int>(std::floor(point.z)),
        };

        if (pos.x == lastPos.x && pos.y == lastPos.y && pos.z == lastPos.z) continue;

        using BlockSourceGetBlockFn = const void* (*)(void*, const BlockPos*, int);
        static BlockSourceGetBlockFn getBlock = reinterpret_cast<BlockSourceGetBlockFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock));
        if (!getBlock) return nullptr;

        static BlockSourceIsSolidBlockingBlockFn isSolid = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));

        const void* block = getBlock(region, &pos, 0);
        if (!block) continue;
        lastPos = pos;

        const auto* blockObj = static_cast<const bedrocktools::sdk::Block*>(block);
        const std::string* fullName = blockObj->fullName();
        if (!fullName || fullName->empty()) continue;

        if (isContainerBlockName(*fullName)) {
            outPos = pos;
            return s_getBlockEntity ? s_getBlockEntity(region, &pos) : nullptr;
        }

        // Ray stops at the first solid non-container block.
        if (isSolid && isSolid(region, &pos)) return nullptr;
    }
    return nullptr;
}

void refreshTarget(bedrocktools::sdk::Player* player) {
    auto* dimension = player->dimension();
    auto* region = dimension ? dimension->blockSource() : nullptr;
    if (!region) return;

    BlockPos pos{0, 0, 0};
    void* blockActor = resolveLookTarget(player, region, pos);

    std::lock_guard<std::mutex> lock(s_previewMutex);
    s_targetBlockActor = blockActor;
    s_targetValid = blockActor != nullptr;
}

// --- Render hook -----------------------------------------------------------

void* activeUiContext = nullptr;
Font* activeFont = nullptr;

void (*screenViewRenderOriginal)(void*, void*, void*, void*, void*, void*, void*, void*) = nullptr;
void (*drawTextOriginal)(void*, Font&, const RectangleArea&, const std::string&, const mce::Color&, TextAlignment, float, const TextMeasureData&, const CaretMeasureData&) = nullptr;

void* getMinecraftGame(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (vtable && vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame]) {
        void* game = reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame])(client);
        if (game) return game;
    }
    return nullptr;
}

void* getClientLocalPlayer(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

Item* getStackItem(const ItemStackStorage& storage) {
    void* counter = *reinterpret_cast<void* const*>(storage.data + ItemStackItemCounterOffset);
    if (!counter) return nullptr;
    return *reinterpret_cast<Item**>(reinterpret_cast<std::byte*>(counter));
}

std::uint8_t getStackCount(const ItemStackStorage& storage) {
    return *reinterpret_cast<const std::uint8_t*>(storage.data + ItemStackCountOffset);
}

bool isStackValid(const ItemStackStorage& storage) {
    return *reinterpret_cast<const std::uint8_t*>(storage.data + ItemStackValidOffset) != 0;
}

unsigned int getItemAnimationFrame(Item* item, void* localPlayer, const ItemStackStorage& storage) {
    if (!item || !localPlayer) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(Item*, void*, int, void*, int);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, const_cast<ItemStackStorage*>(&storage), 1);
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

void drawIcons(void* context, void* blockActor, float originX, float originY) {
    if (!baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) return;
    void* client = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextClient);
    void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    void* game = getMinecraftGame(client);
    void* localPlayer = getClientLocalPlayer(client);
    if (!client || !screenContext || !game) return;

    alignas(16) std::byte baseActorRenderContext[bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    baseActorRenderContextCtor(baseActorRenderContext, screenContext, client, game);
    void* itemRenderer = *reinterpret_cast<void**>(baseActorRenderContext + bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
    if (!itemRenderer) {
        destroyBaseActorRenderContext(baseActorRenderContext);
        return;
    }

    static const HashedString flushMaterial("ui_flush");
    for (int row = 0; row < Rows; ++row) {
        for (int column = 0; column < Columns; ++column) {
            const int slot = row * Columns + column;
            const ItemStackStorage* stack = chestSlot(blockActor, slot);
            if (!stack || !isStackValid(*stack)) continue;
            const float x = originX + column * SlotStride;
            const float y = originY + row * SlotStride;
            unsigned int aux = getItemAnimationFrame(getStackItem(*stack), localPlayer, *stack);
            itemRendererRenderGuiItemNew(itemRenderer, baseActorRenderContext, const_cast<ItemStackStorage*>(stack), aux, 0, 0, x + ItemInset, y + ItemInset, 1.0f, 1.0f, 1.0f);
        }
    }
    destroyBaseActorRenderContext(baseActorRenderContext);
    uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, flushMaterial);
}

void renderPreview(void* context, float x, float y, void* blockActor) {
    if (!context || !blockActor) return;
    CachedUiTextures& textures = getTextures(context);
    static const HashedString material("ui_flush");

    const RectangleArea panel{x, x + PanelWidth, y, y + PanelHeight};
    if (hasTexture(textures.panel)) drawNineSlice(context, textures.panel.getClientTexture(), panel);
    uiFlushImages(context, {0.55f, 0.42f, 0.18f, 1.0f}, 1.0f, material);  // warm chest tint

    const float originX = x + PanelPadding;
    const float originY = y + PanelPadding;
    if (hasTexture(textures.slot)) {
        for (int row = 0; row < Rows; ++row) {
            for (int column = 0; column < Columns; ++column) {
                const float slotX = originX + column * SlotStride;
                const float slotY = originY + row * SlotStride;
                uiDrawImage(context, textures.slot.getClientTexture(), {slotX, slotY}, {SlotDrawSize, SlotDrawSize}, {0.0f, 0.0f}, {1.0f, 1.0f}, false);
            }
        }
    }
    uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, material);

    drawIcons(context, blockActor, originX, originY);

    if (activeFont) {
        TextMeasureData measure{};
        measure.fontSize = 1.0f;
        CaretMeasureData caret{};
        for (int row = 0; row < Rows; ++row) {
            for (int column = 0; column < Columns; ++column) {
                const int slot = row * Columns + column;
                const ItemStackStorage* stack = chestSlot(blockActor, slot);
                if (!stack || !isStackValid(*stack)) continue;
                const std::uint8_t count = getStackCount(*stack);
                if (count <= 1) continue;
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "%u", count);
                std::string text(buffer);
                const float slotX = originX + column * SlotStride;
                const float slotY = originY + row * SlotStride;
                const float width = uiGetLineLength(context, *activeFont, text, 1.0f, false);
                const float right = slotX + SlotDrawSize - 0.5f;
                const float bottom = slotY + SlotDrawSize - 1.5f;
                uiDrawText(context, *activeFont, {right - width, right, bottom - CountTextHeight, bottom}, text, {1.0f, 1.0f, 1.0f, 1.0f}, TextAlignment::Right, 1.0f, measure, caret);
            }
        }
    }
    uiFlushText(context, 0.0f, std::nullopt);
}

void screenViewRenderHook(void* self, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    activeUiContext = nullptr;
    if (screenViewRenderOriginal) screenViewRenderOriginal(self, a2, a3, a4, a5, a6, a7, a8);
    if (!g_module || !g_module->enabled || !activeUiContext) return;

    void* blockActor = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_previewMutex);
        blockActor = s_targetValid ? s_targetBlockActor : nullptr;
    }
    if (!blockActor) return;

    renderPreview(activeUiContext, g_module->m_posX, g_module->m_posY, blockActor);
}

void drawTextHook(void* self, Font& font, const RectangleArea& rectangle, const std::string& text, const mce::Color& color, TextAlignment alignment, float alpha, const TextMeasureData& measure, const CaretMeasureData& caret) {
    activeUiContext = self;
    activeFont = &font;
    if (drawTextOriginal) drawTextOriginal(self, font, rectangle, text, color, alignment, alpha, measure, caret);
}

void hookVtable(const char* className, void** original, void* replacement, std::size_t slot) {
    const auto target = pl::memory::resolveVtableFunction(className, slot, MinecraftLibrary);
    if (!target) return;
    bedrocktools::hooks::install(reinterpret_cast<void*>(target), replacement, original);
}

}  // namespace

ChestPreviewModule::ChestPreviewModule()
    : Module("Chest Preview", "Shows the contents of the chest you are aiming at without opening it.") {
    g_module = this;
}

ChestPreviewModule::~ChestPreviewModule() {
    if (g_module == this) g_module = nullptr;
}

void ChestPreviewModule::onInit() {
    if (m_hooksInstalled) return;

    s_getBlockEntity = reinterpret_cast<BlockSourceGetBlockEntityFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlockEntity));
    baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
    itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));

    const uintptr_t screenRenderAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ScreenViewRender);
    if (screenRenderAddress) {
        bedrocktools::hooks::install(reinterpret_cast<void*>(screenRenderAddress),
                                     reinterpret_cast<void*>(screenViewRenderHook),
                                     reinterpret_cast<void**>(&screenViewRenderOriginal));
    }
    hookVtable("24MinecraftUIRenderContext", reinterpret_cast<void**>(&drawTextOriginal),
               reinterpret_cast<void*>(drawTextHook), bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& event) {
        onTick(event.player);
    });

    m_hooksInstalled = true;
}

void ChestPreviewModule::onEnable() {}

void ChestPreviewModule::onDisable() {
    std::lock_guard<std::mutex> lock(s_previewMutex);
    s_targetBlockActor = nullptr;
    s_targetValid = false;
}

void ChestPreviewModule::onTick(bedrocktools::sdk::Player* player) {
    if (!enabled || !player) return;
    if (!s_getBlockEntity) return;  // cannot resolve chest contents safely
    refreshTarget(player);
}

void ChestPreviewModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_posX")) m_posX = std::clamp(j["m_posX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_posY")) m_posY = std::clamp(j["m_posY"].get<float>(), 0.0f, 4000.0f);
}

void ChestPreviewModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_posX"] = m_posX;
    j["m_posY"] = m_posY;
}
