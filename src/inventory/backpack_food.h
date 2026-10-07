// backpack_food.h - The ground-food score post-hook: an AI that would walk to food on the ground
// while its worn backpack holds food it can eat scores the walk at 0.
#ifndef KEO_INVENTORY_BACKPACK_FOOD_H
#define KEO_INVENTORY_BACKPACK_FOOD_H

namespace keo_inventory {

// Main thread, a startup install step: the row while backpackFoodScore is on.
void InstallBackpackFood(int* installed, int* total);
// Any thread: the food and dialogue counters, for the jobs heartbeat.
void BackpackFoodCountersRead(long* foodZeroed, long* dialogCalls, long* dialogBackpackHits);

} // namespace keo_inventory

#endif
