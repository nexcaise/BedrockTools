#include "guiscalechanger.hpp"

#include "core/GameHooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/offsets/Core.hpp>
#include <bedrocktools/sdk/offsets/UI.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
bedrocktools::sdk::ClientInstance* currentClientInstance() {
    return reinterpret_cast<bedrocktools::sdk::ClientInstance*>(bedrocktools::core::gamehooks::clientInstance());
}
}

GuiScaleChangerModule::GuiScaleChangerModule()
    : Module("Gui Scale Changer", "Overrides the in-game GUI scale using the existing BedrockTools update event.") {
    showInMenu = true;
}

GuiScaleChangerModule::~GuiScaleChangerModule() {
    unsubscribe();
}

void GuiScaleChangerModule::subscribe() {
    if (m_subscription != 0) return;
    m_subscription = bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [this](bedrocktools::events::ClientInstanceUpdateEvent& event) {
            if (!enabled || !event.clientInstance) return;
            applyToGuiData(reinterpret_cast<GuiData*>(event.clientInstance->guiData()), false);
        },
        bedrocktools::events::EventPriority::Late
    );
}

void GuiScaleChangerModule::unsubscribe() {
    if (m_subscription == 0) return;
    bedrocktools::events::bus().unsubscribe(m_subscription);
    m_subscription = 0;
}

void GuiScaleChangerModule::applyToGuiData(GuiData* gui, bool captureDefault) {
    if (!gui || !gui->screenSizeDataValid) return;

    if (captureDefault && !m_defaultScaleCaptured) {
        m_defaultScale = gui->guiScale;
        m_defaultScaleCaptured = true;
    }

    const double requested = m_targetScale;
    const float wanted = m_roundScale ? static_cast<float>(std::round(requested)) : static_cast<float>(requested);
    if (wanted <= 0.0f) return;

    auto& screenSizeData = gui->screenSizeData;
    gui->guiScale = wanted;
    gui->invGuiScale = 1.0f / wanted;
    screenSizeData.guiSizeX = screenSizeData.screenSizeX * gui->invGuiScale;
    screenSizeData.guiSizeY = screenSizeData.screenSizeY * gui->invGuiScale;
    gui->screenSizeDataValid = true;
}

void GuiScaleChangerModule::applyToCurrentClient() {
    auto* ci = currentClientInstance();
    if (!ci) return;
    applyToGuiData(reinterpret_cast<GuiData*>(ci->guiData()), true);
}

void GuiScaleChangerModule::restoreDefaultScale() {
    if (!m_defaultScaleCaptured || m_defaultScale <= 0.0f) return;
    auto* ci = currentClientInstance();
    if (!ci) return;

    auto* gui = reinterpret_cast<GuiData*>(ci->guiData());
    if (!gui || !gui->screenSizeDataValid) return;

    auto& screenSizeData = gui->screenSizeData;
    gui->guiScale = m_defaultScale;
    gui->invGuiScale = 1.0f / m_defaultScale;
    screenSizeData.guiSizeX = screenSizeData.screenSizeX * gui->invGuiScale;
    screenSizeData.guiSizeY = screenSizeData.screenSizeY * gui->invGuiScale;
    gui->screenSizeDataValid = true;
}

void GuiScaleChangerModule::onInit() {
    subscribe();
}

void GuiScaleChangerModule::onEnable() {
    applyToCurrentClient();
}

void GuiScaleChangerModule::onDisable() {
    restoreDefaultScale();
}

void GuiScaleChangerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_targetScale = j.value("guiScale", m_targetScale);
    m_roundScale = j.value("round", m_roundScale);

    if (j.contains("defaultScaleCaptured")) {
        m_defaultScaleCaptured = j.value("defaultScaleCaptured", m_defaultScaleCaptured);
    }
    if (j.contains("defaultScale")) {
        m_defaultScale = j.value("defaultScale", m_defaultScale);
    }
}

void GuiScaleChangerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["guiScale"] = m_targetScale;
    j["round"] = m_roundScale;
    j["defaultScaleCaptured"] = m_defaultScaleCaptured;
    j["defaultScale"] = m_defaultScale;
}
