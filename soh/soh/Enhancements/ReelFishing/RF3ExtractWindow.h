#pragma once

// Reel Fishing III mod (step 76b, P0): "Reel Fishing III Disc Setup", the in-game extractor's window. Picks the
// player's Reel Fishing III disc image, checks it and runs the extractor (RF3Extract.h) with a progress bar.

#include <ship/window/gui/GuiWindow.h>

class RF3ExtractWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~RF3ExtractWindow(){};

  protected:
    void InitElement() override{};
    void DrawElement() override;
    void UpdateElement() override{};
};
