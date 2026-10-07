#pragma once

// Reel Fishing III mod (step 16c): editor for fish regions (RF3Regions.h). Lists the current scene's
// regions, adds one around Link, sets its box, water surface, fish count and allowed RF3 records, and
// saves rf3_fish_regions.json.

#include <ship/window/gui/GuiWindow.h>

class RF3RegionWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~RF3RegionWindow(){};

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override{};
};
