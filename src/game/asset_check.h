#pragma once

namespace dvk {

// Runs every shipped asset through the validators and prints one line each.
//
// M2 validates a level's portal reciprocity and shared vertices; M3 validates
// that a mesh is closed. Both are already enforced at load, but only for the
// assets a given run happens to touch: play `mine01` and nothing ever looks at
// `test01`, and a model that no level places is never loaded at all. This walks
// the directories instead, so a broken asset is found on the next run rather
// than on the run that first needs it.
//
// Returns the number of assets that failed.
//
// In Debug a failure is fatal, because shipping a broken asset is a build error
// and the point of Debug is to refuse to continue past one. In Release a failed
// MODEL is reported and skipped: the game is better off missing a pillar than
// refusing to start. A failed LEVEL is fatal in both, because skipping it leaves
// nothing to play - see the note in the implementation.
int validateAssets();

} // namespace dvk
