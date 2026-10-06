#pragma once

// Reel Fishing III mod: live debug view of the RF3 fight (state, values, distance, inputs).

#include <ship/window/gui/GuiWindow.h>

class RF3DebugWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~RF3DebugWindow(){};

  protected:
    void InitElement() override{};
    void DrawElement() override;
    void UpdateElement() override{};
};
