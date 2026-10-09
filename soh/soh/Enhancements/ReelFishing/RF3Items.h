// Reel Fishing III mod (step 22a): RF3's tackle as randomizer items (RG_RF3_*), so Link can hold them up
// with OoT's item-get animation. See docs/DESIGN.md step 22.
#ifndef RF3_ITEMS_H
#define RF3_ITEMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;
struct GetItemEntry;

// The items are RG_RF3_ROD_0 + offset, in this order: 10 rods, 91 lures, 40 flies, 12 bait types, 6 floats, then
// (step 23c) OoT's pole (rod 10) and its lures (91 normal, 92 sinking).
#define RF3_ITEM_COUNT 162
int32_t RF3Items_Kind(int32_t offset);         // RF3_STOCK_* (RF3Bridge.h), -1 if out of range
int32_t RF3Items_Index(int32_t offset);        // RF3's index within its kind
const char* RF3Items_KindName(int32_t offset); // "Rod", "Lure", "Fly", "Bait", "Float"
int32_t RF3Items_Offset(int32_t kind, int32_t index); // the reverse; -1 if out of range

// The get-item model of lures, flies, baits and floats: OoT's pond lure (object_fish), scaled up.
void RF3Items_DrawTackleGI(struct PlayState* play, struct GetItemEntry* getItemEntry);

// Randomizer_Item_Give for an RG_RF3_* item (offset as above): puts it in the stock (RF3Stock.cpp), unless it is
// the queued item being shown (step 22c), which is in the stock already.
void RF3Items_Receive(int32_t offset);
void RF3Items_ReceiveSohFishingPole(void); // step 24d: SoH's RG_FISHING_POLE gives RF3's pole

// Step 22c: Link holds this item up (OoT's item get) as soon as he is free and no text box is open. The item
// must already be in the stock. Logs "item-queue", then "item-queue-start" and "item-shown".
void RF3Items_Queue(int32_t offset);
int32_t RF3Items_QueueLength(void);

// Debug (step 22a): Link holds up this item now (GiveItemEntryWithoutActor). Returns 0 if he can't.
int32_t RF3Items_GiveWithAnimation(int32_t offset);

#ifdef __cplusplus
}
#include <string>
std::string RF3Items_ItemGetText(int32_t offset); // step 24d (RF3Messages.cpp)
#endif

#endif
