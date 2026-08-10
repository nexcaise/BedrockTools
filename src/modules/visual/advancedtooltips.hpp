#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <string>

class AdvancedItemTooltipsModule : public Module {
public:
    AdvancedItemTooltipsModule();
    ~AdvancedItemTooltipsModule() override;

    void onInit() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

public:
    bool m_colorizeDurability = true;
    bool m_showDurabilityForNonDamageable = false;

    bool m_showFoodInfo = true;
    bool m_showBeeInfo = true;
    bool m_colorizeFoodInfo = true;
    bool m_colorizeBeeInfo = true;

    std::string m_textTemplate = "@originalText@durability_block@food_block@bee_block\n@namespace:@rawNameId (#@id)";
    std::string m_durabilityLineTemplate = "@durability_colorDurability: @durability/@durability_max (@durability_percent)@reset";
    std::string m_foodLineTemplate = "@food_colorFood: @food_nutrition\n§7Saturation: @food_saturation@reset";
    std::string m_beeLineTemplate = "@bee_colorContains @bee_count bee@bee_plural@reset";

    std::uint32_t m_lowColor = 0xFFFF5555u;
    std::uint32_t m_highColor = 0xFF55FF55u;
    std::uint32_t m_foodColor = 0xFF55AA55u;
    std::uint32_t m_beeColor = 0xFF55AAFFu;
    std::uint64_t m_subscription = 0;
};
