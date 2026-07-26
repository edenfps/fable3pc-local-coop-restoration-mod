# Fable III PC — Local Couch Co-op Restoration — HANDOFF

**Read this first.** This is the authoritative, current state of the project. Companion files:
`dllmain.cpp` (the runtime module), `build.bat`, `README.md`. A large amount of trial-and-error is
distilled below — **read §5 (dead ends) and §9 (the real remaining problem) before trying anything.**

---

## 0. Goal & premise

Restore Fable III's **same-console local co-op** on the PC (GFWL) port. The 360 had it; the PC
port shipped it disabled/partially-stripped.

**Framing:** Fable III local co-op is **shared-screen** (both heroes on one dynamically-framed
camera, leashed by `MaxCoopDistance`), **not** two-viewport split-screen. The renderer is
single-view (one D3D9 device, one backbuffer, one camera) — do not look for split-screen.

**Current status (stable milestone, verified this session):**
- ✅ **Player 2 signs in cleanly with NO prompt.** The old "not signed into a gamer profile /
  try again" warning is gone. (This was the previous handoff's "core unsolved problem." Solved —
  see §3.)
- ✅ **Player 2 spawns** with the correct shared camera.
- ✅ **All co-op state is correct:** hero 2 bound to controller/device 1, coop-mode gate open,
  split-screen input flags on, 2nd-player pad field bound. (See §4.)
- ✅ **INPUT IS FULLY SEPARATED.** Controller 2 drives hero 2 and only hero 2; controller 1 still
  drives hero 1. Fixed by a two-instruction patch in `sub_D89DE0` — see §9. **§9's old "recommended
  fix" (demultiplexing the queue around each player's pump) was NOT needed and was not used.**
- ✅ **Player 2 movement works** (analog stick, full 360° control, independent of player 1).
- ❌ **Player 2 cannot press A / use menus.** This is **NOT an input problem** — it is measured,
  end-to-end, that pad 2's button events reach hero 2 correctly (see §11). The blocker is that
  `NUI::CGUIInput` — the listener that actually consumes UI/"Press A" input — **is a singleton
  bound to controller 0**. **See §11 before touching anything here.**

---

## 1. Environment

- **IDA Pro MCP server** — check `server_health` first. Must be pointed at the **PC binary**
  `Fable3.exe.i64` (imagebase `0x400000`, 32-bit x86). The 360 `.xex` builds are PowerPC with ZERO
  symbols — not a shortcut; work in the PC binary.
- **RVA convention everywhere: `RVA = VA − 0x400000`.** The module reads/patches at
  `runtimeBase + RVA` (`runtimeBase = GetModuleHandleW(NULL)`; ASLR-safe).
- **Game folder:** `E:\C Drive SSD stuff 10-20-24\Microsoft Games\Fable III - Backup`
- **Build:** from PowerShell in `CouchCoopMod\`: `& cmd.exe /c ".\build.bat"` (the leading `.\`
  matters; the `vswhere`/`vcvars` "not recognized" lines are noise — look for `EXITCODE=0`).
  Output `fable3_couchcoop.dll` (x86, ~93 KB).
- **Deploy to BOTH** (XLLN loads the LOCALAPPDATA one; game must be CLOSED or the copy fails
  "file in use"):
  - `%LOCALAPPDATA%\XLiveLessNess\modules\fable3_couchcoop.dll`  ← the one XLLN loads
  - `<game>\fable3_couchcoop.dll`
- **Diagnostics:** the module writes **`<game>\couchcoop.log`**. Read it after each test. Key tags:
  `[init]`, `[patch]`, `[hook]` (sign-in), `[trigger]`/`[join]` (spawn), `[signin]` (slot state +
  populate), `[coop]` (gate/context fields), `[gate]` (`sub_673CB0`), `[devid]` (each hero's bound
  device), `[route]` (per-frame pad→hero latch correlation), `[hang]`/`[crash]` (dumps).

---

## 2. What's built & installed

- **`fable3_couchcoop.dll`** — our XLLN module (exports `XLLNModulePostInit @41101`). Built from
  `dllmain.cpp`.
- **XLiveLessNess v1.6.0.1** — built to `<game>\xlive.dll`. **XLLN sign-in works**; it auto-signs
  Eden (user 0) and Liah (user 1). Player 2 = XLLN **user index 1 ("Liah")**.
- Run via **`PlayCouchCoop.bat`**.

---

## 3. THE SIGN-IN FIX (this session's big win — how player 2 became a real local user)

The PC build's sign-in slot sync **`sub_772190`** is **hardcoded to user index 0**: it calls
`XUserGetSigninInfo(0, …)` and writes the result into **slot 0** (`*(mgr+20)`). It is the ONLY
function that populates the slot table (called from init `sub_772ED0` and the `XNotify`
sign-in-change handler `sub_772430`). There is **no per-index version** — multi-user sign-in was
stripped. So controllers 1–3 never get a slot, and **`sub_771D60(mgr, 1)` always returns false** →
the join shows the "not a gamer profile" warning and never treats player 2 as a real local player.

**The gate `sub_771D60(mgr, idx)`** = `(slot[0] || slot[1]) && !slot[2]`, where
`slot = *(mgr+20) + 32*idx`. `mgr = *(dword_1DC2550 + 40)` (i.e. `sub_6A6960(sub_6A66C0())`).

**Our fix (`PopulateSigninSlot(idx)` in `dllmain.cpp`)** drives the game's OWN `sub_772190` to
populate slot `idx` — so the game does its own std::string/xuid handling (no fragile manual struct
faking):
1. Build a **fake manager** where only the two fields `sub_772190` reads are set:
   `fake[5] = &slot[idx]` (the "+20" slot base) and `fake[6] = &slot[idx] + 32` (the "+24" end, so
   the internal size check == 1).
2. Set a scoped flag so **`Hook_XUserGetSigninInfo` remaps `idx 0 → idx` on the populating thread**,
   making `sub_772190`'s hardcoded `XUserGetSigninInfo(0,…)` fetch controller `idx`'s XLLN profile.
3. Call `sub_772190(fake)`. Result: real `slot[idx]` populated exactly as the game would
   (`b1=1` SignedInLocally, `guest=0`, valid name ptr + Liah's xuid `0x880f7bba`).

Verified in-log: `[signin] idx1 b0=0 b1=1 guest(b2)=0 -> 771D60=1  data=b0 ae 60 24 ba 7b 0f 88`.
We run this in `RunDeferredJoin` **before** the join, so `sub_BBFA10` skips the warning and runs its
full native co-op path.

---

## 4. The join & co-op state (VERIFIED correct this session)

Pressing Start (pad 1) → `Hook_DropIn` queues → `RunDeferredJoin(1)` (on the game's drop-in/input
thread) does, in order:
1. **`PopulateSigninSlot(1)`** (§3) → player 2 is a real signed-in local user.
2. **Set `coopObj[60] = coopObj[61] = 1`** (the split-screen-input-active flags; see below).
3. **Set `byte_1C86BDB = 1`** (local-coop-active; ungates the native per-player bind `sub_752CD0`).
4. **`sub_BC0500(1)`** → `sub_BBFA10` → spawns hero 2 (`CreatureHero`, `+52 = 1`) + runs the native
   co-op input bind.
5. **Fallback:** if `context+0x168` (2nd-player pad idx) is still -1, set it to 1.

**Co-op state objects (all confirmed set correctly at runtime):**
- **`owner = *(dword_1DC2550)`** (`sub_6A66C0`). `coopObj = *(owner + 16)` (`sub_6A68A0`).
- **`sub_673CB0()` = `coopObj[0xB8]`** = coop-mode / local-player-count. **==1 gates the player-2
  input pump.** Confirmed `= 1` after join.
- **`sub_6A8780()` = `coopObj[60] && coopObj[61]`** = split-screen input active. Gates
  `sub_74F1D0` setting `context+0x168`. Confirmed `= 1`.
- **Input context** the per-frame pump operates on: **`context = *(*(dword_1DBDD4C + 8) + 0xA8)`**
  (`sub_673BD0(sub_673C50())`). **`context+0x168` = 2nd player's controller index** (-1 = none).
  Confirmed `= 1` after join.
- Hero device binding (`sub_912C10`: type-37 control-scheme component → joystick `+0x14` → XInput
  idx `+0x34`): **hero1 dev=0, hero2 dev=1.** Correct.

**The two-player pump EXISTS and is understood** (`sub_753010` and its coop twin `sub_753A80`):
processes **player 1** (`sub_6B7310` = `sub_6B7290(1)`, controller `*(player1+52)`) always, and
**player 2** (`sub_752E80`, controller `context+0x168`) when `sub_673CB0()==1`. `sub_752E80` applies
via `sub_7521F0(context+4, *(player2+48), context+0x168, …)`. All its preconditions are met — yet
hero 2 still receives nothing, because the input it reads is already merged (§9).

Pump-path selection (`sub_753EC0`, keyed on context flags `+688/+690/+699`): `+690 && sub_6A8780()`
→ `sub_753A80` (2-player); `+688` → `sub_7536B0`→`sub_753010` (2-player); else →
`sub_7533B0/sub_753590` (single-player refresh). **Which path runs in gameplay is not yet confirmed
— worth instrumenting next (see §9).**

---

## 5. DEAD ENDS / CORRECTIONS (do NOT repeat)

- **CORRECTION to the old handoff: `byte_1C86BDB = 1` does NOT crash.** The old notes marked it a
  crash; the logs disprove that — every run that set it reached `[heroes] count=2 +52=1` and
  returned. The old `[hang]` dumps were all the **dismissable sign-in warning box** (`sub_7718B0`,
  EIP in ntdll wait), not a fault. It is now set every join with no ill effect.
- **Patching `sub_771D60`/`sub_7718B0` to force "signed in"** — removes the warning but crashes the
  join (player object half-initialized). Superseded entirely by §3 (populate the real slot instead).
- **`XLLNLogin` auto-login from the module** — collides with the manual guest sign-in → crash.
- **CECPlayerControl field-poke** (`DriveHero2FromPad1`, writing hero2's `+0x0C/+0x10` each frame):
  moves hero 2 briefly but the component pointer goes stale on entity teardown → heap corruption;
  and it only gives movement, never combat/UI. Kept as dead code. Not a real solution.
- **D3D Present-thread join** — blocks rendering during the (former) sign-in box. The join runs fine
  on the drop-in/input thread (`Hook_DropIn`); keep it there.
- **Assets/scripts do NOT gate co-op** — exhaustively searched. Local co-op is 100% binary-driven.

---

## 6. Key addresses (PC `Fable3.exe`, VA; RVA = VA − 0x400000)

### Sign-in
| Addr | What |
|---|---|
| `sub_772190` (RVA `0x372190`) | **Sign-in slot sync — HARDCODED to user 0 → slot 0.** We drive it via a fake mgr to populate slot N (§3). |
| `sub_771D60` | is-signed-in gate: `(slot[0]||slot[1]) && !slot[2]`, `slot=*(mgr+20)+32*idx`. 30+ callers. |
| `sub_6A66C0` | `return dword_1DC2550` (RVA `0x19C2550`). Sign-in mgr = `*(dword_1DC2550+40)`. |
| `sub_772ED0` / `sub_772430` | init populate / `XNotify` sign-in-change handler (both call `sub_772190`). |
| IAT `XUserGetSigninInfo` slot | RVA `0x16D55E8` (we hook it; also used for the populate redirect). |

### Co-op state / gates
| Name | Formula | Meaning |
|---|---|---|
| `owner` | `*(dword_1DC2550)` | root singleton (`sub_6A66C0`). |
| `coopObj` | `*(owner+16)` | co-op state object (`sub_6A68A0`). |
| `sub_673CB0()` | `coopObj[0xB8]` | coop-mode / local-player count. **==1 gates player-2 pump.** |
| `sub_6A8780()` | `coopObj[60] && coopObj[61]` | split-screen input active. Gates `context+0x168` set. |
| `byte_1C86BDB` | RVA `0x1886BDB` | local-coop-active; gates native bind `sub_752CD0`. We set =1. |
| `context` | `*(*(dword_1DBDD4C+8)+0xA8)` | the input context the pump uses (`sub_673BD0`). |
| `context+0x168` | — | **2nd player's controller index** (-1 = none). Set by `sub_74F1D0`. |
| `dword_1C86BE4` | RVA `0x1886BE4` | primary/active controller idx (=0). |
| `dword_1DBDD4C` | RVA `0x19BDD4C` | session/game-mgr singleton (`sub_673C50`). |

### Join / spawn / pump
| Addr | What |
|---|---|
| `sub_BC0500` | native local join `__cdecl(ctrlIdx)`; we call it. → `sub_BBFA10` (×2) + `sub_68ED30`. |
| `sub_BBFA10` | build 2nd hero + sign-in checks + native bind. Bind gate `byte_1C86BDB` @`0xBBFB3A`; `sub_752CD0` @`0xBC045D`; `sub_752E80` @`0xBC046A`; `sub_74F1D0` (sets `context+0x168`) @`0xBBFDC3`. |
| `sub_68ED30` | `SetSharedScreenMode`; runs AFTER the bind (so it can't help the bind — we set the flags ourselves first). |
| `sub_752CD0` | native per-player input-context **bind** → `sub_6B6C00(mgr,idx)` → binds `hero+64`. |
| `sub_74F1D0` | sets `context+0x168 = ctrlIdx` **iff `sub_6A8780()`**. |
| `sub_753010` / `sub_753A80` | per-frame input pump; player 1 always, player 2 (`sub_752E80`) iff `sub_673CB0()==1`. |
| `sub_752E80` | **player-2 pump**: needs `context+4`, `*(context+8)>0`, `sub_6B7320`, `context+0x168!=-1`; applies `sub_7521F0(context+4, *(player2+48), context+0x168, …)`. |
| `sub_753EC0` | pump dispatcher; picks path by `context+688/+690/+699`. |
| `sub_6B7290(n)` | get local player n. `sub_6B7310`=player1, `sub_6B7320`=player2. |
| `sub_6B6C00(mgr,idx)` | hero whose `+52==idx`. |
| `sub_912C10` | `GetJoystickDeviceID`: type-37 comp `+0x14` → `+0x34`; null → -1. |

### Input events (the merge layer — §9)
| Addr | What |
|---|---|
| `sub_188DE60` | `CInputManagerDX` pad-enum ctor. **Multi-pad patch @`0x188DEDA` / RVA `0x148DEDA`** (creates a `CJoystickXBox360` per XInput pad 0–3; retail made only pad 0). |
| `sub_188C730` | `CJoystickXBox360` ctor; XInput poll index stored at **device+4**. |
| `sub_188C7B0` | per-frame poll: `XInputGetState(*(device+4))` → `sub_188C110`. |
| `sub_188C110` | processes raw pad state into events; **stamps each event with `*(device+4)` (the pad's index) at `event+0x28`.** So events ARE tagged per-controller. |
| `sub_5CA020` | pushes a 72-byte event into a per-device queue. |
| `sub_188A1F0` | device event dispatch (device+0x28 listener). **Prime suspect for the merge/drain — start here (§9).** |

### CECPlayerControl (movement component, entity type 134; vtable `0x1b460d4`)
- Accumulator (input in): `+0x0C` (x) / `+0x10` (y). Latched each frame by `sub_104D230` (vtable
  slot 20) → LastControlDirection `+0xC0` / `+0xC4` (read via `sub_104D380`), then cleared.
- Component fetch (ECS): `*( *(entity+0x58) + 8*(*(BYTE*)(*(entity+0xA8)+typeId)) + 4 )`.
- Hero entities: `mgr→+0xC→+0x1C→+0x4→+0x4→holder` (`+0xEC` hero1 / `+0xF4` hero2) `→*`.
- Type 37 = control-scheme component (holds the joystick pointer at `+0x14`).

---

## 7. Assets / scripts (exhaustively checked — nothing to enable here)

Game data is in banks (`*.bnk` + `*.bnk.dat`, zlib). Scripts are compiled (no `.lua`). They contain
the **online orb co-op** system and **couch *interaction*** logic but **no** co-op-enable /
controller-assignment / control-gating tunable. Local co-op is 100% binary-driven. Config files
(`startup.vfsconfig`, `Fable3.exe.cfg`, `keyMenu4Pt_v2.cf`, `dir.manifest`) have no co-op toggle.

---

## 8. What the module does RIGHT NOW (current stable build — `dllmain.cpp`)

- **Multi-pad enumeration patch** @ RVA `0x148DEDA` (the one on-disk-style in-memory binary patch).
- IAT-hooks the xlive sign-in exports (pass-through + logging), plus `Hook_XUserGetSigninInfo`
  carries the **scoped populate redirect** (§3).
- IAT-hooks `XInputGetState` for Start-edge detection on pads ≥1 → queues `g_pendingJoin`.
- Hooks the drop-in dispatch callback (`sub_B9B0C0` ptr at data `0x1b1a26c`, RVA `0x171A26C`) →
  `Hook_DropIn` runs `RunDeferredJoin(idx)` on the game's own thread.
- **`RunDeferredJoin(idx)`** (§4): `PopulateSigninSlot(idx)` → set `coopObj[60]/[61]=1` → set
  `byte_1C86BDB=1` → `sub_BC0500(idx)` → ensure `context+0x168`.
- **Diagnostics:** `Watch` (join globals + hero vector), `RouteWatch` (`[route]`/`[devid]`/`[signin]`
  /`[gate]` — read-only pad→hero correlation), `JoinWatchdog`, VEH `CrashHandler`.
- **Helpers:** `PopulateSigninSlot`, `GetCoopObj`, `GetInputContext`, `HeroDeviceId`,
  `GetHeroEntity`/`GetComponentOf`. Dead code kept: D3D Present hook, `DriveHero2FromPad1`,
  sign-in gate patches.

---

## 9. THE INPUT-LAYER MERGE — ✅ SOLVED

> **Status: fixed.** Everything below documents how the merge was found and what the fix is. The
> "Recommended fix: demultiplex the shared queue" subsection at the end is **historical only** — it
> was never needed. The actual fix is the two-instruction patch documented immediately below.

### THE FIX (two instructions in `sub_D89DE0`'s per-event loop)

`sub_D89DE0(ctx, rawEventList)` buckets every queued event by controller and then matches each
bucket against the registered bindings. Retail did this with `ebp == 0`:

```
d89e70   mov [ecx+28h], ebp      ; wipe the event's source-pad tag to 0
d89e7c   mov [esp+20h], ebp      ; file the event under bucket key 0
```

Meanwhile `sub_B6DD20` dispatches **per listener** using that listener's own controller index
(`entity+0x34`), so hero 1 found bucket 0 and worked while hero 2 asked for bucket 1 and always got
an empty bucket. Patched to:

```
d89e70   mov edx, [ecx+28h]      ; edx = the event's real source pad     (RVA 0x00989E70)
d89e7c   mov [esp+20h], edx      ; bucket key = that pad                 (RVA 0x00989E7C)
```

`edx` is dead across `d89e66..d89e8c`; `ebp` must stay 0 because it is still the zero comparand for
the `cmp` at `d89e88`. Apply the bucket-key patch **first** so a partial application can never leave
tags preserved but keys hardcoded. Both sites are original-byte guarded in `ApplyBytes`.

**Result:** pad 2 drives hero 2 only; pad 1 drives hero 1 only; movement is fully independent.

---

### Historical: how the merge was diagnosed

**Symptom:** controller 2 drove hero 1 **and the menus**, even before player 2 existed. Hero 2
received nothing.

**Why the menu behavior is the smoking gun:** menus have no players/heroes/co-op. If pad 2 drives
the menu, the game's input is merged at a layer **below** the player/hero/co-op machinery — which we
have now fully and correctly set up (§4). So the fix is NOT more co-op state; it's the input
manager folding all connected pads into one logical input stream (a common single-player-port
simplification: "any controller works"). The two-player pump (`sub_753A80`/`sub_752E80`) reads from
that merged stream, so it can't separate players no matter how correct its per-controller indices
are.

**What's already proven:**
- Events **are** tagged with the correct per-pad controller index (`sub_188C110` writes
  `*(device+4)` into `event+0x28`), and each device polls its own XInput index.
- So the separation data exists at the event level; it's **lost during delivery/consumption** — the
  consumer applies all events to the primary/active input regardless of `event+0x28`.

### Input API trace (DONE — negative result, do NOT redo)

Full API map of `Fable3.exe` (all three devices are created in `sub_188DE60`, the input-manager ctor
we patched; its config bytes gate them: `a3[0]`=mouse, `a3[1]`=gamepad *(gate our patch removed)*,
`a3[2]`=keyboard):

| Device | API | Class | Evidence |
|---|---|---|---|
| **Gamepads** | **XInput 1.3 — EXCLUSIVELY** | `NInput::CJoystickXBox360` | `XInputGetState/GetCapabilities/SetState` (XINPUT1_3); polled as `XInputGetState(*(device+4))` in `sub_188C7B0` |
| Keyboard | **DirectInput8** | `NInput::CKeyboardDX` (`sub_188E190`) | `DirectInput8Create` @`0x188DED5` → `CreateDevice(GUID_SysKeyboard)` → `SetDataFormat` → buffered 256 → `Acquire`; fills a DIK table. `IDirectInput8*` stored at **inputMgr+100** |
| Mouse | **Raw Input** | `NInput::CMouseRaw` (`sub_188DB60`) | `RegisterRawInputDevices` **usUsagePage=1, usUsage=2**; `sub_188D710` handles only `dwType==0` (`RIM_TYPEMOUSE`) |

`riidltf` @`0x1BC9440` = `BF798030-483A-4DA2-AA99-5D64ED369700` = **`IID_IDirectInput8A`**.
`GetAsyncKeyState` is referenced once (`0x1975854`), not part of the gamepad path.

**Consequences (important):**
1. **There is NO second gamepad API.** DirectInput = keyboard only; Raw Input = mouse only. The
   "another API is merging the pads" hypothesis is dead — don't re-investigate it.
2. **We own the entire gamepad input surface** via the `XInputGetState` IAT hook (RVA `0x16D5508`);
   `dwUserIndex` identifies the polling device. The log confirms the game polls index 0 AND 1 as
   separate devices.
3. Therefore the merge is **purely internal to `NInput`'s event→binding layer**, downstream of
   devices that are already correctly separated and correctly tagging events.

### ⚠ MAJOR CORRECTION — `sub_74xxxx`/`sub_75xxxx` IS THE SAVE SYSTEM, NOT INPUT

**Do not treat that cluster as the input pump.** It is `CSaveLoadManager`. Proof: calling
`sub_752E80` per-frame made the game's **"Saving…" indicator** appear continuously in-game. The
strings in those functions (`"HeroSave"`, `"SaveName"`, `"JourneyName"`, `"Header"`,
`"MiscSystems"`, `CSaveLoadManager::CContentPackageHandle`,
`"CSaveLoadManager::WriteSaveGame >> COMPLETE."`) are REAL, not Hex-Rays RTTI confusion as an
earlier revision of this document claimed.

Corrected meanings:
| Symbol | WRONG (earlier) | ACTUAL |
|---|---|---|
| `sub_752E80` | "player-2 input pump" | **save player 2's hero** |
| `sub_753010` / `sub_753A80` | per-frame input pump | save-request processing |
| `sub_753EC0` | input dispatcher | per-frame save-request dispatcher |
| `sub_7533B0` / `sub_753590` | single-player input pump | save helpers (a2=1 → player 1, a2=0 → player 2) |
| `sub_752CD0` | "native per-player input bind" | save/profile container bind |
| `context+0x168` | 2nd player's input pad | 2nd player's controller **for profile/save** |
| `sub_673CB0` / `sub_6A8780` | input pump gates | save/coop-state gates |
| `sub_771D60` | gate on input | **gate on SAVING** (need a profile to save to) |

Consequence: `[pump2]` never firing was NOT evidence that "player 2 is never pumped for input" —
it just meant player 2's hero was never being saved. The real input consumer was never in this
cluster. **Do not re-investigate `sub_74xxxx`/`sub_75xxxx` for input.**

### THE MERGE POINT — FOUND (`sub_188EFB0`)

`CInputManagerDX` vtable slot 2 → `sub_188DDF0` (= rescan `sub_188DCF0` + **`sub_188EFB0`**).
`sub_188EFB0` is the per-frame poll driver:

```c
for ( i = v1[3]; i != v1[4]; i += 2 )        // every device in the list (mgr+12 .. mgr+16)
    (*(*(*i) + 8))( *i, v1 + 11, time );     // device->Poll(queue) ; queue = v1+11
```

**`v1 + 11` == `inputManager + 44` — ONE shared event queue passed to EVERY device.** Pad 0, pad 1,
`CKeyboardDX` and `CMouseRaw` all push into that single stream (same queue reused by the
`sub_5CA020(v11, this + 44)` call later in the same function). **This is the merge.**

**Critically, the per-pad tag SURVIVES.** `sub_188C110` stamps `event+0x28 = *(device+4)`, and
`sub_188A1F0` explicitly propagates it when generating derived events (`0x188A2F0`:
`mov ecx,[ebp+28h]` / `mov [eax+28h],ecx`). Event stride = **72 bytes (0x48)**; queue is a plain
contiguous `[begin,end)` array. Event layout: `+0x08` code, `+0x0C` value(float), `+0x28`
**source controller index**, `+0x2C` type (0x0D/0x0E/0x0F/9/10/11/12/15…), `+0x34` value2,
`+0x40` timestamp(double).

So NO separation data is lost — the downstream binding matcher simply never consults `event+0x28`
when matching a player's bindings. That is precisely why pad 2 drives player 1 **and the menus**.

### Recommended fix: demultiplex the shared queue around each player's pump

Because the tag survives and the queue is a flat 72-byte array, we do NOT need to understand the
deep binding matcher. Filter the queue in place around each pump:
- Before player 1's input runs → present only events with `event+0x28 == 0`.
- Before **`sub_752E80`** (player-2 pump; already confirmed reached with `context+0x168 == 1`) →
  present only events with `event+0x28 == 1`.
- Restore the queue afterwards.

This also fixes the **menu** case for free, since the same queue feeds the UI. Requires a real
**trampoline/code hook** on `sub_752E80` / `sub_753010` / `sub_753A80` (they are not imports, so the
IAT-hook technique used elsewhere in the module does not apply).

*(Superseded hypothesis: "only controller 0's binding namespace is loaded." Still possible as a
secondary factor, but the shared-queue merge above is confirmed and is the direct cause.)*

**Recommended next steps:**
1. **Confirm which pump path runs in gameplay** (read `context+688/+690/+699` per-frame in
   `RouteWatch`). If the single-player `sub_7533B0/sub_753590` else-branch runs, forcing the
   two-player path (`sub_753A80`, gated by `context+690` + `sub_6A8780()`) is step one — but it will
   NOT fix separation alone (both heroes would then read the same merged input).
2. **Find the event drain / consumer.** From `sub_188A1F0` (per-device dispatch, `device+0x28`
   listener) follow where 72-byte events are drained into the game input state / input contexts.
   Determine where `event+0x28` (controller idx) is (not) used to pick a destination context.
3. **Route by controller:** make device-N's events reach controller-N's `"%d"`-keyed input context
   (`sub_752BF0`/`sub_7527B0` build `"%d"` keys from the controller number), instead of merging onto
   controller 0. That single change should give player 1 = pad 0 only, player 2 = pad 1 only, for
   movement + combat + UI simultaneously.
4. Beware: several input leaves (`sub_74E070`, `sub_7503C0`) decompile as garbage because IDA's
   Hex-Rays confuses their code with save-game/`CSaveLoadManager` routines (shared inlined helpers).
   Use `disasm` and trace the `NInput` binding-value read (`sub_18AB910` / `sub_75FA80` and the
   `(*(reader+16))(…)` virtual read) rather than trusting the pseudocode.

**Fallback (lower value):** robust game-thread field-poke of hero 2's CECPlayerControl accumulator
from pad 1 (movement only, no combat/UI, and doesn't fix the menu merge). Not recommended.

---

## 11. THE CURRENT REMAINING PROBLEM — player 2 can't press A / use menus

> **Do not treat this as an input bug.** It was measured end-to-end and input is provably correct.
> Everything in this section is empirical, from `couchcoop.log` runs with the census/gate-trace
> builds. That instrumentation is preserved in `dllmain.gatetrace.bak.cpp` (file toggle:
> `nocensus.txt` next to `Fable3.exe` disables it).

### The dispatch architecture (verified)

`sub_B6DD20` is the per-frame input update. Per frame it:
1. `sub_D89DE0(ctx, rawList)` — buckets raw 72-byte events by pad, matches them against every
   registered binding, and emits 64-byte **action records** per pad.
2. For each listener: `sub_D89480(ctx, padIdx, out)` copies that pad's surviving actions into a
   40-byte list, then `sub_B6CF30(listener, out)` hands them to `listener->vtbl[4]`.
   `padIdx` comes from the listener's entity via `sub_6B6F60(id)->+0x34` (0 = hero 1, 1 = hero 2,
   −1 = listener with no entity).

`sub_D89480` **skips** an action whose contributing event is already marked consumed
(`event+0x38 != 0`, checked at `d89522`), which is how an earlier listener suppresses later ones.

### What was measured (all confirmed, do NOT redo)

- **Raw events exist for pad 1**: `[raw] pad1: t13=9 t14=27 t15=9` (13 = press, 15 = release,
  14 = hold, 11/12 = sticks). Button events are tagged with the pad exactly like stick events
  (`sub_188BFE0` writes `device+4` into `event+0x28`; `sub_188C110` does the same for sticks).
- **The binding matchers are pad-aware.** `CInputTypeXboxPadButtonEvent::Match` (`sub_1358850`) is
  `event->type == expected && event->pad == ctrl && event->button == expected`;
  `CInputTypeXboxPadLeftStickEvent::Match` (`sub_13588E0`) is `type == 11 && pad == ctrl`.
  Neither hardcodes pad 0.
- **Action streams are identical.** For the same button, `[act] pad0` and `[act] pad1` list the
  same ids with the same counts (e.g. `4d 5d 38 18 19 0d 40 5f 67 80 8a a6 90 92 1a 4e`).
- **Hero capability words are identical.** `[caps]` shows hero 1 and hero 2 with byte-identical
  `+40/+44/+48/+55/+56/+60/+64/+172`, same control-scheme component, same permission word.
- **Devices are registered symmetrically**: hero 1 `device+0x30 = 1, +0x34 = 0`;
  hero 2 `device+0x30 = 2, +0x34 = 1`.

### The listener roster (the actual finding)

Resolved from vtable pointers logged per pad. Hero 1 has **11** listeners, hero 2 has **9**:

| Listener (vtable VA) | hero 1 | hero 2 |
|---|:--:|:--:|
| `CInputProcessCharacterMovement` `0x1b644bc` | ✓ | ✓ |
| `CInputProcessInteract` `0x1b64478` | ✓ | ✓ |
| `CInputProcessCombat` `0x1b64430` | ✓ | ✓ |
| `CInputProcessMagicCharge` `0x1b6440c`, `StickJabDetection` `0x1b64454`, `Release` `0x1b6451c` | ✓ | ✓ |
| `CSpellChargingDefenceManager` `0x1b556bc`, `CECPlayerCombatAbilityManager` `0x1b5978c` | ✓ | ✓ |
| `CPlayerModeControlEntitySimple` `0x1b2d37c` | ✓ | ✓ |
| **`NUI::CGUIInput` `0x1b1a254`** | ✓ | ✗ |
| **`CCameraModeScript` `0x1b28a0c`** | ✓ | ✗ |

`CCameraModeScript` being absent is expected and harmless — co-op is shared-camera.

**`NUI::CGUIInput` is a SINGLETON.** `sub_B9C760` (the UI subsystem constructor) allocates exactly
one (`sub_B9AB30`, 72 bytes) and stores it at `*(this+3)`. Its ctor sets `*(this+0x34) = -1`
(controller index). There is no per-player instance. This is the blocker: the listener that
consumes UI and "Press A" input exists once, bound to controller 0.

### `CInputProcessInteract` is a dead end (proven, do NOT chase)

`sub_1355000` fires on action id 13 then gates on
`E+172&2 → E+44&0x20 → comp37 → sub_9134F0(comp37,3) → E+55&1`.
`sub_9134F0` is `mov ecx,[ecx+14h] ; jmp sub_BBE560` — it runs on the control scheme's **device**,
and `sub_BBE560` does `sub_BFDDA0(globalObj, key = device+0x30, 3)`, a registry lookup.

**Both heroes fail that check** (`fail@g5`), including hero 1 — which can interact perfectly well.
So this processor is not the "Press A" path in the early game at all; the UI path (`CGUIInput`)
consumes the event first and hero 1's `CInputProcessInteract` usually never even sees action 13.
Registering player 2's device for ability 3 would fix nothing.

### The raw-event path — mapped, and why it is NOT the answer either

`sub_B6DD20` runs TWO passes per listener: a **raw** pass (`sub_B6D020`, delivers 72-byte events to
`listener->vtbl[0x18]`) and then the **action** pass (`sub_D89480` + `sub_B6CF30`, delivers 40-byte
action records to `listener->vtbl[4]`).

`sub_B6D020` contains a genuine single-player hardcode:

```
b6d076  call sub_6B7310        ; GetPlayer(1)
b6d07f  cmp dword ptr [esi+24h], 1   ; event came from a pad
b6d085  mov ecx, [esi+28h]     ; the event's source pad
b6d088  cmp ecx, [eax+34h]     ; player ONE's controller index
b6d08b  jnz -> skip this event
```

It only applies when `sub_674320()` or `sub_71C410()` is true; otherwise the filter is bypassed
entirely (`jz loc_B6D08D`). A **null** return from `sub_6B7310` also bypasses it.

**But the raw path is near-useless for gameplay:** `vtbl[0x18]` is `nullsub_4601` on *every*
`CInputProcess*` class (verified on Interact and CharacterMovement). **`NUI::CGUIInput` is the only
listener in the roster with a real raw handler** (`sub_B9B0C0`). So the raw path exists to feed the
UI, and nothing else.

### Three fix attempts — all no-ops, none regressed player 1 (do NOT repeat)

| # | Change | Result | Why it failed |
|---|---|---|---|
| A | Append pad-1 **action** records to the singleton `CGUIInput`'s list (hook at `call sub_B6CF30`, `0xB6DE8C`) | no effect | `CGUIInput::Handle` (`sub_B9BFD0`) returns immediately at `if (!sub_71C410() \|\| *(this+8))` when no UI **screen** is open; both of its action paths (`sub_BBD510` → screen, `sub_D78BA0` → widget) need one. Confirmed feeding: `[uimerge] CGUIInput fed pad-1 actions (+6 records)`. |
| B | Make `sub_B6D020`'s pad filter **per-listener** (stand-in object whose `+0x34` is the listener's own controller) | no effect | Delivered pad-1 raw events to hero 2's input processes, whose `vtbl[0x18]` are all `nullsub_4601`. |
| C | Return **null** `GetPlayer(1)` for `CGUIInput` only, so its raw path accepts every pad | no effect | Confirmed live (`[rawfix] CGUIInput raw path unfiltered`). Still nothing for player 2. |

All three are preserved in `dllmain.gatetrace.bak.cpp` behind file toggles (`uimerge.txt`,
`norawfix.txt`). **Player 1 was verified unaffected in every case.**

### ⚠ Correction to the g5 claim above

The "`CInputProcessInteract` is a dead end" verdict is **provisional, not proven**. Both heroes fail
its g5 gate, but `sub_BBE560`'s guard (`sub_6E2C50(sub_673F70(sub_673C50()))->vtbl[8]()`) is
**global state**, so "both fail" is equally consistent with ability 3 being unavailable during the
castle intro — where the measurements were taken and where the player cannot yet attack. Re-measure
later in the game before writing this path off.

### What is still unknown (the actual open question)

**Nobody has identified what performs hero 1's "Press A" interaction.** It is not:
the per-pad action bucket (identical for both heroes), the `CInputProcess*` action handlers
(`CInputProcessInteract` is *skipped* — 23 calls vs 30 — in exactly the frames interact input
flows), or the raw-event path as filtered. The census showed an unidentified 12th listener
appearing intermittently on pad 0: **`vt=0x1b10604`** — start there.

Suggested method: stop reasoning from the listener list and find the consumer **empirically** —
guard-page or hardware breakpoint on the interaction actually firing (e.g. the
`CAbilityPlayerInteract` queue in `sub_7F3A50`), then walk the call stack back to whatever fed it.
That technique already found the tag-wipe in §9 when static analysis was going in circles.

### If you attack this next

The only real route is giving controller 1 a UI input path. Honest assessment: **large and
crash-prone.** `CGUIInput` is wired into a singleton UI subsystem (menus, overlays, focus, cursor);
a second instance needs its own focus/selection state or it will fight player 1's. Consider first
whether the 360 build actually gave player 2 full UI, or only world interaction — that changes the
target substantially.

---

## 12. DEEP RE SESSION — the architecture, and the REAL root cause

> This section is the product of a long, dedicated reverse-engineering pass. It supersedes the
> guesswork in §11's "if you attack this next". **Read it before doing anything.**

### 12.1 The two input destinations (why only movement works)

There are **two** places input can land. Player 2 reaches one of them:

| Destination | Drives | Player 2? |
|---|---|---|
| Entity/gameplay listeners (`CPlayerModeControlEntitySimple`, ability system) | movement, ability firing | ✅ **reaches hero 2** (proven) |
| **`NUI::CGUIInput` singleton** (`0x1b1a254`) | pause/Sanctuary, all menus, interaction UI, HUD nav | ❌ **controller 0 only** |

`CGUIInput` is a **singleton** — `sub_B9C760` allocates exactly one (`sub_B9AB30`, 72 bytes) and stores
it at `*(this+3)`. `sub_B9BFD0` is its action handler; `sub_B9B0C0` its raw handler. The pause menu
(`sub_B9B900`, opens "Guild"/Sanctuary) is called from **exactly one place** — `sub_B9BFD0` — and
operates on `sub_6BCB80` (the **primary** hero). That is why player 2 cannot press Start.

### 12.2 The hero model

- `sub_6BCB80(entityMgr)` = `*(*(mgr+0xEC))` = **primary hero** (player 1)
- `sub_6BCBA0(entityMgr)` = `*(*(mgr+0xF4))` = **henchman** (player 2)

Our hero 2 IS correctly registered as the henchman, has all 69 components, and resolves to
controller 1 in the entity→player map. **Player registration is not the problem.**

### 12.3 The interaction protocol (message-driven, and NOT henchman-hostile)

`CECCharacterInteraction` (`0x1b2533c`) subscribes via `sub_A1AA30` (in `sub_CB4230`) to message ids
**367–377, 566, 567**. Message **367** = `CRequestCharacterInteractionPacket`; its handler
`sub_CB2F80` → `sub_CB0F70` starts the interaction with **whatever hero the message names** — it does
**not** gate on primary-vs-henchman. So the interaction system structurally accepts the henchman.

The A-press chain, **all of which fires correctly for hero 2**:
```
button -> CPlayerModeBase::HandleAction (sub_1121F70, shared by ~30 CPlayerMode* vtables) case 13
       -> CAbilityPlayerInteract (0x1b21754, CanStart = `return 1`)
       -> sub_7F3A50 (gate sub_7F3210 -- hero 2 PASSES it)
       -> CAbilityActionPlayerInteract::Perform (sub_13CBCF0, vtbl+0xC of 0x1b30bfc)  ret=1
       -> posts GameAction(category 3, hero, NPC, 101) via sub_9F8AB0
```

### 12.4 ⚠ COOP OBJECT FLAGS — semantics, and a CRASH TRAP

`coop = *(dword_1DC2550 + 16)`.

| Field | Meaning | Notes |
|---|---|---|
| `coop[60]`, `coop[61]` | coop enabled | set by `sub_684650` |
| **`coop[62]`** | **"player 2 is a REAL profiled user with save/content package"** | ☠ **DO NOT SET — see below** |
| `coop[148]` | coop-active (`sub_683610`) | set **indirectly**; was **0** in our sessions |
| `coop[178]` | coop mode / camera (`sub_6840C0`) | was **already 1** — not the gap |
| `coop[194]` (0xC2) | online session (`sub_674320`) | stays 0 for local |

Derived predicates:
```
sub_6A8760() = coop[60] && coop[62]
sub_6A8780() = coop[60] && coop[61]
sub_6A87A0() = coop[60] && coop[62] || coop[148]      <- "coop active", read by HUNDREDS of funcs
```

**☠ THE `coop[62]` TRAP (cost a crash — do not repeat):** setting `coop[62]=1` makes `sub_6A8760()`
true, and `sub_6A8760` is checked inside **`sub_7521F0`**, which is the
`CSaveLoadManager` / `CContentPackageHandle` path. The game immediately tries to load/commit
**player 2's save data**, which does not exist for our synthetic guest → **"Saving…" then CRASH.**
This is precisely why `sub_684650` explicitly sets `coop[62] = 0`.

`sub_684650(coop)` is the real coop activation (sets 60/61/62 = 1/1/0, calls `sub_6840C0(1)`, flips a
global). **Its guard only runs while `coop[60] || coop[61]` is still 0** — calling it after our flag
poke is a silent no-op. `sub_BBCCD0` is the higher-level "enter local coop" (checks `sub_683610`,
then calls `sub_684650`).

### 12.5 THE REAL ROOT CAUSE

**Player 2 is not a real *user* to this engine — it is a hero entity wearing a fabricated sign-in
slot.** `PopulateSigninSlot` drives `sub_772190` against a fake manager plus a scoped
`XUserGetSigninInfo` redirect. That is enough to spawn a *body*, which is why movement and ability
firing work. It is **not** enough for any system that treats player 2 as a *person*:

- **save/profile** — no content package (`coop[62]` proves this, violently)
- **coop-active** (`sub_6A87A0`) — false, and hundreds of interaction/UI/hero functions gate on it
- **UI/pause/menus** — the `CGUIInput` singleton + primary-hero binding
- **interaction presentation** — fires logically, presents nothing

This is upstream of every individual binding, and it is why no per-binding patch has ever landed.

### 12.6 Dead ends — ALL measured, none regressed player 1. DO NOT REPEAT.

| # | Attempt | Result / why it failed |
|---|---|---|
| 1 | Append pad-1 **action** records to the `CGUIInput` singleton | `sub_B9BFD0` returns immediately at `if (!sub_71C410() \|\| *(this+8))` when no UI **screen** is open |
| 2 | Make `sub_B6D020`'s pad filter per-listener | `vtbl[0x18]` is `nullsub_4601` on **every** `CInputProcess*`; only `CGUIInput` has a real raw handler (`sub_B9B0C0`) |
| 3 | Null `GetPlayer(1)` so `CGUIInput`'s raw path accepts all pads | confirmed live, no effect |
| 4 | **GameAction mirror** — repost hero 2's `sub_9F8AB0` event attributed to hero 1 | no effect ⇒ **the GameAction is a side channel, not the visible trigger** |
| 5 | Ability-gate override at `sub_7F3210` | never triggered — hero 2 already **passes** that gate |
| 6 | `sub_684650` at join | **no-op** (guard short-circuits; `coop[60]/[61]` already 1) |
| 7 | `coop[62]=1` | ☠ **"Saving…" + CRASH** (see 12.4) |

Also re-confirmed: **`sub_752E80` / `sub_7521F0` / `sub_74xxxx` are the SAVE / content-package system,
not an input pump.** (This has now misled two separate efforts. See §5.)

Corrections to earlier sections: §11's "`CInputProcessInteract` is a dead end" is **true but for the
wrong reason** — both heroes fail its `g5` gate, and it is simply not the Press-A path at all.

---

## 13. NEXT MISSION — make player 2 a REAL second local user

This is the sanctioned next effort, chosen by the project owner. The thesis below is the single most
valuable output of §12; everything else is supporting detail.

### 13.1 The thesis (test this first)

> **`coop[62] = 1` is the GOAL STATE, not a bug.** It means "player 2 is a real profiled user."
> It crashes *today* only because our player 2 has no profile/save/content package behind it.
> **Give player 2 a genuine user identity + save, and `coop[62]=1` becomes legal — which flips
> `sub_6A8760()` and `sub_6A87A0()` (coop-active) true, and that is the master switch hundreds of
> interaction/UI/hero functions are waiting on.**

If that thesis holds, this stops being "patch each binding" and becomes one coherent fix.

### 13.2 Concrete investigation order

1. **Find exactly what player 2's content package is missing.** During the join,
   `sub_BBFA10` @ `0xBC0434` runs (local-coop branch):
   ```
   if (!session_is_online() && byte_1C86BDB && !hero_preexisted)
       sub_752CD0(idx);  sub_752E80(1, 0, 1);
   ```
   `sub_752CD0` → `sub_7521F0` → `sub_7503C0` / `sub_74C410` / `sub_74C5D0` / `sub_74AB30`.
   Instrument these (read-only, per §12.7 safety rules) and log which one fails and with what.
   `sub_7521F0` early-outs unless `sub_747BF0`/`sub_18B11C0` returns **1 or 5** — find what that is.
2. **Investigate XLiveLessNess multi-user.** `CouchCoopMod/xlln-src` is present. Determine whether
   XLLN can sign in a **second real local user** on controller 1. If yes, `sub_7720E0` / `sub_771D60`
   succeed natively and `PopulateSigninSlot` (our fake) can be deleted entirely. This is the
   cleanest possible route and should be evaluated **before** any deeper patching.
3. **The save side.** Fable III co-op player 2 uses their own hero save. Find the "create a new
   second-player hero/save" path (the 360 flow offers this on join). If it exists in the PC binary,
   drive it instead of faking the slot.
4. **Only then** set `coop[62]=1` (or better: let the game set it via `sub_C30940`, which is the
   engine's own setter, reached by vtable dispatch) and re-test every binding.

### 13.3 Verification ladder (in order — do not skip)

1. Player 2 spawns, movement still works, **no "Saving…"**, no crash.
2. `sub_6A87A0()` returns **true** (log `coop[60]/[62]/[148]`).
3. Player 2 **Start** opens a menu. ← the cleanest single proof
4. Player 2 **A** at an NPC does something visible.
5. Player 1 completely unregressed.

### 12.7 Hard-won safety rules — violating these has cost FOUR crashes

- **Never trust Hex-Rays' calling convention when the call site contradicts it.** `sub_B6CF30`
  decompiles `__stdcall(a1,a2)` but its call site does `mov ecx, esi` first.
- **Never dereference game objects during the spawn frame.** A half-built input process holds
  *garbage* in `this+12`, not null — `<= 0x10000` guards sail straight past it. Use the
  `PtrOk` (VirtualQuery) + SEH + `g_probeQuietUntil` pattern in `dllmain.gatetrace.bak.cpp`.
- **Anything touching `sub_6A8760` / `coop[62]` can wake the SAVE system.** Assume any flag you
  flip may engage save/profile code. Test with a throwaway save.
- **Put every behaviour change behind a file toggle** next to `Fable3.exe` so it can be disabled
  without a rebuild (`nocoopfix.txt`, `nomirror.txt`, `norawfix.txt`, `nocensus.txt`, …).
- **Each test costs the owner a game launch.** Design one build to answer as much as possible, and
  prefer read-only measurement over speculative fixes. The log cannot see the screen — **ask the
  owner what happened visually** when that is the deciding fact.

---

## 15. `coop[148]` — the joined-player counter (build 36)

This section **supersedes the `coop[62]` thesis in §13**. Same goal (coop-active TRUE), different and
much safer lever, and it explains the build-35 crash exactly.

### 15.1 What the three coop predicates really are

```
sub_683770(coop) = (coop[60] && coop[62]) || coop[148]      "co-op is ACTIVE"
sub_6A87A0()     = sub_683770 on the singleton              <- read by hundreds of funcs
sub_6835F0/6A8760() = coop[60] && coop[62]                  <- READ BY THE SAVE SYSTEM
sub_6A8780()     = coop[60] && coop[61]                     <- "co-op enabled/lobby"
```

`coop[148]` reaches coop-active **without** reaching `sub_6A8760()`. That is the whole trick.

### 15.2 coop[148] is the engine's own joined-player count

| Site | What it does |
|---|---|
| `sub_68F950` case 0 (`ClientJoinConfirmPacket`, msg 239) | `v21 = ++coop[148]; coop[157] = 1;` then **rejects the join if `v21 >= 0x80`** |
| `sub_68EF70` (player leave / teardown) | `if (coop[148]) coop[148] -= 1;` |
| `sub_683FA0` (session activate) | `++coop[148]; coop[60]=1; coop[62]=1; coop[61]=0; coop[157]=1` |

So `coop[148]` = **how many other players have joined**. Setting it to 1 states the literal truth for
a local guest, and it is the engine's own representation — not a hack value.

Note `sub_683FA0` (the real activation) sets `coop[61] = 0` and `coop[62] = 1`, the **opposite** of
`sub_684650` (`61=1, 62=0`). Those are two different states: `sub_684650` = "co-op enabled/lobby",
`sub_683FA0` = "co-op session live". Our build was stuck permanently in the first one.

### 15.3 Why build 35 crashed — confirmed by reading, not guessing

`coop[62]=1` makes `sub_6A8760()` true. `sub_7521F0` (`CSaveLoadManager::CContentPackageHandle`)
does `if (sub_6A8760() && (!*this || !*(*this+4))) sub_1A01270(a2);` — it goes after player 2's
content package, which our synthetic guest has none of. Hence "Saving…" then the fault.
**`coop[148]` is not in that expression.** `coop[62]` must stay 0 until player 2 has a real save.

### 15.4 The cost of coop[148] != 0, and the fallback

`sub_686DE0` (targeted send) and `sub_687640` (broadcast) gate on `coop[148]` / `sub_683770` and
enqueue onto two `std::list`s at `coop+292` and `coop+320` (torn down by `sub_69BBC0` inside
`sub_68EF70`). They are members, so they should be constructed even with no session — build 36
**validates the four list heads (`coop+292/312/320/340`) at join and logs them** before writing
`coop[148]`. If they don't validate it falls back to patching `sub_683770` itself:

```
RVA 0x283770   80 79 3C 00 74 06   ->   B8 01 00 00 00 C3   (mov eax,1 / retn)
```

Same coop-active result; the packet bus stays shut because it reads `coop[148]` directly.

### 15.5 Corrections to earlier sections

- **§13's "`coop[62]=1` is the goal state" is wrong as a next step.** It is a goal state only *after*
  player 2 has a content package. `coop[148]` is the correct lever now.
- **`sub_C30940` is not "the engine's own coop[62] setter" in any useful sense.** It is
  `void f(this){ this[62] = 1; }` with **zero xrefs** — an unreferenced/vtable-only stub. Don't build
  on it.
- **`sub_B9B900` (pause/Sanctuary) is not a player-2 blocker.** In `sub_B9BFD0` it sits inside
  `if (sub_674320())` — an *online-session* branch. Local play never reaches it. The earlier note
  that `sub_B9BFD0` "returns immediately at `if (!sub_71C410() || *(this+8))`" also reads it
  backwards: that condition **wraps** the whole body, and the body runs when `sub_71C410()` is false.
- **The sign-in slot array is per-index but has exactly one writer, and it is hardcoded.**
  `sub_772190` always does `XUserGetSigninInfo(0, …)` into `slot[0]`, while every reader
  (`sub_771D60(idx)`, `sub_7720E0(idx)`) is per-index and the array holds 4 entries. The PC port
  simply dropped multi-user sync. `sub_7720E0(idx, panes, flags)` is a real per-index
  "sign this user in" path that calls `XShowSigninUI` and waits — but the notification handler
  `sub_772430` (XN_SYS_SIGNINCHANGED = notif 10) then re-syncs **slot 0 only**. This is exactly why
  `PopulateSigninSlot` exists; it is a workaround for a hardcoded index, not for a missing profile.
  XLLN already has a genuine second local user (`XLIVE_LOCAL_USER_COUNT` = 4, `XLLNLogin` @41140,
  log shows `XUserGetSigninInfo(idx=1) -> OK user='Liah'`). Generalising `sub_772190` to loop
  `i = 0..3` would delete the fake-manager hack — but it is **hygiene, not the button fix**: the log
  already shows `[signin] idx1 b0=0 b1=1 guest=0 -> 771D60=1`, i.e. the slot is already correct.

### 15.6 Interaction path facts worth keeping

- `sub_13CBCF0` (`CAbilityActionPlayerInteract::Perform`) special-cases the henchman at the top:
  `if (a2 == sub_6BCBA0(entMgr) && sub_6A8760()) → SendHenchmanInteractingPacket`. Gated on
  `sub_6A8760()`, so it never fires for us — but it is a *sync* packet, meaningless with no peer.
- The visible effect is `sub_9F8AB0(3, &v66, a2, v17, 101, 0)`, which the prior session confirmed
  already fires for hero 2.
- `sub_912C20(comp37) = *(comp37+20)` = the hero's joystick pointer, and
  `sub_9134F0(comp37, 4) = comp37[5] ? sub_BBE560(4) : 0`. **Hero 1's is NULL, hero 2's is real** —
  so these two branch *differently* per hero. `sub_BBE560`/`sub_9134D0`/`sub_BBE840` all require
  `sub_6E2C50(...)->vtbl[8]` (a scripted-sequence manager) to be active, so they are inert in normal
  gameplay — but if step 3 of the ladder passes and step 4 still fails, **this asymmetry is the next
  thing to look at.**

---

## 16. THE SCRIPT-RULES ROOT CAUSE — player 2's buttons, SOLVED (mostly)

**Player 2 now has: menus (Start), sprint, magic, weapon draw.** Confirmed in game. What remains is
NPC/object *interaction*, and §16.6 says exactly why it is a different problem.

### 16.1 The root cause

Player 2's hero carried **script-rule restrictions** that player 1's did not:

```
hero1 ruleword = 00000000      (none)
hero2 ruleword = 001D0080      (bits 7, 16, 18, 19, 20)
```

Read through the game's own `sub_AA1510`, one bit at a time — a clean bitfield, not a garbage read.

Script rules are Fable III's gameplay-lockout system (the thing that stops you acting during
cutscenes). The manager is `sub_658E80(world)`; its script API is registered in `sub_AA64E0`
(`AddScriptRules` → `sub_AA5B70`, `RemoveScriptRules` → `sub_AA5CB0`, `RemoveAllScriptRules` →
`sub_AA5DF0`, plus `EntitiesExcludedFromNoInteractionRule`, `SetClientInSpectatorMode`).

The ability dispatcher refuses **every** ability for a restricted entity:

```
sub_7F3A50(entity, ability):
    if (!sub_7F3210(entity)) return 0;
    sub_7F3660(&handler, entity, ability, params);   // data-driven rule match
    if (handler) { ret = 1; if (!byte_1DC9D4C) (*(handler+12))(handler, entity, ...); }
```

and `CanPerform` (e.g. `sub_13CC4B0`) bails on `sub_AA1510(entity, 0x40400)` — bit 18 (`0x40000`) is
in hero 2's word. Movement is not ability-gated, which is exactly why it alone kept working.

**Not** `AddRemoteHeroJoiningScriptRules` — `sub_AA6060` applies mask `0x200000`, which hero 2 lacks.
Where the five bits come from is still unknown; they are applied from game script.

### 16.2 THE FIX (in place, default ON, `norulefix.txt` disables)

Rule queries about hero 2 answer with **hero 1's** rules. Mirroring, not clearing — during a real
cutscene hero 1 is restricted too, so hero 2 still is. Applied by repointing **all 66** `sub_AA1510`
call sites (`RepointAllCallsTo`, §16.5).

### 16.3 coop-active via `coop[148]`, NOT `coop[62]` (default ON, `nocoopactive.txt` disables)

```
sub_683770(coop) = (coop[60] && coop[62]) || coop[148]     "co-op active"  (== sub_6A87A0)
sub_6A8760()     =  coop[60] && coop[62]                   <- READ BY THE SAVE SYSTEM
```

`coop[148]` is the engine's own **joined-player count** (`sub_68F950` increments it on join confirm,
`sub_68EF70` decrements on leave). Setting it to 1 reaches coop-active **without** reaching
`sub_6A8760()`, which is what made build 35 crash: `sub_7521F0` (`CSaveLoadManager::
CContentPackageHandle`) branches on `sub_6A8760()` and went looking for player 2's nonexistent save.
**`coop[62]` must stay 0 until player 2 has a real save.** Verified live: `[coopnow] … 148=1 ->
coop-active=1 savegate=0`, no crash.

### 16.4 Where the menu gate was

`sub_7D7000(playerKey, …)` — the "can this player open a menu" predicate — ends with:

```c
if (!sub_6A87A0() && a1 != 1 && !flag) return 1;   // refuse: not player 1 and co-op inactive
```

So menus needed coop-active (§16.3) *and* player 2's actions reaching the UI (`uimerge.txt`).

### 16.5 `RepointAllCallsTo` — the tool that made this practical

Scans the game's executable sections for every `E8`/`E9` whose rel32 already resolves **exactly** to
a target function, and repoints them. Only rewrites operands that provably point at the intended
function. Validated against IDA: 66 sites for `sub_AA1510`, 10 for `sub_CB44A0`. Prefer this over
prologue trampolines (which have caused crashes here) when a function has many call sites.

### 16.6 WHAT REMAINS: player 2 gets no interaction PROMPT

Measured, in order:

| Finding | Consequence |
|---|---|
| `[ixstart]` empty for both heroes; target `+76 & 0x20 == 0` | `CAbilityActionPlayerInteract::Perform` (`sub_13CBCF0`) is **not** the mechanism — both heroes leave at the same early `return 1`. Several earlier sessions (and this one) chased it for nothing. |
| `[requestix]` all `hero1`, even for player 2's presses | `sub_CB44A0` = `CECCharacterInteraction::RequestInteraction`; the HUD resolves the component from the **primary hero**. |
| `[requestix]` empty while the outfit stand worked | `sub_CB44A0` is not the only entry either — different interactables use different HUD handlers. |
| Both heroes have the component (`typeId=302`) | Hero 2 is not missing machinery; it is never **chosen**. |
| **The prompt only appears when player 1 is near the object** | The decisive one. The target is bound to hero 1 by the per-frame **prompt/candidate scan**; by press time it is already too late. |

**So the remaining work is in the interaction candidate/prompt scan, not in the button path.** Find
what decides "the player can interact with X" each frame and make it consider hero 2. Note the action
record is copied to a fixed scratch address before dispatch, so provenance must be by **order**
(`g_pad1FirstIdx`), never by pointer.

`ixfix.txt` (opt-in, off by default) enables a `sub_6BCB80` (GetPrimaryHero) swap during pad-1
actions. It fires correctly but buys nothing, for the reason above. Left in because it is likely a
*necessary* part of the eventual fix, just not a sufficient one.

### 16.7 Also outstanding

- **Player 2 spawns with no weapon/equipment**, and both heroes share an appearance. Same root as
  everything in §13: player 2 has no profile or save of its own. The unsheathe ability now fires
  correctly — there is simply nothing in hand.

---

## 17. TOOLING — two binaries, headless, no IDA GUI

`ida-multi-mcp` is installed and is now the way to work. Both databases open headless in ~15s.

```
idalib_open("E:\\Xenia\\Games\\fable 3 dump\\default.xex.i64")                       -> 360 build
idalib_open("E:\\C Drive SSD stuff 10-20-24\\Microsoft Games\\Fable III - Backup\\Fable3.exe.i64")  -> PC build
list_instances()   # confirm both; ids change per session, pass instance_id on every call
```

| Build | Base | Notes |
|---|---|---|
| Xbox 360 `default.xex` | `0x82000000` | PPC, 56,536 funcs, 24,134 strings, already decompressed/analysed |
| PC `Fable3.exe` | `0x400000` | x86; **RVA = VA − 0x400000** |

Install details (already done, recorded in case it must be redone):
- package installed into IDA's own Python (`pythoncore-3.14-64`), which is also `python` on PATH
- `idapro` (idalib bindings) installed from a **copy** of `C:\Program Files\IDA Professional 9.3\
  idalib\python` — installing in place fails with "could not create idapro.egg-info: Access is denied"
- `IDADIR` set persistently to the IDA install dir
- the `ida-multi-mcp.exe` shim hangs with no output; drive it as `python -m ida_multi_mcp`
- `py_eval` is **not** exposed on idalib sessions. Use the structured tools (`find_regex`, `decompile`,
  `xrefs_to`, `disasm`, `survey_binary`). If you truly need IDAPython, open a GUI instance.

### 17.1 THE FINDING THAT REFRAMES THE PROJECT

Same regex (`ScriptRules|CharacterInteraction|SpectatorMode|RemoteHero`) on both builds:

- **360: 24 matches. PC: 40+, a strict superset.**
- Present in the PC build and **absent from the 360 build**: `RemoteHeroJoining`,
  `RemoteHeroDogJoining`, `AddRemoteHeroJoiningScriptRules`, `RemoveRemoteHeroJoiningScriptRules`,
  `.?AVCPlayerModeSpectatorMode@@`, `.?AVCNetModifyScriptRulesPacket@@`,
  `.?AVCNetSetSpectatorModePacket@@`

**The PC port is not a stripped-down 360 build.** It is a later revision carrying at least as much
co-op machinery — PC shipped months after 360. So nothing needs porting from the 360 binary; the
co-op code is all present in the PC build and simply is not reached.

That makes the owner's own hypothesis the working thesis, now evidence-backed rather than intuition:

> *"we're truly not initializing this as a co-op game correctly"*

Use the 360 build as a **reference for the call path**, not as a source of missing code.

### 17.2 Why not just play it on Xenia

Genuinely on the table and the owner may take it — the 360 build has native local co-op, so if Xenia
renders the game it is strictly better than anything this mod can reconstruct. The only open question
there is playability (a known black-shader issue), not whether co-op works. If the owner reports
Xenia renders, **stop and say so** rather than continuing to burn their launches.

---

## 18. THE GAME-MODE WORD — `coop + 0xB8` (build 37, awaiting first test)

### 18.1 Name corrections — three sessions used wrong names for these

The co-op script API table is registered in **`sub_6A6A70`**, and it names the predicates outright:

```
IsInMultiplayer  = sub_6A8AB0    (mode == 1) || IsInLiveGame
IsInLiveGame     = sub_6A87A0    sub_683770(coop) = (coop[60] && coop[62]) || coop[148]
IsInCouchGame    = sub_6A8A90    *(DWORD*)(coop + 0xB8) == 1
IsClient         = sub_6A8760    coop[60] && coop[62]
IsServer         = sub_6A8780    sub_6835D0(coop)
GetMultiplayerGameMode = sub_6A87D0   *(DWORD*)(coop + 0xB8)
```

So, correcting §16.3:

| Called it | Actually is |
|---|---|
| `sub_6A87A0` = "co-op active" | **`IsInLiveGame`** |
| `sub_6A8760` = "the save gate" | **`IsClient`** |

**This finally explains the build-35 crash properly.** Setting `coop[62]` did not "wake the save
system" — it made `IsClient()` return TRUE, so the game believed it was a *network client* and the
content-package system went looking for a host's data that does not exist. The rule "never set
`coop[62]`" still stands, but for the correct reason.

It also means our current state is: `IsInLiveGame = TRUE` (via `coop[148]`), `IsInCouchGame =
FALSE`. **The engine thinks this is an online session, not a couch session.**

### 18.2 The mode word

`coop + 0xB8` is a game-mode enum; **1 = couch**. Accessors:

```
sub_673CB0()        GetGameMode  -> *(DWORD*)(coop + 0xB8)      (jmps to the folded [ecx+0B8h] getter)
sub_673C90(mode)    SetGameMode  -- __stdcall(int), retn 4, disasm-verified
sub_6839F0          the raw __thiscall setter underneath it
```

**Exactly one place in the whole binary writes it:** `sub_BBFA10` @ `0xBC00FA` calls
`sub_673C90(1)`, guarded on the freshly spawned hero entity:

```c
if ( v49 && (*(BYTE*)(v108 + 172) & 2) != 0 ) {
    sub_673C90(1);                       // <- become a couch game
    ...
    sub_AA0BD0(sub_658E80(world));       // <- and touch the SCRIPT RULES manager (§16.1)
}
```

Verified by scanning every store encoding to disp32 `0xB8` across `.text` directly from the PE on
disk. **Do not use the MCP `find_bytes` on these idalib sessions — it is broken**, it returns zero
matches for patterns that provably exist (checked against `0x6839E0`, which literally is
`8B 81 B8 00 00 00`). `py_eval` is also unavailable. The disk scanner is
`scratchpad/pescan.py` / `pescan2.py`.

### 18.3 Why this is the prime suspect for everything that remains

The engine branches on `GetGameMode() == 1` **specifically when handling the second hero**:

```c
sub_6BCC20:  if (entity == secondHero) {
                 if (GetGameMode() != 1) { ...remote-player path... }
                 else                    { ...couch path (sub_6B7320)... }
             }
```

With mode 0, hero 2 is processed as a **remote** hero. That is consistent with all three symptoms
at once: the remote-hero script rules it carried (`001D0080`, §16.1), couch-partner targeting never
considering it, and no interaction prompt.

The engine's own vocabulary for player 2 is **"couch partner" / "henchman"** — a search term nobody
had tried:

```
IsHeroWithinInteractionDistance     sub_812640 -> sub_6BCB80 = hero at mgr+0xEC   (primary)
IsHenchmanWithinInteractionDistance sub_812670 -> sub_6BCBA0 = hero at mgr+0xF4   (couch partner)
SetClientOrCouchPartnerCanTarget    sub_80F080 ] "Targeted" component, flag at +44
ClientOrCouchPartnerCanTarget       sub_80EA50 ]
IsInCouchGame / CNetSetClientOrCouchPartnerTargetablePacket / m_CanCouchPlayerEnterGUIld
```

Both hero accessors already return non-null for us (the mod reads `0xEC`/`0xF4` today), so the
machinery is there and addressable — the question is only whether the engine is in the mode that
uses it.

### 18.4 What build 37 does

Default ON, `nocouchmode.txt` disables:

- calls `sub_673C90(1)` **before** the join (the engine sets it mid-join; setting it earlier also
  suppresses `sub_684620`'s online-activation path, which is guarded on `mode != 1`)
- logs the mode either side of the join — `[couch] game mode N -> M`, `[couch] post-join game mode`
- adds `mode[B8]` and `IsInCouchGame` to every `[coopnow]` tick, so a later reset would be visible

**Safety argument:** `+0xB8` does not feed `IsClient` (`sub_6A8760`), so this cannot repeat the
build-35 crash. Every `mode == 1` branch found does strictly *less* work — `sub_684620` and
`sub_684A20` skip online activation, `sub_10761F0` skips the interaction-mode camera focus.

`coop[148]` is deliberately left at 1 as well: the menu gate `sub_7D7000` needs `IsInLiveGame`, so
clearing it would regress the menus that now work. Online + couch is a legitimate combination in
this engine, not a contradictory state.

### 18.5 The measurement this run makes

`[couch] post-join game mode` is the one line that matters, and it is informative either way:

| Reading | Meaning |
|---|---|
| pre-join `0 -> 1`, post-join `1` | We are now genuinely a couch game — a state the game has never been in for us |
| post-join `0` with the fix ON | Something in the join actively **clears** it; a new and specific bug |
| pre-join already `1` | The join was already setting it; couch mode is a dead end, look elsewhere |

To get the engine's unaided answer, drop `nocouchmode.txt` next to `Fable3.exe` and read
`[couch] post-join game mode` — that is a clean control run.

### 18.6 RESULT of build 37 — couch mode is real but NOT sufficient

Measured in game:

```
[couch] game mode 0 -> 1 via sub_673C90(1): IsInCouchGame() now TRUE
[couch] post-join game mode = 1  (IsInCouchGame=TRUE, couchmode fix ON)
[coopnow] ... IsInLiveGame=1  IsClient=0  mode[B8]=1 IsInCouchGame=1     (holds all session)
```

So the mode really was `0` — the engine was **not** treating this as a couch game, and now it does.
It holds for the whole session with zero faults, so the fix is correct and safe and stays ON. But
the owner confirmed: **still no interaction prompt for player 2 with player 1 away.** Couch mode is
necessary-looking but not sufficient. Keep it; it is not the prompt blocker.

Also measured, and worth recording because it kills another candidate: `[ixcomp]` shows
`focus=00000000` for **both** heroes, including hero 1 whose prompt works. So
`CECCharacterInteraction`'s focus field (+160) is not the prompt mechanism either.

---

## 19. THE LOCAL-vs-REMOTE CHOKEPOINT — `sub_825100` (build 38, awaiting first test)

### 19.1 The prompt is not suppressed — it is transmitted

The floating "press A" prompt is an **emotion icon**. `AddDesiredEmotionIcon` = `sub_919E00`,
registered on the script class `"Player"` (table `sub_924780`). Its shape:

```c
if ( sub_825100(hero) ) { ...build and show the icon locally...                  }
else                    { ...sub_6A8640(): send CNetDesiredEmotionIconPacket...  }
```

If `sub_825100` says "not local" for hero 2, hero 2's prompt is **not dropped — it is sent over the
network**, to a peer that does not exist. That is a different failure from everything assumed so far
and it fits the symptom exactly: the request is made, and goes nowhere visible.

Prompt vocabulary, for searching: `EmotionIconInteract`, `EIUI_Interact`, `EmotionIconShopPlinth`,
`EmotionIconInteractPawnShop1of1`, `AddDesiredEmotionIcon`, `SetCanAddSimEmotionIcons`,
`CNetDesiredEmotionIconPacket`. HUD prompts are also called **suggestions**
(`AddHUDSuggestionOfType`, `UpdateSimSuggestions`) — a separate system worth checking if icons
turn out to be the wrong one.

### 19.2 Why this may be the master switch, not another symptom

```
sub_825100  __thiscall(heroEntity)   ecx = entity, retn 0, returns char   (disasm-verified)
            1 = handle locally, 0 = it belongs to someone else -> go to the network
            40+ call sites across presentation, abilities and inventory
```

It is the single chokepoint deciding local-vs-network **per entity**. If it returns 0 for hero 2,
that plausibly explains the prompt *and* the missing equipment *and* the shared appearance with one
cause, instead of three.

Its first input is `sub_825080`, which gates on **`sub_6A87A0()` = IsInLiveGame**:

```c
sub_825080(entity):
    v2 = (entity[173] & 0x40) == 0;
    ...
    if (!sub_6A87A0()) return v2;          // NOT in a live game -> early, local-ish answer
    ... consults the remote-player maps (sub_D68770 / sub_D687C0) ...
```

**This exposes a real tension in our own setup.** `IsInLiveGame` is TRUE for us *only* because we
set `coop[148]` to open the menu gate (§16.4). A genuine couch game is `IsInCouchGame=1,
IsInLiveGame=0`; we are currently **1/1**, which the game only ever produces when actually online.
So `coop[148]` may be buying menus at the cost of routing player 2 through remote-player paths.

### 19.3 What build 38 does

`Hook_IsLocal` repoints all `sub_825100` call sites. Default ON, `nolocalfix.txt` disables.

- **measures** `sub_825100` for both heroes — `[islocal] heroN entity=... -> sub_825100=V`, deduped
  to one line per hero per second (it is called many times per frame; the interesting event is a
  *change* in the verdict)
- **overrides** it to 1 for hero 2 only, and says so in the log (`FORCED->1`)

Hero pointers are cached on a 250 ms tick so the hook does not walk the entity manager per call.

### 19.4 How to read the result

| `[islocal]` says | Meaning |
|---|---|
| `hero1 -> 1`, `hero2 -> 0` + `FORCED->1` | Hypothesis confirmed; hero 2's output was going to the network. If the prompt now appears, this is the root cause of the remainder. |
| both `-> 1`, no `FORCED` | `sub_825100` is not the discriminator; the prompt blocker is upstream, in whatever decides to call `AddDesiredEmotionIcon` at all. Go there — do not keep pushing on this. |
| `hero2 -> 0`, forced, prompt still absent | The icon is local now but nothing requests one for hero 2. Move upstream to the per-frame scan that decides *who* gets an icon. |

**If the third row happens, the next thing to try is the `coop[148]` trade in §19.2** — clear it so
`IsInLiveGame` goes false (with couch mode on), and accept that the menu may regress for that one
run. The menu gate has an escape hatch that does not need IsInLiveGame:

```c
sub_7D7000:  if (!sub_6A87A0() && playerKey != 1 && !*(BYTE*)(*(DWORD*)(*(DWORD*)(dword_1DC2558+28)+88)+141))
                 return 1;   // refuse
```

so identifying that `+141` flag would let us have couch mode, `IsInLiveGame=0`, **and** menus.

### 19.5 RESULT of build 38 — `sub_825100` is NOT the discriminator

```
islocal hero2 count: 23      every one "-> sub_825100=1"
FORCED count: 0
```

Hero 2 was **already** considered local at every call site. So §19.1's "the prompt is transmitted to
a peer that does not exist" is **wrong** — `AddDesiredEmotionIcon` would take the local branch for
hero 2 all along. Nothing is *requesting* an icon for hero 2.

`sub_825100` is still correctly identified (it is the local-vs-remote chokepoint, 300 call sites
repointed cleanly) — it simply is not what is broken. The hook and its `nolocalfix.txt` toggle are
left in place because the measurement is cheap and may matter later, but **do not spend more launches
here.** The remaining question is upstream: what decides to request a prompt for a hero at all.

Also settled by the same run: this is the third mechanism ruled out for the prompt, after
`CAbilityActionPlayerInteract::Perform` (§16.6) and `CECCharacterInteraction`'s focus field (§18.6).

---

## 20. THE PC BUILD HAS A NATIVE COUCH JOIN AT CHARACTER SELECT

**The owner confirmed visually: at the new-game character select the game itself shows a second
player slot.** Not something the mod produced — the game's own UI.

That reframes the project again, and more sharply than §17.1 did. The mod has spent its whole life
*fabricating* a second player because the natural join was assumed unreachable. It is reachable, and
our own XInput trigger has been **swallowing the Start press** that would drive it: the trigger fires
on any Start edge from pad >= 1 and runs the synthetic join instead.

A natively joined player 2 would be a genuinely profiled local user — which is the original mission
statement — and would presumably arrive with correct script rules, equipment, appearance and
targeting, i.e. every remaining symptom at once.

### 20.1 How the black screen exposed it

The owner started a new game and joined at that slot. Result: black screen on load-in. The log shows
**no crash and no fault** — it ticks to the end, and after the load there are two heroes with correct
controller indices and both alive. Co-op state survived; presentation did not. Hero count went
`1 -> 2` (our forced join at character select) `-> 1` (world torn down for the new game) `-> 2`.

Cause: our synthetic join spawns a hero and wires a co-op camera, and has only ever been exercised
in-world. Running it before a level load is out of contract.

### 20.2 Build 39 — the trigger is now gated and can be handed back

- The trigger consults the engine's own session predicates and **refuses outside a live session**,
  logging `[trigger] pad N Start REFUSED: not in a live session (674320=.. 674340=..)`:
  ```
  sub_674320()  world/session ready       (dword_1DBDD4C -> +12 -> +194)
  sub_674340()  that AND sub_9EE3E0(+36)  -- the SAME predicate sub_BC0500 uses internally
  ```
- **`nojoin.txt`** (created, currently ACTIVE) stops the trigger intercepting Start at all, passing
  the press to the game so the native flow can run.

### 20.3 The next experiment, and why it is worth more than any other

With `nojoin.txt` in place: new game -> join at the native P2 slot -> play.

`RunDeferredJoin` never runs, so **none** of our fabrication happens — no `PopulateSigninSlot`, no
`coop[148]`, no forced `SetGameMode(1)`, no `sub_684650`. What stays active is the sign-in/XInput
emulation that makes the game *offer* the slot in the first place, plus the passive hooks.

That makes this simultaneously the clean control run §18.5 asked for. `[coopnow] mode[B8]` now reads
the **engine's own** game mode, answering the question build 37 could not: does the native join set
couch mode itself? And `[aa1510] ruleword` shows hero 2's genuine script rules rather than ours.

Read in this order:

| Log line | Tells you |
|---|---|
| `[queue] JOIN-TRIGGER = OFF` | confirms the native path is being used |
| `[heroes] count=2` with `+52(ctrlIdx)=1` | the game joined player 2 by itself |
| `[coopnow] mode[B8]` | whether the native join sets couch mode (§18 answered properly) |
| `[aa1510] hero2 ruleword` | hero 2's real script rules, unmasked by our mirror |

If player 2 joins natively and works, **most of §13–§19 becomes dead weight** and the job becomes
deleting fabrication rather than adding to it. Consider turning `norulefix.txt` on afterwards to see
whether the rule mirror is still needed at all.

### 20.4 CONFIRMED: the native prompt exists and the game declines for a SIGN-IN reason

The owner photographed it: **"Player Two, press ⒝ to join."** on screen. Spamming Start does nothing.

Diagnosis from the same run — and note the first line corrects a wrong assumption of mine:

- `[queue] JOIN-TRIGGER = ON` — `nojoin.txt` had **vanished** from the game folder (it was created
  and verified at 153 bytes, then was gone). The toggle code was fine; the file was not there.
- **It did not matter.** `Hook_GameXInputGetState` only *observes* — it returns the original pad
  state unchanged and never consumes the press. So the game received every Start on pad 1.
- `[trigger] pad 1 Start REFUSED: not in a live session (674320=1 674340=0)` × ~20 — our gate
  correctly stood aside at character select.
- **Zero `[signin]` lines in the whole run.** `PopulateSigninSlot` only ever ran inside
  `RunDeferredJoin`, so with no synthetic join the pad was never signed in.

Earlier logs give the missing value directly: before populate, pad 1 is `771D60=0` — *not a
signed-in local user*. The game is showing its own join prompt and refusing because there is no
second user to join **as**.

### 20.5 Build 40 — sign the pad in, let the GAME join

At a menu / character select (`sub_674340() != 1`), a Start edge from pad >= 1 now queues
**sign-in only**, never the synthetic join:

```
[trigger] pad N Start at a menu (...): queueing SIGN-IN only, no synthetic join
[d3d]     Present tid=... -> sign in pad N for the game's own join
[signin]  populate idx N -> b0=.. b1=.. guest=..  771D60=1
[signin]  pad N is now a signed-in local user -- now press Start again
```

- Runs on the **main thread** via the existing `Present` hand-off, because `PopulateSigninSlot`
  calls the game's `sub_772190`, which allocates and has only been exercised there.
- **Once per pad** (`g_signinDone[]`), since Start will be pressed repeatedly; re-syncing the slot
  underneath the game's own join handler is not worth the risk. Reset automatically if it fails.
- In-world behaviour is unchanged: the synthetic join still runs there.

So the sequence is: **press Start once (signs the pad in) → press Start again (the game joins).**

This is the closest the project has come to the original mission. If it works, player 2 is joined by
the game's own character-select flow — a real profiled local user with its own appearance and
equipment — and the fabrication in §13–§19 becomes removable rather than something to extend.

### 20.6 Build 40 never actually signed in — the Present hook does not exist

Build 40 did nothing, and not because the theory was wrong: **the queued work never ran.**

```
[trigger] pad 1 Start at a menu (674320=1 674340=0): queueing SIGN-IN only ...
[trigger] pad 1 Start at a menu: already signed in ...        (x many)
```

— and then **no `[d3d]` line and no `[signin]` line anywhere.** `g_signinDone` latched on the first
press, so every later press reported "already signed in" when nothing had been signed in.

**The D3D `Present` hook has never fired in this project.** There is not one `[d3d]` line in any log.
Proof of where the work has really been running, from build 38:

```
[trigger] Start edge pad 1 -> queued join for main thread  (input tid=2648)
[join] tid=2648 byte_1C86BDB 1->1  sub_BC0500(1)            <- SAME thread
```

`Hook_DropIn` — which runs on the **input thread** — is what has always dispatched the join. The
long comment at `RunDeferredJoin` claiming it "executes on the MAIN thread (via the D3D Present
hook)" is **wrong** and has been wrong for many builds. `PopulateSigninSlot` has therefore always
run on the input thread, successfully, every time.

`Hook_DropIn` does not dispatch at a menu, so deferring the sign-in there lost it entirely.

**Build 41 calls `PopulateSigninSlot` directly from the trigger on the input thread**, which is
where it has always effectively run. The `g_signinDone` latch is now only set on success.

Lesson worth keeping: a "deferred to the main thread" hand-off in this codebase is not load-bearing.
Check for a `[d3d]` line before assuming Present runs.

### 20.7 Sign-in is NOT the gate — eliminated by measurement

```
[signin] populate idx 1 -> b0=0 b1=1 guest=0  771D60=1
[signin] pad 1 is now a signed-in local user
```

Pad 1 became a genuine signed-in local user and the game still refused every press. §20.4's
inference was wrong. The sign-in call is kept (correct and harmless) but it is not the answer.

### 20.8 THE GAME'S OWN PENDING-JOIN SLOT — the best lead in the project

Working back from `sub_BC0500`'s three native callers instead of guessing. Two of them make the
same two-way choice:

```c
sub_B9B0C0 (in-world drop-in) / sub_B74740:
    if ( <game is mid-transition> )  sub_9EE630(idx);   // record a PENDING join
    else                             sub_BC0500(idx);   // join right now
```

and the third is the consumer, in its post-load tail:

```c
sub_9F0010:   if ( c[0x20] != -1 ) { sub_BC0500(c[0x20]); c[0x20] = -1; }
```

So the engine has **one field meaning "player N asked to join; do it once the world is up"**, which
is exactly what a character-select join must use. The whole chain is plain pointer walks:

```
game        = dword_1DBDD4C                 (sub_673C50)
wrapper     = *(*(game + 0x0C) + 0x24)      (sub_673ED0)
coordinator = *(wrapper + 4)
coordinator[0x20] = controller index        (sub_9EE630, __thiscall(wrapper, idx), retn 4)
```

`sub_9F07E0` is the coordinator's constructor and confirms the identification: it initialises
`+0x20` to -1, registers network packet handlers 312/313, and branches on `byte_1C86BDB` — the
split-screen flag this mod already sets. Found via `0x9EE637 mov [eax+20h], ecx`, the only register
write to that field in the whole class (disk scan; the MCP `find_bytes` is broken, §18.2).

### 20.9 Build 42 — set the pending slot, let the game do everything

At a menu, a Start edge from pad >= 1 now writes the pending-join field via `SetPendingCoopJoin`
(guarded reads + SEH, each step logged so a null says exactly where the chain broke). Re-armed on
every press (throttled to 400 ms) because the post-load handler consumes it and resets it to -1.

```
[pending] coordinator=........  pendingJoin -1 -> 1  (the GAME should now join pad 1 ...)
```

**This is the least invasive attempt the project has made.** Nothing is fabricated and no hero is
spawned; we write the one value the character-select prompt is supposed to write, and the game's own
post-load path performs the join. It should also avoid the §20.1 black screen, which came precisely
from spawning a hero mid-menu before a level load.

Read the result as:

| Log | Meaning |
|---|---|
| `[pending] ... -1 -> 1` then a native join after the load | The native path works; strip the fabrication in §13–§19 |
| `[pending] ... -1 -> 1` but nothing happens at load | The slot is right but `sub_9F0010` does not run in this flow. Trace its caller `sub_9F07C0` <- `sub_67B250` |
| `[pending] <step> null` | The pointer chain differs at this phase; the log names the exact step that failed |

### 20.10 CORRECTION — the join was never being refused. READ THIS BEFORE §20.4–§20.9

The owner: *"Before we started messing with the new character prompt I was able to press this and
have it disappear, but I complained of a black screen after character creation."*

**The prompt used to disappear because OUR synthetic join spawned hero 2 right there.** The game's
prompt is satisfied by a second hero existing. It was never refusing anything.

So the sequence of errors, all mine:

1. Build 38 joined at character select. Prompt disappeared. **Black screen after character creation
   — that was always the only real bug.**
2. Build 39 added a gate refusing the join at menus (§20.2). That removed the working behaviour.
3. Builds 40–42 then chased "why does the game refuse to join" — **a problem that did not exist.**
   Sign-in (§20.4/§20.7) and the pending slot (§20.8) were both answers to the wrong question.

§20.4 through §20.9 are not wrong about the *engine* — the pending-join slot is real and correctly
identified — but they are answering a question nobody asked. **The target is the black screen.**

Not wasted: `sub_9EE630` / the pending slot / `sub_9F0010` are genuinely how the engine defers a
join across a load, and may still be the *right* way to join at character select (it avoids spawning
a hero mid-menu, which is the prime suspect for the black screen). It is simply **untested** —

### 20.11 Build 42's test never ran

```
[pending] coordinator=244c0d90  pendingJoin -1 -> 1      <- set correctly, stayed 1, never consumed
[heroes] count=1                                          <- once, early
[coopnow]                                                 <- ZERO lines all run
```

~2800 further log lines with no world. **The owner never proceeded past the prompt**, so the load
that would consume the pending slot never happened. With build 42 the prompt is *expected* to stay
on screen — the hero is created after the load, not at the prompt.

### 20.12 Build 43 — the `[phase]` heartbeat

The watchdog's `[coopnow]`/`[heroes]` logging only runs once a world exists, which is why a
black-screened or never-loading session produced thousands of lines saying nothing. Build 43 ticks
from the input poll (which always runs), logging only on change:

```
[phase] worldReady(674320)=..  liveSession(674340)=..  pendingJoin=..
```

This distinguishes the two black-screen explanations that have been conflated:

| After character creation | Meaning |
|---|---|
| `674340` goes to 1, screen black | The load COMPLETED; rendering/camera is the bug |
| `674340` stays 0, screen black | The load never finished; not a rendering bug at all |
| `pendingJoin` goes `1 -> -1` | `sub_9F0010` consumed it — the native deferred join fired |

### 20.13 The two candidate causes of the black screen, untested

Both are cheap toggle experiments, and only ONE variable should change per run:

- **`nocouchmode.txt`** — couch mode (§18) was ON for the build-38 black screen and is set *before*
  a level load. It changes camera/viewport branches engine-wide.
- **`nocoopfix.txt`** — currently PRESENT, so `sub_684650` never runs, so the co-op camera/viewport
  wiring in `sub_6840C0(1)` is **skipped**. Harmless in-world, but a fresh world after a load may
  need it. Removing this file is the single most promising untried change.

Known from the build-38 black screen: no crash, no fault, and after the load **two heroes with
correct controller indices, both alive, devices bound** (`hero2 device +0x34=1`). Co-op state
survived; only presentation was wrong.

### 20.14 If it still refuses

The sign-in gate is the *likely* reason, not a proven one — `771D60=0` is a fact, "that is why the
handler refuses" is inference. If `[signin] ... 771D60=1` appears and Start still does nothing, find
the handler and read its real precondition rather than guessing again: start from the prompt string
(`GUI_COUCH_INVITE_*` / the "press to join" text) and work back to the code that polls for the press.
`sub_674340()==0` at that screen is a useful landmark — whatever runs there is menu-phase code.

---

## 14. First actions for the next contributor

1. Read this file end-to-end. **§16, §17 and §18 are the current state of knowledge** — §12/§13 are
   superseded (see §16.5 for the corrections), **§18.1 corrects the flag names used in §16.3**, and
   everything before them is history. Open both binaries headless per §17 and confirm with
   `list_instances()`. Note §18.2: the MCP `find_bytes` tool is broken on idalib sessions.
2. Build/deploy; confirm the milestone in `couchcoop.log`: `[signin] populate idx 1 -> … 771D60=1`,
   `[coop] post-join context=… +0x168(2nd pad)=1`, `[devid] hero2 dev=1`, clean spawn with no
   prompt, no `[crash]`.
3. Verify pad 2 moves hero 2 and only hero 2. That is the committed, working baseline.
4. Do NOT re-open: §5, §9 (solved), the `CInputProcessInteract` gate (§11), or **any** of the seven
   dead ends in §12.6. Do **NOT** set `coop[62]=1` without first completing §13.
5. Start at **§13.2 step 2 (XLLN multi-user)** — it is the cheapest route to a real second user and
   would make most of the rest unnecessary.

### Crash lessons (both cost a launch)

- **Never trust Hex-Rays' calling convention when the call site contradicts it.** `sub_B6CF30`
  decompiles as `__stdcall(a1,a2)` but its call site does `mov ecx, esi` first.
- **Never dereference game objects during the spawn frame.** A half-built input process holds
  *garbage* in `this+12`, not null, so `<= 0x10000` guards sail straight past it. Use the `PtrOk`
  (VirtualQuery) + SEH pattern in `dllmain.gatetrace.bak.cpp`, plus the `g_probeQuietUntil` window
  set in `RunDeferredJoin`. With those three in place the same probe ran clean.
