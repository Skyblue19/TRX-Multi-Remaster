#pragma once

// A puppet replaying Lara's own recent moves a little behind her: a remote
// player stood in for by a local recording, to try the puppets without a
// network. Enabled with --coop-ghost.

// Record Lara's state for this frame and move the ghost along. Called once per
// logic frame, after Lara has moved.
void Coop_Ghost_Control(void);

// Forget the recording. The level is about to go.
void Coop_Ghost_Reset(void);
