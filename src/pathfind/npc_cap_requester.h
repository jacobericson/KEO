#ifndef KEO_NPC_CAP_REQUESTER_H
#define KEO_NPC_CAP_REQUESTER_H

// Who issues the character path searches that end at the node cap (DEV builds): the path thread
// copies each such request's owner handle into a ring, and the main thread resolves the handles
// noted since its last line into the character's name, faction, race and current task types.

#ifdef KEO_DEBUG
// Path thread, on a capped character search; request is the request being served (NULL: none).
void NpcCapRequesterNote(const void* request, int player);
// Main thread: resolves the handles noted since the last call and writes the NpcCapWho: line.
void NpcCapRequesterPrintLine();
#endif

#endif // KEO_NPC_CAP_REQUESTER_H
