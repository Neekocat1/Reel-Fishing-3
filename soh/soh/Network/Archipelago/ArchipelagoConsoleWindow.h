#pragma once
#ifndef ARCHIPELAGO_CONSOLE_WINDOW_H
#define ARCHIPELAGO_CONSOLE_WINDOW_H

#include <libultraship/libultraship.h>

class ArchipelagoConsoleWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    ~ArchipelagoConsoleWindow() {};

  protected:
    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override {};
};

void AddToArchipelagoConsole(const char* fmt, ...);

#endif // ARCHIPELAGO_CONSOLE_WINDOW_H