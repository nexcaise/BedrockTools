#pragma once

#include "../Module.hpp"
#include <cstddef>
#include <string>

class GuiScaleChangerModule : public Module {
public:
    GuiScaleChangerModule();
    ~GuiScaleChangerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    struct ScreenSizeData {
        float totalScreenSizeX;
        float totalScreenSizeY;
        float screenSizeX;
        float screenSizeY;
        float guiSizeX;
        float guiSizeY;
    };

    struct GuiData {
        std::byte pad0[0x40];
        ScreenSizeData screenSizeData;
        bool screenSizeDataValid;
        float guiScale;
        float invGuiScale;
    };

    void subscribe();
    void unsubscribe();
    void applyToCurrentClient();
    void applyToGuiData(GuiData* gui, bool captureDefault);
    void restoreDefaultScale();

    bool m_roundScale = true;
    double m_targetScale = 2.0;
    bool m_defaultScaleCaptured = false;
    float m_defaultScale = 0.0f;
    std::uint64_t m_subscription = 0;
};
