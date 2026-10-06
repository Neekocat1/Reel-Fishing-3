#pragma once

// Reel Fishing III mod: optional on-screen tension gauge, shown only during an RF3 fight
// when "Reel Fishing III Tension Gauge" is on. Not part of Reel Fishing III itself.

#include <ship/window/gui/GuiWindow.h>

class RF3TensionGauge final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override{};
    void Draw() override;
    void UpdateElement() override{};
};
