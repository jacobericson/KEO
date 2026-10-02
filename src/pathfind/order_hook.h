// order_hook.h - Player order hook, called on the main thread.

#ifndef KEO_ORDER_HOOK_H
#define KEO_ORDER_HOOK_H

void hook_addOrderSelected(void* thisPI, void* destIndoors, int task,
                            void* subject, bool shift, bool addDontClear,
                            const float* location);

#endif // KEO_ORDER_HOOK_H
