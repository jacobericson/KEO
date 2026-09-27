// order_hook.h - Player order hook, called on the main thread.

#ifndef KENSHI_ZONE_OPT_ORDER_HOOK_H
#define KENSHI_ZONE_OPT_ORDER_HOOK_H

void hook_addOrderSelected(void* thisPI, void* destIndoors, int task,
                            void* subject, bool shift, bool addDontClear,
                            const float* location);

#endif // KENSHI_ZONE_OPT_ORDER_HOOK_H
