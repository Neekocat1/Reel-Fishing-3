#pragma once
#ifndef ARCHIPELAGO_H
#define ARCHIPELAGO_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // ARCHIPELAGO_H

#include <libultraship/libultraship.h>

#ifdef __cplusplus

class ArchipelagoClient;

class ArchipelagoWindow : public Ship::GuiWindow {
    public:
    using GuiWindow::GuiWindow;

    void InitElement() override {};
    void DrawElement() override;
    void UpdateElement() override{};

    private:
    //std::shared_ptr<ArchipelagoClient AP_client = nullptr;

    void ArchipelagoDrawConnectPage();
};


#endif