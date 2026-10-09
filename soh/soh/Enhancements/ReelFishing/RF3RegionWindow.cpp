// Reel Fishing III mod (step 16c): the fish region editor. See RF3RegionWindow.h and
// docs/DESIGN.md step 16.

#include "RF3RegionWindow.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "RF3FishTable.h"
#include "RF3Regions.h"

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
// OPEN_DISPS / CLOSE_DISPS (as kaleido.cpp declares them)
void FrameInterpolation_RecordOpenChild(const void* a, int b);
void FrameInterpolation_RecordCloseChild(void);
}

namespace {

std::string sStatus;          // last load/save result
int sOpenedFor = -1;          // step 16h: the selected region whose header was opened for it
int sPendingDelete = -1;      // region index whose Delete was clicked once (click again to confirm)
constexpr float kNewHalfSize = 150.0f; // a new region: a 300 x 300 box around Link

// The water surface under (x, z), if there is water there.
bool WaterSurfaceAt(PlayState* play, float x, float z, float* surfaceY) {
    WaterBox* waterBox;
    return WaterBox_GetSurface1(play, &play->colCtx, x, z, surfaceY, &waterBox) != 0;
}

void NormalizeBox(rf3::FishRegion& r) {
    if (r.minX > r.maxX) {
        std::swap(r.minX, r.maxX);
    }
    if (r.minZ > r.maxZ) {
        std::swap(r.minZ, r.maxZ);
    }
}

// The species list: one row per species, a checkbox per allowed size class (s / n / b).
void DrawRecordChoices(rf3::FishRegion& r) {
    for (int species = 0; species <= 21; species++) {
        const char* speciesName = nullptr;
        for (const rf3::FishRecord& rec : rf3::kFishRecords) {
            if ((rec.species == species) && rf3::IsRegionRecord(rec.record)) {
                speciesName = rec.speciesName;
                break;
            }
        }
        if (speciesName == nullptr) {
            continue;
        }
        ImGui::Text("%-18s", speciesName);
        for (const rf3::FishRecord& rec : rf3::kFishRecords) {
            if ((rec.species != species) || !rf3::IsRegionRecord(rec.record)) {
                continue;
            }
            auto it = std::find(r.records.begin(), r.records.end(), (int)rec.record);
            bool on = (it != r.records.end());
            char label[64];
            std::snprintf(label, sizeof(label), "%c %d-%d mm##rec%d", rec.sizeClass, rec.params.lengthMinMm,
                          rec.params.lengthMaxMm, rec.record);
            ImGui::SameLine();
            if (ImGui::Checkbox(label, &on)) {
                if (on) {
                    r.records.push_back(rec.record);
                } else {
                    r.records.erase(it);
                }
                rf3::RegionsDirty() = true;
            }
        }
    }
}

void DrawRegion(PlayState* play, rf3::FishRegion& r, int index, const Vec3f& link) {
    char header[160];
    std::snprintf(header, sizeof(header), "#%d %s  (%.0f x %.0f, %d fish, %d records)%s###region%d", index,
                  r.name.c_str(), r.maxX - r.minX, r.maxZ - r.minZ, r.fishCount, (int)r.records.size(),
                  r.Contains(link.x, link.z) ? "  <- Link is here" : "", index);
    if ((rf3::SelectedRegion() == index) && (sOpenedFor != index)) {
        ImGui::SetNextItemOpen(true); // selected in the world (edit mode) or just created
        sOpenedFor = index;
    }
    bool open = ImGui::CollapsingHeader(header);
    if (ImGui::IsItemClicked()) {
        rf3::SelectedRegion() = index; // step 16h: clicking a region selects it (yellow in the world)
        sOpenedFor = index;
    }
    if (!open) {
        return;
    }
    ImGui::PushID(index);

    char name[96];
    std::snprintf(name, sizeof(name), "%s", r.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) {
        r.name = name;
        rf3::RegionsDirty() = true;
    }

    float xs[2] = { r.minX, r.maxX };
    float zs[2] = { r.minZ, r.maxZ };
    ImGui::TextDisabled("Box edges in the region's own frame (turned by the rotation below):");
    if (ImGui::DragFloat2("X min / max", xs, 5.0f)) {
        r.minX = xs[0];
        r.maxX = xs[1];
        rf3::RegionsDirty() = true;
    }
    if (ImGui::DragFloat2("Z min / max", zs, 5.0f)) {
        r.minZ = zs[0];
        r.maxZ = zs[1];
        rf3::RegionsDirty() = true;
    }
    if (ImGui::DragFloat("Rotation (degrees)", &r.angle, 0.5f, -180.0f, 180.0f, "%.1f")) { // step 16i
        rf3::RegionsDirty() = true;
    }
    if (ImGui::Button("Corner 1 = Link")) {
        float fx, fz;
        r.Corner(2, &fx, &fz); // the opposite corner stays where it is in the world
        r.SetFromCornersWorld(link.x, link.z, fx, fz);
        rf3::RegionsDirty() = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Corner 2 = Link")) {
        float fx, fz;
        r.Corner(0, &fx, &fz);
        r.SetFromCornersWorld(fx, fz, link.x, link.z);
        rf3::RegionsDirty() = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Centre on Link")) { // step 16g: move the box, keep its size
        r.MoveTo(link.x, link.z);
        rf3::RegionsDirty() = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fix corners")) {
        NormalizeBox(r); // corners set from Link can come out min > max
        rf3::RegionsDirty() = true;
    }

    if (ImGui::DragFloat("Water surface Y", &r.surfaceY, 1.0f)) {
        rf3::RegionsDirty() = true;
    }

    // Step 20c-3a: the water current (drifts a resting fly).
    float yaw = r.CurrentYawDeg();
    if (ImGui::DragFloat("Current toward (degrees)", &yaw, 1.0f, -180.0f, 180.0f, "%.0f")) {
        r.currentYaw = yaw;
        r.currentSet = true;
        rf3::RegionsDirty() = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("= Link's facing")) {
        r.currentYaw = GET_PLAYER(play)->actor.shape.rot.y * (360.0f / 65536.0f);
        r.currentSet = true;
        rf3::RegionsDirty() = true;
    }
    if (ImGui::DragFloat("Current speed (m/s)", &r.currentSpeed, 0.01f, 0.0f, 2.0f, "%.2f")) {
        r.currentSet = true;
        rf3::RegionsDirty() = true;
    }
    if (!r.currentSet) {
        ImGui::SameLine();
        ImGui::TextDisabled("(default: along the box)");
    }
    if (ImGui::DragFloat("Current reach outside (m)", &r.currentReachM, 0.1f, 0.0f, 50.0f, "%.1f")) { // step 20c-4
        rf3::RegionsDirty() = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(falls to 0 this far outside the box)");
    ImGui::SameLine();
    if (ImGui::Button("Read under Link")) {
        float y;
        if (WaterSurfaceAt(play, link.x, link.z, &y)) {
            r.surfaceY = y;
            rf3::RegionsDirty() = true;
        } else {
            sStatus = "No water under Link.";
        }
    }

    if (ImGui::SliderInt("Fish count", &r.fishCount, 0, 16)) {
        rf3::RegionsDirty() = true;
    }

    // Step 24a: the randomizer area this water is in (logic requires reaching it, step 24c).
    std::vector<rf3::RandoArea> areas = rf3::RandoAreasForScene(r.scene);
    std::string shown = "(any area of this scene)";
    for (const rf3::RandoArea& a : areas) {
        if (a.enumName == r.randoArea) {
            shown = a.name + " (" + a.enumName + ")";
        }
    }
    if (!r.randoArea.empty() && shown == "(any area of this scene)") {
        shown = r.randoArea + " (not in this scene!)";
    }
    if (ImGui::BeginCombo("Randomizer area", shown.c_str())) {
        if (ImGui::Selectable("(any area of this scene)", r.randoArea.empty())) {
            r.randoArea.clear();
            rf3::RegionsDirty() = true;
        }
        for (const rf3::RandoArea& a : areas) {
            std::string label = a.name + " (" + a.enumName + ")";
            if (ImGui::Selectable(label.c_str(), a.enumName == r.randoArea)) {
                r.randoArea = a.enumName;
                rf3::RegionsDirty() = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d in scene)", static_cast<int>(areas.size()));

    if (ImGui::TreeNode("Species and size classes (s juvenile, n normal, b big)")) {
        DrawRecordChoices(r);
        ImGui::TreePop();
    }

    if (sPendingDelete == index) {
        if (ImGui::Button("Really delete this region")) {
            rf3::FishRegions().erase(rf3::FishRegions().begin() + index);
            if (rf3::SelectedRegion() == index) {
                rf3::SelectedRegion() = -1;
            } else if (rf3::SelectedRegion() > index) {
                rf3::SelectedRegion()--;
            }
            sPendingDelete = -1;
            rf3::RegionsDirty() = true;
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) {
            sPendingDelete = -1;
        }
    } else if (ImGui::Button("Delete region")) {
        sPendingDelete = index;
    }

    ImGui::PopID();
}

} // namespace

void RF3RegionWindow::InitElement() {
    std::string error;
    sStatus = rf3::LoadFishRegions(&error) ? "Loaded." : ("Load failed: " + error);
}

void RF3RegionWindow::DrawElement() {
    ImGui::TextWrapped("File: %s", rf3::FishRegionsPath().c_str());
    if (ImGui::Button("Save")) {
        std::string error;
        if (rf3::SaveFishRegions(&error)) {
            sStatus = "Saved.";
            rf3::RegionsDirty() = false;
        } else {
            sStatus = "Save failed: " + error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload (drops unsaved edits)")) {
        std::string error;
        sStatus = rf3::LoadFishRegions(&error) ? "Reloaded." : ("Load failed: " + error);
        rf3::RegionsDirty() = false;
    }
    ImGui::SameLine();
    ImGui::Text("%s%s", sStatus.c_str(), rf3::RegionsDirty() ? "  (unsaved changes)" : "");
    bool drawAlways = CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.DrawRegions"), 0) != 0;
    if (ImGui::Checkbox("Draw regions in the world when this window is closed", &drawAlways)) {
        CVarSetInteger(CVAR_ENHANCEMENT("ReelFishing3.DrawRegions"), drawAlways ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::TextDisabled("In the world: yellow = selected, green = Link inside, blue = other regions.");

    // Step 16h: edit mode, placement without Link.
    rf3::RegionEditView ev = rf3::GetRegionEditView();
    if (!ev.active) {
        if (ImGui::Button("Edit mode: cursor + camera")) {
            rf3::RegionEditStart(&sStatus);
        }
    } else {
        if (ImGui::Button("Leave edit mode")) {
            rf3::RegionEditStop();
            sStatus = "Left edit mode.";
        }
        ImGui::SameLine();
        ImGui::Text("Cursor x %.0f  y %.0f  z %.0f  (%s)%s%s", ev.cursorX, ev.cursorY, ev.cursorZ,
                    ev.cursorOnWater ? "water" : "ground", ev.hasCorner1 ? "  - corner 1 set" : "",
                    ev.grab[0] ? "  - holding" : "");
        if (ev.grab[0]) {
            ImGui::SameLine();
            ImGui::Text("%s", ev.grab);
        }
        ImGui::TextWrapped("Stick: move cursor (hold Z: faster).  C-Left/Right or right stick: turn.  "
                           "C-Up/Down: zoom.  D-pad Left/Right: rotate the selected region (hold Z: fine).  "
                           "A: corner 1, A again: corner 2 = new region.  R: select the "
                           "region under the cursor.  B: grab the selected region's corner, edge or whole box; "
                           "B again: drop.  START: leave.");
    }
    ImGui::Separator();

    PlayState* play = gPlayState;
    if (play == nullptr) {
        ImGui::Text("Load a save file to place regions.");
        return;
    }
    Player* player = GET_PLAYER(play);
    Vec3f link = player->actor.world.pos;
    float waterY;
    bool water = WaterSurfaceAt(play, link.x, link.z, &waterY);

    ImGui::Text("Scene %d   Link x %.0f  y %.0f  z %.0f", play->sceneNum, link.x, link.y, link.z);
    if (water) {
        ImGui::Text("Water under Link: surface y %.0f", waterY);
    } else {
        ImGui::Text("No water under Link.");
    }

    std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    int here = 0;
    int otherScenes = 0;
    std::string inside;
    for (size_t i = 0; i < regions.size(); i++) {
        if (regions[i].scene != play->sceneNum) {
            otherScenes++;
            continue;
        }
        here++;
        if (regions[i].Contains(link.x, link.z)) {
            inside += (inside.empty() ? "" : ", ") + std::string("#") + std::to_string(i) + " " + regions[i].name;
        }
    }
    ImGui::Text("Link is in: %s", inside.empty() ? "no region" : inside.c_str());
    ImGui::Text("Regions here: %d   (other scenes: %d)", here, otherScenes);
    if (ImGui::Button("Respawn region fish here")) { // step 16d: try edits without re-entering the scene
        rf3::RespawnRegionFish(&sStatus);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(fish also spawn on entering the scene)");

    if (ImGui::Button("Add region around Link")) {
        rf3::FishRegion r;
        r.scene = play->sceneNum;
        r.name = "Scene " + std::to_string(play->sceneNum) + " region " + std::to_string(here + 1);
        r.minX = link.x - kNewHalfSize;
        r.maxX = link.x + kNewHalfSize;
        r.minZ = link.z - kNewHalfSize;
        r.maxZ = link.z + kNewHalfSize;
        r.surfaceY = water ? waterY : link.y;
        regions.push_back(r);
        rf3::RegionsDirty() = true;
        if (!water) {
            sStatus = "No water under Link: surface set to Link's height.";
        }
    }
    ImGui::Separator();

    for (size_t i = 0; i < regions.size(); i++) {
        if (regions[i].scene == play->sceneNum) {
            DrawRegion(play, regions[i], (int)i, link);
        }
    }
}

// --- Step 16g: regions drawn in the world ---------------------------------------------------------
// Like SoH's collision viewer (colViewer.cpp): translucent quads in world coordinates, appended to the
// frame at OnPlayDrawEnd. Each region is its water surface plus four sides reaching 150 units down.

namespace {

constexpr float kDrawDepth = 150.0f;
// Colour = primitive colour, alpha = environment alpha (the same mode SoH's collision viewer defines
// locally; it isn't in gbi.h).
#define RF3_CC_PRIMITIVE_ENVA 0, 0, 0, PRIMITIVE, 0, 0, 0, ENVIRONMENT
std::vector<Gfx> sDrawDl;
std::vector<Vtx> sDrawVtx;

void PushQuad(const Vec3f& v0, const Vec3f& v1, const Vec3f& v2, const Vec3f& v3) {
    const Vec3f* corners[4] = { &v0, &v1, &v2, &v3 };
    for (const Vec3f* c : corners) {
        Vtx v = {};
        v.v.ob[0] = (short)c->x;
        v.v.ob[1] = (short)c->y;
        v.v.ob[2] = (short)c->z;
        v.v.cn[0] = v.v.cn[1] = v.v.cn[2] = v.v.cn[3] = 0xFF;
        sDrawVtx.push_back(v);
    }
    sDrawDl.push_back(gsSPVertex((uintptr_t)&sDrawVtx.at(sDrawVtx.size() - 4), 4, 0));
    sDrawDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

} // namespace

// At global scope: OPEN_DISPS declares the frame-interpolation functions inside the calling function,
// and inside an anonymous namespace that declaration would not bind to the C functions.
static void DrawRegionsInWorld() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    bool show = CVarGetInteger(CVAR_WINDOW("ReelFishing3Regions"), 0) ||
                CVarGetInteger(CVAR_ENHANCEMENT("ReelFishing3.DrawRegions"), 0) || rf3::RegionEditActive();
    if (!show) {
        return;
    }
    const std::vector<rf3::FishRegion>& regions = rf3::FishRegions();
    size_t here = 0;
    for (const rf3::FishRegion& r : regions) {
        here += (r.scene == play->sceneNum) ? 1 : 0;
    }
    rf3::RegionEditView ev = rf3::GetRegionEditView(); // step 16h
    if ((here == 0) && !ev.active) {
        return;
    }

    Player* player = GET_PLAYER(play);
    sDrawDl.clear();
    sDrawVtx.clear();
    sDrawVtx.reserve((here * 7 + 5) * 4); // exact: the vertex pointers in the display list must stay valid

    uint32_t rm = Z_CMP | IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_XLU;
    uint32_t blc1 = GBL_c1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    uint32_t blc2 = GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    sDrawDl.push_back(gsDPPipeSync());
    sDrawDl.push_back(gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF));
    sDrawDl.push_back(gsDPSetCycleType(G_CYC_1CYCLE));
    sDrawDl.push_back(gsDPSetRenderMode(rm | blc1, rm | blc2));
    sDrawDl.push_back(gsDPSetCombineMode(RF3_CC_PRIMITIVE_ENVA, RF3_CC_PRIMITIVE_ENVA));
    sDrawDl.push_back(gsSPLoadGeometryMode(G_ZBUFFER)); // no culling: both sides of every face
    sDrawDl.push_back(gsDPSetEnvColor(0xFF, 0xFF, 0xFF, 0x60));
    sDrawDl.push_back(gsSPMatrix(&gMtxClear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));

    for (size_t i = 0; i < regions.size(); i++) {
        const rf3::FishRegion& r = regions[i];
        if (r.scene != play->sceneNum) {
            continue;
        }
        if ((int)i == rf3::SelectedRegion()) {
            sDrawDl.push_back(gsDPSetPrimColor(0, 0, 255, 230, 0, 255));
        } else if (r.Contains(player->actor.world.pos.x, player->actor.world.pos.z)) {
            sDrawDl.push_back(gsDPSetPrimColor(0, 0, 0, 255, 80, 255));
        } else {
            sDrawDl.push_back(gsDPSetPrimColor(0, 0, 40, 140, 255, 255));
        }
        float top = r.surfaceY;
        float bottom = r.surfaceY - kDrawDepth;
        float cx[4], cz[4]; // the turned corners (step 16i)
        for (int k = 0; k < 4; k++) {
            r.Corner(k, &cx[k], &cz[k]);
        }
        Vec3f t0 = { cx[0], top, cz[0] }, t1 = { cx[1], top, cz[1] };
        Vec3f t2 = { cx[2], top, cz[2] }, t3 = { cx[3], top, cz[3] };
        Vec3f b0 = { cx[0], bottom, cz[0] }, b1 = { cx[1], bottom, cz[1] };
        Vec3f b2 = { cx[2], bottom, cz[2] }, b3 = { cx[3], bottom, cz[3] };
        PushQuad(t0, t1, t2, t3); // the water surface
        PushQuad(t0, t1, b1, b0);
        PushQuad(t1, t2, b2, b1);
        PushQuad(t2, t3, b3, b2);
        PushQuad(t3, t0, b0, b3);

        // Step 20c-4: an arrow on the surface, at the box centre, in the current's direction.
        float yaw = r.CurrentYawDeg() * 3.14159265f / 180.0f;
        float dx = std::sin(yaw), dz = std::cos(yaw); // along the flow
        float px = dz, pz = -dx;                      // across it
        float half = std::fmin(std::fabs(r.maxX - r.minX), std::fabs(r.maxZ - r.minZ)) * 0.3f;
        float len = std::fmax(40.0f, std::fmin(half, 200.0f));
        float mx = r.CentreX(), mz = r.CentreZ(), y = top + 3.0f;
        float w = 6.0f, hw = 20.0f;
        float neckX = mx + dx * len * 0.25f, neckZ = mz + dz * len * 0.25f;
        Vec3f s0 = { mx - dx * len * 0.5f + px * w, y, mz - dz * len * 0.5f + pz * w };
        Vec3f s1 = { mx - dx * len * 0.5f - px * w, y, mz - dz * len * 0.5f - pz * w };
        Vec3f s2 = { neckX - px * w, y, neckZ - pz * w };
        Vec3f s3 = { neckX + px * w, y, neckZ + pz * w };
        Vec3f h0 = { neckX + px * hw, y, neckZ + pz * hw };
        Vec3f h1 = { neckX - px * hw, y, neckZ - pz * hw };
        Vec3f h2 = { mx + dx * len * 0.5f, y, mz + dz * len * 0.5f };
        sDrawDl.push_back(gsDPSetPrimColor(0, 0, 255, 255, 255, 255));
        PushQuad(s0, s1, s2, s3);
        PushQuad(h0, h1, h2, h2); // the head: a triangle
    }
    if (ev.active) {
        // Step 16h: the cursor (a post and a cross) and, after the first A, the box it would make.
        sDrawDl.push_back(gsDPSetPrimColor(0, 0, 255, 255, 255, 255));
        float x = ev.cursorX, y = ev.cursorY, z = ev.cursorZ;
        Vec3f p0 = { x - 3, y, z }, p1 = { x + 3, y, z }, p2 = { x + 3, y + 60, z }, p3 = { x - 3, y + 60, z };
        Vec3f q0 = { x, y, z - 3 }, q1 = { x, y, z + 3 }, q2 = { x, y + 60, z + 3 }, q3 = { x, y + 60, z - 3 };
        PushQuad(p0, p1, p2, p3);
        PushQuad(q0, q1, q2, q3);
        Vec3f a0 = { x - 25, y + 1, z - 3 }, a1 = { x + 25, y + 1, z - 3 }, a2 = { x + 25, y + 1, z + 3 },
              a3 = { x - 25, y + 1, z + 3 };
        Vec3f b0 = { x - 3, y + 1, z - 25 }, b1 = { x + 3, y + 1, z - 25 }, b2 = { x + 3, y + 1, z + 25 },
              b3 = { x - 3, y + 1, z + 25 };
        PushQuad(a0, a1, a2, a3);
        PushQuad(b0, b1, b2, b3);
        if (ev.hasCorner1) {
            Vec3f c0 = { ev.corner1X, y + 2, ev.corner1Z }, c1 = { x, y + 2, ev.corner1Z }, c2 = { x, y + 2, z },
                  c3 = { ev.corner1X, y + 2, z };
            PushQuad(c0, c1, c2, c3);
        }
    }

    sDrawDl.push_back(gsDPPipeSync());
    sDrawDl.push_back(gsSPEndDisplayList());

    OPEN_DISPS(play->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, sDrawDl.data());
    CLOSE_DISPS(play->state.gfxCtx);
}

namespace {

void RegisterRegionDrawing() {
    COND_HOOK(OnPlayDrawEnd, true, DrawRegionsInWorld);
}

} // namespace

static RegisterShipInitFunc initRF3RegionDrawing(RegisterRegionDrawing);
