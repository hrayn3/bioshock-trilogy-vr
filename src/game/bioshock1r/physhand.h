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
void handle_command(const char* args);

} // namespace bvr::b1r::physhand
