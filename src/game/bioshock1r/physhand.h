#pragma once
// Physics hand: one keyframed Havok body follows the active hand, so props react
// to it. Design, critique and stages: docs/bioshock1/PHYSICS_HANDS.md.

#include "core/hooks/pattern_scan.h"
#include "game/bioshock1r/frame_context.h"

namespace bvr::b1r::physhand {

void init(const bvr::pattern_scan::ProcessImage& image);
void on_world_change();
// Once per frame, game thread, after the bone drive placed the hand.
void on_frame(const GamePose& gp, int hand);
// Once per CalcView, game thread: the free hand and whether its grip is held.
// Pulls the nearest loose prop to the hand and hovers it; letting go throws it
// with the hand's motion. Copies the game's own Telekinesis hold, without the
// listener that makes thrown props hurt.
void telekinesis_tick(const float handUu[3], const float dir[3], bool grip, void* rayActor);
// Where the object the grip would take sits, in UU. False when there is none.
bool telekinesis_target(float uu[3]);
void handle_command(const char* args);

} // namespace bvr::b1r::physhand
