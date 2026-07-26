# Fable III PC — Local (Same-Console) Co-op Restoration

> ## ⚠️ This is an UNFINISHED work-in-progress / research project
>
> It is **not a finished, fully-playable co-op mod.** A second player can join, sign in, spawn,
> and **walk and fight**, but **cannot yet interact with anything that uses a "Press A" prompt**
> (shops, outfit/clothing stands, dialogue, quest objects, menus). Treat this as a technical
> proof-of-concept and a documented reverse-engineering effort, not a plug-and-play release.
> **Read [`HANDOFF.md`](HANDOFF.md) for the authoritative, detailed status** — every key
> address/global, what works, the dead ends, and the remaining problem.

Restores Fable III's **same-console shared-screen co-op** on the PC (Games for Windows
Live) port. The feature shipped compiled into `Fable3.exe` but dormant. This mod turns it
back on.

---

## Project status — what works and what does NOT (read before trying it)

**✅ Working**
- Player 2 (and, in principle, 3–4) **joins by plugging in a controller and pressing Start**.
- Player 2 **signs in silently** — no GFWL "not signed into a profile" prompt.
- Player 2 **spawns** as a real second hero on the shared, leashed co-op camera.
- **Input is separated:** controller 2 drives hero 2 (full analog movement), controller 1 drives hero 1.
- Player 2 **movement and locomotion** work independently.

**❌ Not working / not supported yet**
- **Player 2 cannot press A or use any menu/interaction.** This is the big one: no shops,
  no clothing/outfit stands, no dialogue, no quest interactions, no pause/inventory for P2.
  Root cause is understood but unsolved — the UI input listener (`NUI::CGUIInput`) is a
  **singleton bound to controller 0**; see HANDOFF §11. This makes P2 currently limited to
  walking and combat.
- **No interaction prompts appear for player 2** when player 1 isn't standing at the same object.
- **Hot-plug is not supported.** Controllers must be connected **before launch** — the game's
  pad-rescan only re-adds controller 0.
- **Only free-roam / drop-in context is tested.** Some quests, cutscenes, and menus may block a
  join or behave unexpectedly.
- **3–4 players is untested.** The design scales but only 2-player has been exercised.
- **Save data:** only the host's profile saves; there is no second-player save binding.
- This is **shared-screen** co-op (one camera, both heroes leashed together), **not**
  split-screen — Fable III never had split-screen on any platform and this mod does not add it.

**Requirements/caveats:** requires XLiveLessNess and must be launched via `PlayCouchCoop.bat`
(see below). Local only — no netcode is involved. Reverse-engineered addresses are specific to
the retail GFWL `Fable3.exe`; other builds are not supported. The third-party build dependencies
(`opus-src/`, `xlln-src/`) are **not** included in this repo — see "Rebuilding" for where to get them.

> **Terminology, up front:** Fable III's local co-op is **shared-screen** (both heroes on
> one dynamically-framed camera, leashed by `MaxCoopDistance`), *not* two-viewport
> split-screen. True split-screen never existed in Fable III on any platform — the renderer
> is single-view top to bottom (one D3D9 device, one backbuffer, one camera). So this mod
> restores couch co-op as the 360 actually had it; it does **not** and cannot add a second
> viewport without writing a new renderer.

---

## Why it's off on PC (root cause)

The entire couch-coop code path is present and internally consistent in `Fable3.exe`:

| Stage | Function | Notes |
|---|---|---|
| Join trigger (2nd pad presses Start) | `sub_B9B0C0` @ `0x00B9B0C0` | gate: `event.controllerIndex != primary` |
| Join body | `sub_BC0500` @ `0x00BC0500` | spawns hero 2 + shared-screen |
| Create 2nd hero for a controller | `sub_BBFA10` @ `0x00BBFA10` | guest sign-in + `CreatureHero` |
| Shared-screen toggle | `sub_68ED30` @ `0x0068ED30` | `SetSharedScreenModeToggle` |
| Per-hero input routing | `sub_6B6C00` @ `0x006B6C00` | hero stores its pad index at `+52` |

Two independent blocks stop it on PC:

1. **Input layer only ever creates controller 0.** The input-manager constructor
   `sub_188DE60` (`0x00188DE60`) creates exactly one Xbox 360 pad, hardcoded to XInput
   index 0 (`xor eax,eax` at `0x00188DF00`). The 360 build enumerated pads 0–3. Because
   only pad 0 exists, every input event carries source index 0 == the primary player, so
   the `!= primary` join test in `sub_B9B0C0` never fires. **This is the primary block.**
   Per-hero *routing* already works — heroes poll `XInputGetState(*(device+4))`, i.e. their
   own pad — so once pads 1–3 exist, player 2 is driven by pad 2 automatically. No routing
   patch needed.

2. **Second-player GFWL sign-in.** `sub_BBFA10` requires the joining pad's user to be
   "signed in" (`XUserGetSigninState(idx) != 0`, and the validity gate
   `sub_771D60`). GFWL on PC only ever surfaces one local profile, so a second local user
   is never signed in. We satisfy this with a small sign-in shim (see below).

The runtime enable flags are **already `= 1`** in static data (`byte_1C86C16`
"SetAllowMultiplayerJoining", `byte_1C86BDB`), so this was never a one-flag fix.

---

## The two fixes

### Fix 1 — Multi-pad enumeration (binary patch)

Replace the 59-byte single-pad create block in `sub_188DE60` with a loop over XInput
indices 0–3. Verified with IDA's assembler (all `call`/`jz`/`jb` targets resolve).

- **Address:** VA `0x00188DEDA` (RVA `0x0148DEDA`, i.e. `moduleBase + 0x0148DEDA`)
- **Length:** 59 bytes (in-place; no code cave)

**Original bytes (verify before patching):**
```
38 5D 01 74 36 8D 54 24 0C 52 53 E8 58 C0 CD FF 3D 8F 04 00 00
74 24 68 A0 00 00 00 E8 C5 56 17 00 8B C8 83 C4 04 33 C0 3B CB
74 06 56 E8 24 E8 FF FF 8B F8 8B CE E8 EB 0F 00 00
```

**Patched bytes:**
```
31 FF 8D 54 24 0C 52 57 E8 5B C0 CD FF 3D 8F 04 00 00 74 20 68
A0 00 00 00 E8 C8 56 17 00 59 89 C1 56 89 F8 E8 2D E8 FF FF 57
89 C7 89 F1 E8 F3 0F 00 00 5F 47 83 FF 04 72 C8 90
```

Disassembly of the patched block:
```
xor  edi, edi                 ; i = 0
loop:
lea  edx, [esp+pState]
push edx
push edi                      ; dwUserIndex = i
call XInputGetState           ; -> 0x01569F42
cmp  eax, 48Fh                ; ERROR_DEVICE_NOT_CONNECTED
jz   short skip               ; not connected -> next index
push 0A0h                     ; sizeof(CJoystickXBox360) = 160
call sub_1A035C0              ; operator new
pop  ecx
mov  ecx, eax                 ; this = device
push esi                      ; parent = CInputManager
mov  eax, edi                 ; EAX = i  ->  device+4 (its poll index)
call sub_188C730              ; CJoystickXBox360 ctor
push edi
mov  edi, eax                 ; EDI = device (arg to register, __usercall)
mov  ecx, esi
call sub_188EF00              ; register device with manager
pop  edi
skip:
inc  edi
cmp  edi, 4
jb   short loop               ; i < 4
nop                           ; pad; falls through to original mouse/kbd init
```

**Behavioural note:** this drops the original `a3[1]` "enable gamepad" config gate (each
pad is still gated by its own `XInputGetState(i)` connection check), which is the desired
behaviour for a co-op-enable patch.

This fix can be applied either in-memory by the DLL (recommended, update-safe) or baked
into a copy of `Fable3.exe` with any hex editor / the included checks.

### Fix 2 — Second-player sign-in shim (xlive IAT hooks)

Make a second local user appear signed in so `sub_BBFA10` proceeds without GFWL's sign-in
UI. We overwrite the game's xlive Import Address Table slots (known RVAs below) and forward
index 0 to the real implementation (XLiveLessNess).

Contract derived from the binary (`sub_7720E0`, `sub_BBFA10`, `sub_771D60`,
`XUSER_SIGNIN_INFO` reads):

| xlive export | IAT slot (VA / RVA) | Required behaviour |
|---|---|---|
| `XUserGetSigninState` | `0x01AD55E4` / `0x016D55E4` | idx 0 → forward. idx N (pad N connected) → **1** (SignedInLocally). Never 2. Must persist all session (polled every frame). |
| `XUserGetSigninInfo` | `0x01AD55E8` / `0x016D55E8` | idx 0 → forward. idx N → return 0, fill `XUSER_SIGNIN_INFO`: `UserSigninState=1`, `dwInfoFlags=0` (**GUEST bit 0x2 MUST be clear**), `xuid` = stable **non-zero unique**, `szUserName` non-empty. Same for flags 1 and 2. |
| `XUserGetName` | `0x01AD565C` / `0x016D565C` | idx 0 → forward. idx N → return 0 + non-empty name. |
| `XUserCheckPrivilege` | `0x01AD5640` / `0x016D5640` | idx N → success, granted = FALSE (offline guest has no online privileges; does not block local join). idx 0 → forward. |

> **Critical trap:** despite the code calling player 2 a "guest", the engine's validity gate
> `sub_771D60` = `(signedInLocally || liveEnabled) && !isGuest` **rejects** the Xbox GUEST
> flag. The shim must present player 2 as an ordinary **non-guest, SignedInLocally** profile
> with a unique non-zero XUID — not an Xbox guest.

`XShowSigninUI` (`0x01AD55E0`) is intentionally **not** hooked: with state faked, the guest
path never calls it, and the host still needs it at boot.

We report a guest for index N iff `XInputGetState(N)` says a pad is connected — so co-op is
"plug in a 2nd controller, press Start". This scales to 4 players.

---

## Delivery: single XLiveLessNess module DLL

Both fixes ship in one DLL (`fable3_couchcoop`), placed in `./xlln/modules/` (loaded by
XLiveLessNess after the game's imports are bound) or injected by any loader. On load it:

1. Verifies it's inside `Fable3.exe` and the original bytes at `base+0x0148DEDA` match, then
   applies the multi-pad patch in memory (`VirtualProtect` + copy). No on-disk edit.
2. Saves and overwrites the four xlive IAT slots with the sign-in hooks above.

Why XLiveLessNess (not `xliveless`): it's an actively-maintained, LGPL, complete GFWL
rewrite with a module loader; the archived GTA-IV `xliveless` is a single-user DRM stub that
can't provide a second local user. **Note:** local couch co-op uses *no* netcode
(`sub_68ED30` disables Live presence and makes the session non-joinable), so XLLN's known
"online co-op rejected" issue for Fable III does **not** affect this mod.

See `dllmain.cpp` for the implementation.

---

## What is installed (done)

Built and placed into the game folder:

```
<game>\xlive.dll                                   XLiveLessNess v1.6.0.1, built Release|Win32
<game>\XLiveLessNess\modules\fable3_couchcoop.dll  our module (exports XLLNModulePostInit @41101)
<game>\PlayCouchCoop.bat                           launcher (pins XLLN config to game folder)
```

Build workspace (kept under `CouchCoopMod\`, not needed at runtime):
`xlln-src\` (XLLN source + `bin\xlive.dll`), `opus-src\` (Opus 1.5.2, built `opus.lib`),
`dllmain.cpp` / `fable3_couchcoop.def` / `build.bat` (our module), `build-xlln.bat`.

### How XLLN loads our module
XLLN loads every `.dll` in `{configDir}\modules\` and calls its export at **ordinal 41101**
(`XLLNModulePostInit`) after the game's imports are bound — where our module applies the
input patch and installs the sign-in IAT hooks. `PlayCouchCoop.bat` passes
`-xllnconfig=.\XLiveLessNess\xlln-config.ini` so `{configDir}` is the game folder (XLLN
auto-creates the `.ini` on first run). Without the launcher, XLLN would instead use
`%LOCALAPPDATA%\XLiveLessNess\` and you'd put the module in that `modules\` folder.

### Run
Launch **`PlayCouchCoop.bat`** (not FableLauncher) so the module is picked up.

### Uninstall / revert
Delete `<game>\xlive.dll`, the `<game>\XLiveLessNess\` folder, and `PlayCouchCoop.bat`.
Nothing in `Fable3.exe` on disk was modified — the input patch is applied in memory at runtime.

### Rebuilding
- Our module: run `CouchCoopMod\build.bat` (VS 2022 BuildTools, x86).
- XLLN: `CouchCoopMod\build-xlln.bat` (retargets to SDK 10.0.26100 + toolset v143; needs
  `opus.lib` at `xlln-src\xlivelessness\third-party\opus\bin\Win32\Release\`, built from
  `opus-src\`). One upstream fix was required: add `#include <chrono>` to
  `xlln-src\xlivelessness\xlive\xnetqos.cpp` (newer toolset no longer includes it transitively).

---

## Test plan

1. Baseline: run under XLiveLessNess, host signs in normally, single-player works.
2. Install the module. Launch, load a save, be in normal free-roam (drop-in co-op context).
3. Plug in controller 2, press **Start** on it.
   - Expect: guest sign-in is silent (no GFWL UI), a second hero (`CreatureHero`) spawns,
     shared-screen mode engages (`sub_68ED30`), the second-player HUD appears
     (`MoneyAlertSecondPlayer.*`).
4. Verify **independent control**: move on pad 2 only — hero 2 moves, hero 1 doesn't.
   Confirms per-pad routing (`event+40 == 1` → hero with `+52 == 1`).
5. Verify the leash / camera framing and quitting the guest (`GUI_SCREEN_PAUSE_HENCHMAN_QUIT`).

## Open runtime-validation risks (confirm by test)

- **Co-op context gates.** Beyond input, join is gated by `sub_674340` / `sub_674320` and
  `byte_1C86BDB` — the game's "co-op allowed here/now" flags. Normally true in free-roam
  drop-in co-op; some contexts (certain quests/menus) legitimately block a join.
- **Manager per-frame device iteration.** The 4 devices register identically in the
  manager's listener list (`mgr+12/+16`); confirm each pad emits `event+40 == its index`
  live (step 4 above).
- **Hot-plug.** The pad-0 rescan `sub_188DCF0` still only re-adds index 0; pads 1–3 must be
  connected before/at the create loop, or extend the rescan similarly (not required for a
  first working build).
- **XLLN forwarding.** Confirm index-0 xlive calls forwarded to XLLN behave identically to
  unhooked (host sign-in/save binding unaffected).

## Address reference (imagebase 0x400000)

Input: `sub_188DE60` `0x00188DE60`, patch site `0x00188DEDA`, `sub_188C730`
(CJoystickXBox360 ctor, user index at obj+4) `0x00188C730`, `sub_188EF00` (register)
`0x00188EF00`, per-frame poll `sub_188C7B0` `0x00188C7B0`.
Join: `sub_B9B0C0` `0x00B9B0C0`, `sub_BC0500` `0x00BC0500`, `sub_BBFA10` `0x00BBFA10`,
`sub_6B6C00` (idx→hero) `0x006B6C00`, `sub_68ED30` (shared-screen) `0x0068ED30`.
Sign-in: `sub_7720E0` `0x007720E0`, gate `sub_771D60` `0x00771D60`.
Flags: `byte_1C86C16` `0x01C86C16`, `byte_1C86BDB` `0x01C86BDB`,
`dword_1C86BE4` (primary pad) `0x01C86BE4`, `dword_1C86BE8` (guest pad) `0x01C86BE8`.
