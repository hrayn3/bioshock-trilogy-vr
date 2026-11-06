# BS1 threaded stereo: design and critique

Why: in stereo the game thread does everything, and the render queue is 66% of it (11.4 ms of a
17.5 ms pair at the Nexus spot). In threaded mono the same work sits on a second thread.

## Design

Structural 1t stays. The flush point still takes our inline branch, so the engine's event protocol
(the 0x61D38E deadlock) never runs. Only the second eye's drain changes: the flush writes the
manager fields as before, then hands the drain to one mod-owned worker and returns. The game thread
finishes the build and runs the next tick while the right eye renders.

```
now:    build L  drain L  build R  drain R  tick
new:    build L  drain L  build R  tick ...
                                   drain R (worker)
```

Every path that could meet the worker joins it first: build head, flush point, and any drain on
the game thread. Submit does not: it runs straight after the hand-off and is the engine's own
threaded-mode call. Nothing overlaps except tick and submit against the second drain.

**Ceiling.** The drain enters a render lock at its head (0x4006F4) that the build also holds, so a
build can never overlap a drain. The pair is therefore serial: build L, drain L, build R, drain R,
about 15 ms at the Nexus spot, or 66 fps. Offloading both eyes hung and gained nothing: the game
thread could retake the lock and then join a worker waiting for it (worker stalled at 0x4006FA).
Going further means making a drain or a build cheaper.

## Critique

| Risk | Why it might bite | Answer |
|---|---|---|
| Tick uses the D3D context during the drain | Immediate contexts are not thread-safe | The numerator is untouched, so game code keeps its threaded paths. Also turn on the driver's multithread protection |
| Tick changes what the drain reads | Threaded mode allows this by design, but the frame pool and second slot are unproven under inline | Soak test, look for torn hands or popping |
| Bones written by tick while the right eye draws | Viewmodel could tear | Screenshot test. If it tears, snapshot or hold the tick |
| Drain assumes the pump thread | Engine TLS or a thread id check | Fault shows on the first frame. The worker copies MXCSR and the x87 control word from the game thread |
| A wait in the drain expects the pump | Stall | Join logs when it waits over 2 s |
| Mod state assumes one render thread | Present runs on two threads, never at once | Join guarantees order |
| Load or menu during the drain | The worst case historically | Only steady gameplay offloads, the join runs at every build head |

Off by default: `reentry offload on|off|status`. Any fault poisons reentry, as today.

## Results (GOG, 2026-11-05)

On by default on the GOG build with `vrstereo on`. `reentry offload off` restores inline.

- Nexus spot, simulator: 52 to 66 stereo fps. Fake SteamVR headset: 54 to 60, which looks like that
  display's cap.
- Soak: 20 loads with weapon switching and firing, a save and focus change every 5 loads, then 4
  minutes idle. About 26,000 offloads, no fault, one stall-watchdog line during the load cycles.
- Both eyes look right in side-by-side captures. The viewmodel bone risk is untested in motion.
- A drain still running at window close faulted and raised the game's "failed to shutdown" box on
  the next launch. The teardown callback now stops offloading and joins first.
- The join serves sent messages because Present on the worker may send to the window thread.

Not proven: a real headset, a long session, busy combat scenes.

## Update, 2026-11-06: opt-in, not default

The worker no longer starts with stereo. `reentry offload on` turns it on.

- **Protection silences the inspector.** `SetMultithreadProtected(TRUE)` rewrites the function table
  held inside the context (table at ctx+4, pointer unchanged) to separate thread-safe entry points.
  The inline hooks stay on the old entries, so draw counts stop, and HUD capture and frame dumps go
  dead with them. Measured: `DrawIndexed` 2,147,638 to 2,148,093 over 6 s after protection.
- **Removing protection is not safe.** Soak without it: 20,831 D3D calls off the worker while it
  drained, one drain fault at RVA 0x587451, and the game would not close. The calls were three
  routes into one texture update at 0x4E83F0, on the game thread and a job thread.
- **Re-hooking failed.** Inline hooks on the protected entries, and writing detours into the table
  of the context seen in a draw, both killed the game at once. Writing the table of the context
  from `GetImmediateContext` before any inline hook ran, but draws never arrived.
- **In the headset the gain did not show.** The stereo pair sat at 36 on Air Link at 72 Hz with the
  worker on. Alternate-eye does not use the worker and held 72, so it is now the BS1 default.
