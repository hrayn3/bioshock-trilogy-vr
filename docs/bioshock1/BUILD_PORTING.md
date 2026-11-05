# Running on builds other than the 2022-04-13 Steam exe

The hardcoded addresses in `patterns.h` belong to one Steam link. GOG's exe is a different
link (PE timestamp 0x5BBD3A7A, 21700512 bytes), so every address differs and no single
offset converts them.

## Rule

Derive addresses from the running image. If one cannot be proven, leave its feature off.
Never hook a target that has not passed an identity check.

## How each kind is found

| Kind | Method | Code |
|---|---|---|
| Class vtable | MSVC RTTI: name, TypeDescriptor, locator with offset 0, vtable | `find_rtti_vtable` |
| Virtual function | Rebound vtable plus the documented slot, then a shape check | `resolve_aim_natives` |
| Directly called function | Scan the class's virtuals for a call whose target has the shape | `find_ability_fire_start` |
| Name table | Writable data triple whose first entries carry their own index | `bind_gnames` |
| Engine global | Writable data slot pointing at an object with the engine vtable | `bind_engine_ptr` |

Lookups that need live objects run on demand and retry, throttled to every 2 seconds.

A newly found address can wake code that was dormant. Binding the engine global let the
console seam run, and it then called a Steam-only function RVA. After adding any lookup,
trace what it unblocks. Console entries now come from vtables, verified by the RTTI
locator's this-adjustment, or the command is refused.

## Shape check

A fire-start must end in the right `ret` size and read the documented pawn fields
(`0x1D8`, `0x450`). Without it, the old code hooked an unrelated function on GOG.

## Verified on GOG

Vtable slots `+0x304` and `+0x2FC` match the Steam layout. Weapon fire-start is at RVA
`0x69730`, ability fire-start at `0xFDF0`, reached from ability vtable slot `+0xE8`.

## Still off on unrecognised builds

`run_object_scan` refuses, so FOV control and the viewmodel drive stay off. Their callers
write through Steam field offsets. Verify each offset read-only first.
