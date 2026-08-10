#include "advancedtooltips.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using AppendFormattedHoverTextFn = void (*)(void*, void*, void*, std::string&, bool);
AppendFormattedHoverTextFn g_originalAppend = nullptr;
bedrocktools::hooks::Handle g_hook = nullptr;
AdvancedItemTooltipsModule* g_module = nullptr;

struct MemoryRange {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    bool executable = false;
};

struct NbtTreeKey {
    const char* data = nullptr;
    std::size_t len = 0;
};

struct ListTagLayout {
    void* vtable = nullptr;
    void* begin = nullptr;
    void* end = nullptr;
    void* cap = nullptr;
    std::uint8_t type = 0;
};

class IFoodItemComponent {
public:
    virtual ~IFoodItemComponent() = default;
    virtual int getNutrition() const = 0;
    virtual float getSaturationModifier() const = 0;
};

template <class T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

void replaceAll(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text.find(needle.data(), pos, needle.size())) != std::string::npos) {
        text.replace(pos, needle.size(), replacement.data(), replacement.size());
        pos += replacement.size();
    }
}

bool parseHexColor(std::string_view value, std::uint32_t& out) {
    std::string copy(value);
    if (copy.empty()) return false;
    if (copy.front() == '#') copy.erase(copy.begin());
    if (copy.size() == 6) copy.insert(copy.begin(), 'F'), copy.insert(copy.begin(), 'F');
    if (copy.size() != 8) return false;
    char* end = nullptr;
    const auto parsed = std::strtoul(copy.c_str(), &end, 16);
    if (!end || *end != '\0') return false;
    out = static_cast<std::uint32_t>(parsed);
    return true;
}

std::uint32_t lerpColor(std::uint32_t a, std::uint32_t b, double t) {
    t = clampValue(t, 0.0, 1.0);
    const auto ar = static_cast<int>((a >> 16) & 0xFF);
    const auto ag = static_cast<int>((a >> 8) & 0xFF);
    const auto ab = static_cast<int>(a & 0xFF);
    const auto aa = static_cast<int>((a >> 24) & 0xFF);

    const auto br = static_cast<int>((b >> 16) & 0xFF);
    const auto bg = static_cast<int>((b >> 8) & 0xFF);
    const auto bb = static_cast<int>(b & 0xFF);
    const auto ba = static_cast<int>((b >> 24) & 0xFF);

    const auto ir = static_cast<std::uint8_t>(std::lround(ar + (br - ar) * t));
    const auto ig = static_cast<std::uint8_t>(std::lround(ag + (bg - ag) * t));
    const auto ib = static_cast<std::uint8_t>(std::lround(ab + (bb - ab) * t));
    const auto ia = static_cast<std::uint8_t>(std::lround(aa + (ba - aa) * t));

    return (static_cast<std::uint32_t>(ia) << 24) |
           (static_cast<std::uint32_t>(ir) << 16) |
           (static_cast<std::uint32_t>(ig) << 8) |
           static_cast<std::uint32_t>(ib);
}

std::string colorTag(std::uint32_t argb) {
    char hex[9]{};
    std::snprintf(hex, sizeof(hex), "%08X", argb);
    std::string out;
    out.reserve(14);
    out += "\xC2\xA7";
    out += 'x';
    for (char c : std::string_view(hex)) {
        out += "\xC2\xA7";
        out += c;
    }
    return out;
}

std::string resetTag() {
    return "\xC2\xA7r";
}

void replaceStringToken(std::string& text, std::string_view token, const std::string& value) {
    replaceAll(text, token, value);
}

void replaceIntToken(std::string& text, std::string_view token, std::int64_t value) {
    replaceAll(text, token, std::to_string(value));
}

void replaceDoubleToken(std::string& text, std::string_view token, double value, int precision = 0) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), precision > 0 ? "%.*f" : "%.0f", precision > 0 ? precision : 0, value);
    replaceAll(text, token, buffer);
}

std::string readHashedString(void* item, std::size_t offset) {
    if (!item) return {};
    const auto address = reinterpret_cast<std::uintptr_t>(item) + offset + bedrocktools::sdk::offsets::HashedString::mString;
    const auto* str = reinterpret_cast<const std::string*>(address);
    if (!str) return {};
    if (str->size() > 256) return {};
    return *str;
}

void* readStackItem(void* stack) {
    if (!stack) return nullptr;
    const auto sharedCounter = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!sharedCounter) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(sharedCounter) + bedrocktools::sdk::offsets::ShulkerPreview::SharedCounterPointer);
}

void* readStackUserData(void* stack) {
    if (!stack) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseUserData);
}

std::string readItemNamespace(void* item) {
    return readHashedString(item, bedrocktools::sdk::offsets::Item::mNamespace);
}

std::string readRawNameId(void* item) {
    return readHashedString(item, bedrocktools::sdk::offsets::Item::mRawNameId);
}

std::int64_t readItemId(void* item) {
    if (!item) return -1;
    return *reinterpret_cast<const std::int16_t*>(reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::Item::mId);
}

short getItemMaxDamage(void* item) {
    if (!item) return 0;
    return bedrocktools::sdk::virtualCall<short>(item, bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage);
}

using ItemStackBaseGetDamageValueFn = short (*)(void*);
ItemStackBaseGetDamageValueFn getDamageValue = nullptr;

short readDamageValue(void* stack) {
    if (!getDamageValue) {
        getDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue)
        );
    }
    return (getDamageValue && stack) ? getDamageValue(stack) : 0;
}

using NbtTreeFindFn = void* (*)(void*, const NbtTreeKey*);

NbtTreeFindFn nbtTreeFind = nullptr;

void* treeFindNode(void* compound, const char* key, std::size_t length) {
    if (!compound) return nullptr;
    if (!nbtTreeFind) {
        nbtTreeFind = reinterpret_cast<NbtTreeFindFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NbtTreeFind));
    }
    if (!nbtTreeFind) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(compound);
    NbtTreeKey searchKey{key, length};
    auto* treeRoot = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeRoot;
    auto* treeEnd = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeEnd;
    void* node = nbtTreeFind(treeRoot, &searchKey);
    return node == treeEnd ? nullptr : node;
}

std::uint32_t nodeType(void* node) {
    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodeType);
}

void* nodePayload(void* node) {
    return reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodePayload;
}

template <std::size_t N>
bool containsTag(void* compound, const char (&key)[N]) {
    return treeFindNode(compound, key, N - 1) != nullptr;
}

template <std::size_t N>
void* getListTag(void* compound, const char (&key)[N]) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node || nodeType(node) != 9) return nullptr;
    return nodePayload(node);
}

int listSize(ListTagLayout* list) {
    if (!list || !list->begin || !list->end) return 0;
    auto difference = reinterpret_cast<std::byte*>(list->end) - reinterpret_cast<std::byte*>(list->begin);
    if (difference <= 0) return 0;
    return static_cast<int>(difference / sizeof(void*));
}

const std::vector<MemoryRange>& minecraftRanges() {
    static const std::vector<MemoryRange> ranges = []() {
        std::vector<MemoryRange> out;
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libminecraftpe.so") == std::string::npos) continue;
            std::uintptr_t start = 0;
            std::uintptr_t end = 0;
            char perms[5]{};
            if (std::sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) == 3) {
                out.push_back({start, end, std::strchr(perms, 'x') != nullptr});
            }
        }
        return out;
    }();
    return ranges;
}

bool isMappedInMinecraft(std::uintptr_t address, bool executable = false) {
    for (const auto& range : minecraftRanges()) {
        if (address >= range.start && address < range.end) {
            if (!executable || range.executable) return true;
        }
    }
    return false;
}

struct FoodInfo {
    bool valid = false;
    int nutrition = 0;
    float saturationModifier = 0.0f;
    int saturation = 0;
};

FoodInfo resolveFoodInfo(void* item) {
    FoodInfo result;
    if (!item) return result;

    constexpr std::size_t ScanLimit = 0x300;
    const auto base = reinterpret_cast<std::uintptr_t>(item);
    for (std::size_t offset = 0; offset + sizeof(void*) <= ScanLimit; offset += alignof(void*)) {
        auto* candidate = *reinterpret_cast<void**>(base + offset);
        if (!candidate) continue;

        const auto vtable = *reinterpret_cast<std::uintptr_t*>(candidate);
        if (!isMappedInMinecraft(vtable)) continue;
        if (!isMappedInMinecraft(*reinterpret_cast<std::uintptr_t*>(vtable), true)) continue;

        auto* food = reinterpret_cast<IFoodItemComponent*>(candidate);
        const int nutrition = food->getNutrition();
        const float saturationModifier = food->getSaturationModifier();

        if (nutrition < 0 || nutrition > 64) continue;
        if (saturationModifier < 0.0f || saturationModifier > 10.0f) continue;
        if (nutrition == 0 && saturationModifier == 0.0f) continue;

        result.valid = true;
        result.nutrition = nutrition;
        result.saturationModifier = saturationModifier;
        result.saturation = static_cast<int>(std::lround(saturationModifier * nutrition * 2.0f));
        return result;
    }

    return result;
}

std::string buildDurabilityLine(const AdvancedItemTooltipsModule& module, short current, short maximum, bool hasDurability) {
    if (!hasDurability && !module.m_showDurabilityForNonDamageable) return {};

    std::string line = module.m_durabilityLineTemplate;
    const double percent = maximum > 0 ? (static_cast<double>(current) * 100.0 / static_cast<double>(maximum)) : 100.0;
    const auto color = lerpColor(module.m_lowColor, module.m_highColor, percent / 100.0);

    replaceStringToken(line, "@durability_color", module.m_colorizeDurability ? colorTag(color) : "");
    replaceStringToken(line, "@reset", module.m_colorizeDurability ? resetTag() : "");

    if (hasDurability) {
        replaceIntToken(line, "@durability", current);
        replaceIntToken(line, "@durability_max", maximum);
        replaceDoubleToken(line, "@durability_percent", percent, 0);
        replaceStringToken(line, "@durability_percent_raw", std::to_string(static_cast<int>(std::lround(percent))));
        replaceStringToken(line, "@durability_percent_value", std::to_string(static_cast<int>(std::lround(percent))) + "%");
    } else {
        replaceStringToken(line, "@durability", "N/A");
        replaceStringToken(line, "@durability_max", "N/A");
        replaceStringToken(line, "@durability_percent", "N/A");
        replaceStringToken(line, "@durability_percent_raw", "N/A");
        replaceStringToken(line, "@durability_percent_value", "N/A");
    }

    return line;
}

std::string buildFoodLine(const AdvancedItemTooltipsModule& module, void* item) {
    if (!module.m_showFoodInfo) return {};
    const FoodInfo info = resolveFoodInfo(item);
    if (!info.valid) return {};

    std::string line = module.m_foodLineTemplate;
    replaceStringToken(line, "@food_color", module.m_colorizeFoodInfo ? colorTag(module.m_foodColor) : "");
    replaceStringToken(line, "@reset", module.m_colorizeFoodInfo ? resetTag() : "");
    replaceIntToken(line, "@food_nutrition", info.nutrition);
    replaceIntToken(line, "@food_saturation", info.saturation);
    replaceDoubleToken(line, "@food_saturation_modifier", info.saturationModifier, 2);
    replaceIntToken(line, "@nutrition", info.nutrition);
    replaceIntToken(line, "@saturation", info.saturation);
    replaceDoubleToken(line, "@saturation_modifier", info.saturationModifier, 2);
    return line;
}

std::string buildBeeLine(const AdvancedItemTooltipsModule& module, void* stack, void* item) {
    if (!module.m_showBeeInfo || !stack || !item) return {};

    const auto rawNameId = readRawNameId(item);
    if (rawNameId != "bee_nest" && rawNameId != "beehive") return {};

    int bees = 0;
    void* userData = readStackUserData(stack);
    if (userData && (containsTag(userData, "Occupants") || containsTag(userData, "occupants"))) {
        auto* list = reinterpret_cast<ListTagLayout*>(getListTag(userData, "Occupants"));
        if (!list) list = reinterpret_cast<ListTagLayout*>(getListTag(userData, "occupants"));
        if (list) bees = listSize(list);
    }

    std::string line = module.m_beeLineTemplate;
    replaceStringToken(line, "@bee_color", module.m_colorizeBeeInfo ? colorTag(module.m_beeColor) : "");
    replaceStringToken(line, "@reset", module.m_colorizeBeeInfo ? resetTag() : "");
    replaceIntToken(line, "@bee_count", bees);
    replaceStringToken(line, "@bee_plural", bees == 1 ? "" : "s");
    return line;
}

std::string formatTooltip(AdvancedItemTooltipsModule& module, void* stack, std::string& originalText) {
    auto* item = readStackItem(stack);
    if (!item) return originalText;

    const auto rawNameId = readRawNameId(item);
    const auto ns = readItemNamespace(item);
    const auto id = readItemId(item);

    const short maxDamage = getItemMaxDamage(item);
    const short damage = readDamageValue(stack);
    const bool hasDurability = maxDamage > 0;
    const short current = hasDurability ? std::max<short>(0, static_cast<short>(maxDamage - damage)) : 0;

    const std::string durabilityLine = buildDurabilityLine(module, current, maxDamage, hasDurability);
    const std::string foodLine = buildFoodLine(module, item);
    const std::string beeLine = buildBeeLine(module, stack, item);

    const std::string durabilityBlock = durabilityLine.empty() ? std::string{} : std::string("\n") + durabilityLine;
    const std::string foodBlock = foodLine.empty() ? std::string{} : std::string("\n") + foodLine;
    const std::string beeBlock = beeLine.empty() ? std::string{} : std::string("\n") + beeLine;

    std::string result = module.m_textTemplate;
    replaceStringToken(result, "@originalText", originalText);
    replaceStringToken(result, "@durability_line", durabilityLine);
    replaceStringToken(result, "@durability_block", durabilityBlock);
    replaceStringToken(result, "@food_line", foodLine);
    replaceStringToken(result, "@food_block", foodBlock);
    replaceStringToken(result, "@bee_line", beeLine);
    replaceStringToken(result, "@bee_block", beeBlock);
    replaceStringToken(result, "@namespace", ns);
    replaceStringToken(result, "@rawNameId", rawNameId);
    replaceIntToken(result, "@id", id);

    const double percent = maxDamage > 0 ? (static_cast<double>(current) * 100.0 / static_cast<double>(maxDamage)) : 100.0;
    const std::string computedColor = module.m_colorizeDurability && !durabilityLine.empty()
        ? colorTag(lerpColor(module.m_lowColor, module.m_highColor, percent / 100.0))
        : std::string{};
    replaceStringToken(result, "@durability_color", computedColor);
    replaceStringToken(result, "@reset", module.m_colorizeDurability && !durabilityLine.empty() ? resetTag() : "");

    return result;
}

void appendFormattedHoverTextHook(void* self, void* stack, void* level, std::string& text, bool flag) {
    if (g_originalAppend) {
        g_originalAppend(self, stack, level, text, flag);
    }

    if (!g_module || !g_module->enabled || !stack) return;
    text = formatTooltip(*g_module, stack, text);
}

}

AdvancedItemTooltipsModule::AdvancedItemTooltipsModule()
    : Module("Advanced Item Tooltips", "Customizes item hover text, durability text, food info, bee hive/nest info, and durability color based on item percentage.") {
    showInMenu = true;
}

AdvancedItemTooltipsModule::~AdvancedItemTooltipsModule() {
    if (g_module == this) g_module = nullptr;
    if (g_hook) {
        bedrocktools::hooks::remove(g_hook);
        g_hook = nullptr;
    }
}

void AdvancedItemTooltipsModule::onInit() {
    g_module = this;
    if (!g_hook) {
        const auto target = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemAppendFormattedHoverText);
        if (target) {
            g_hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(appendFormattedHoverTextHook),
                reinterpret_cast<void**>(&g_originalAppend)
            );
        }
    }
}

void AdvancedItemTooltipsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_colorizeDurability = j.value("colorizeDurability", m_colorizeDurability);
    m_showDurabilityForNonDamageable = j.value("showDurabilityForNonDamageable", m_showDurabilityForNonDamageable);
    m_showFoodInfo = j.value("showFoodInfo", m_showFoodInfo);
    m_showBeeInfo = j.value("showBeeInfo", m_showBeeInfo);
    m_colorizeFoodInfo = j.value("colorizeFoodInfo", m_colorizeFoodInfo);
    m_colorizeBeeInfo = j.value("colorizeBeeInfo", m_colorizeBeeInfo);
    m_textTemplate = j.value("textTemplate", m_textTemplate);
    m_durabilityLineTemplate = j.value("durabilityLineTemplate", m_durabilityLineTemplate);
    m_foodLineTemplate = j.value("foodLineTemplate", m_foodLineTemplate);
    m_beeLineTemplate = j.value("beeLineTemplate", m_beeLineTemplate);

    auto parseColor = [&](const std::string& key, std::uint32_t& outColor) {
        if (!j.contains(key)) return;
        const auto value = j[key].get<std::string>();
        std::uint32_t parsed = outColor;
        if (parseHexColor(value, parsed)) outColor = parsed;
    };

    parseColor("durabilityLowColor", m_lowColor);
    parseColor("durabilityHighColor", m_highColor);
    parseColor("foodColor", m_foodColor);
    parseColor("beeColor", m_beeColor);
}

void AdvancedItemTooltipsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["colorizeDurability"] = m_colorizeDurability;
    j["showDurabilityForNonDamageable"] = m_showDurabilityForNonDamageable;
    j["showFoodInfo"] = m_showFoodInfo;
    j["showBeeInfo"] = m_showBeeInfo;
    j["colorizeFoodInfo"] = m_colorizeFoodInfo;
    j["colorizeBeeInfo"] = m_colorizeBeeInfo;
    j["textTemplate"] = m_textTemplate;
    j["durabilityLineTemplate"] = m_durabilityLineTemplate;
    j["foodLineTemplate"] = m_foodLineTemplate;
    j["beeLineTemplate"] = m_beeLineTemplate;

    char lowHex[12]{};
    char highHex[12]{};
    char foodHex[12]{};
    char beeHex[12]{};
    std::snprintf(lowHex, sizeof(lowHex), "#%08X", m_lowColor);
    std::snprintf(highHex, sizeof(highHex), "#%08X", m_highColor);
    std::snprintf(foodHex, sizeof(foodHex), "#%08X", m_foodColor);
    std::snprintf(beeHex, sizeof(beeHex), "#%08X", m_beeColor);
    j["durabilityLowColor"] = std::string(lowHex);
    j["durabilityHighColor"] = std::string(highHex);
    j["foodColor"] = std::string(foodHex);
    j["beeColor"] = std::string(beeHex);
}
