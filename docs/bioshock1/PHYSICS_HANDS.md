# BS1 physics hands: design and critique

Goal: props react to the hand and the held weapon. Melee damage stays on the swing gesture and
does not need contact (Hugh's call, 2026-11-05).

## Design

One keyframed `hkpRigidBody` in the game's own world follows the active hand. The game already
has keyframed bodies and a queue that turns a target pose into velocity and zeroes it after the
step, so we feed that queue instead of writing velocity ourselves.

- **Shape:** one capsule per holdable, laid along the rendered weapon axis (`bones::weapon_frame`).
  The table holds length and radius only, so the axis stays right for every weapon and animation.
  Anything not in the table takes the default capsule. Left hand takes the hand capsule.
- **Filter:** `(1<<16)|5`. Layer 5 hits dynamic props and ragdolls, and skips static geometry and
  pawns. Group 1 is the player's, so the player's own queries pass through.
- **Motion:** once per frame, in the bone-drive path on the game thread, outside the physics step.
  Step size is clamped (a fast flick cannot launch props at silly speed). A long jump (load,
  recenter) warps instead of sweeping through the room.
- **Lifetime:** the world pointer is re-read each frame. On any world change the body is forgotten
  with no Havok call, because the old world owns its teardown.

## Critique

| Risk | Answer in the design | Proven by |
|---|---|---|
| Door listener dereferences a body's actor property | Set property 0 to the player pawn, as the game does for actor bodies | Walk through a door with the body on |
| Queue only drains on a step, so a pause grows it | Skip the push while the body's refcount shows an undrained entry | Pause for 30 s, refcount stays flat |
| Fast hand launches props | Clamp step to 11 m/s | Swing into a prop in the headset |
| Body blocks grenades or crossbow bolts leaving the hand | Unknown. Fire each projectile weapon with the body on | If they burst at the muzzle, drop the body to a non-colliding layer while the trigger is down |
| Enemy melee or shots hit the hand | The body's actor is the player, so this reads as hitting the player | Headset |
| Sleeping props may not wake | Unknown | Move the hand into a resting prop; `vrphys near` shows it move |
| Level change, save, load, quit | Forget on world change, no Havok call. Our one reference leaks one small body per level | Load loop, save, quit with the body on |
| `hkpListShape` ownership is inferred | Not used: one capsule per weapon is enough until the headset says otherwise | |

Rejected: writing velocity ourselves (the game's queue already does this and zeroes it after the
step), and spawning an `AHavokActor` (needs a package object, and it would be saved).

## Results (simulator, GOG build, 2026-11-05)

- Body follows the hand exactly, swaps collider on weapon change, survives two loads and a save
  with zero faults. The refcount guard holds through weapon switches.
- `vrphys sweep` through a pile of 17 sleeping props 46 m away: nothing moved before the sweep, all
  of them moved within 60 ms of it. So contact works and it wakes sleeping props.
- Grenades look the same with the body at the hand and parked 100 m below (`vrphys park on`): one
  sighting each shot in both. That shows no difference, not a tracked flight, so check them in
  the headset. Crossbow bolts never appeared on the projectile layer.
- The axis must be read after the equip animation: 2 s after a switch or load gives the barrel
  axis, 0.8 s gave a mid-animation one.

Open for the headset: push strength, collider sizes, and whether the Camera and Chemical Thrower
axes (bone 44 is not obviously the barrel on those) look right.

Tools: `vrphys status|near [loose]|sweep <n>|watch|park on|off|on|off`.

Addresses are in ENGINE_NOTES, "Havok on the GOG build".
