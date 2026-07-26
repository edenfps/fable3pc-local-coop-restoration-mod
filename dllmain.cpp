// Fable III PC — Local (same-console) shared-screen co-op restoration.
//
// Two fixes in one DLL (see README.md):
//   1) In-memory patch of the input-manager ctor sub_188DE60 so XInput pads 0..3 each get a
//      CJoystickXBox360 device (retail PC only ever created pad 0). Verified 59-byte in-place
//      patch at RVA 0x0148DEDA.
//   2) xlive IAT hooks so a second local user (pad N connected) appears "signed in locally",
//      satisfying sub_BBFA10 / sub_771D60 without GFWL's sign-in UI. Index 0 forwards to the
//      real xlive/XLiveLessNess.
//
// Build: x86 (32-bit) DLL, e.g.  cl /LD /O2 dllmain.cpp
// Deploy: drop into ./xlln/modules/  (or inject after the game's imports are bound).
//
// NOTE: addresses assume the retail Fable3.exe analysed here (imagebase 0x400000). The patch
// is guarded by an original-bytes check and will refuse to apply to a different build.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// ------------------------------------------------------------------ diagnostics
// Writes couchcoop.log next to Fable3.exe. Low-frequency calls only.
static void Log(const char* fmt, ...) {
    char line[1024];
    va_list ap; va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);   // supports %s %d %u %x %c (no %p/%f)
    va_end(ap);
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    lstrcpyA(slash ? slash + 1 : path, "couchcoop.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n; char out[1088];
    int len = wsprintfA(out, "%s\r\n", line);
    WriteFile(h, out, (DWORD)len, &n, nullptr);
    CloseHandle(h);
}

// ------------------------------------------------------------------ config / offsets
static const uintptr_t RVA_INPUT_PATCH        = 0x0148DEDA; // sub_188DE60 single-pad block
static const uintptr_t RVA_IAT_GetSigninState = 0x016D55E4;
static const uintptr_t RVA_IAT_GetSigninInfo  = 0x016D55E8;
static const uintptr_t RVA_IAT_GetName        = 0x016D565C;
static const uintptr_t RVA_IAT_CheckPrivilege = 0x016D5640;
static const uintptr_t RVA_IAT_XInputGetState = 0x016D5508; // __imp_XInputGetState (XINPUT1_3)
static const uintptr_t RVA_IAT_WaitForSingleObject = 0x016D5044; // __imp_WaitForSingleObject (KERNEL32)
static const uintptr_t RVA_IAT_WaitForSingleObjectEx = 0x016D5204; // __imp_WaitForSingleObjectEx (KERNEL32)
static const uintptr_t RVA_g_inputSubsys = 0x01CFC8F0; // dword_20FC8F0 (input subsystem global struct)
static const unsigned  OFF_inputDoneEvent = 0x120;     // +288: event the input thread SetEvents; main thread waits on it
static const uintptr_t RVA_cb_DropIn = 0x0171A26C;     // callback ptr -> sub_B9B0C0 (drop-in handler, main thread)
static const uintptr_t RVA_IAT_Direct3DCreate9 = 0x016D5554; // __imp_Direct3DCreate9 (d3d9)
static const uintptr_t RVA_IAT_ShowSigninUI   = 0x016D55E0; // __imp_XShowSigninUI (xlive)
static const uintptr_t RVA_g_primaryPad = 0x01886BE4; // dword_1C86BE4 (primary controller idx, init -1)
static const uintptr_t RVA_g_pendingPad = 0x01886BE8; // dword_1C86BE8 (pending guest idx, init -1)
static const uintptr_t RVA_b_coopActive = 0x01886BDA; // byte_1C86BDA (ONLINE coop active)
static const uintptr_t RVA_b_localCoop  = 0x01886BDB; // byte_1C86BDB (LOCAL split-screen coop active) -- NEVER set by the game; the stubbed 360 switch. Gates the input-context bind in sub_BBFA10 (0xBC0416).
static const uintptr_t RVA_mgrSingleton = 0x019BDD4C; // dword_1DBDD4C (session-manager singleton)
static const uintptr_t RVA_signinOwner  = 0x019C2550; // dword_1DC2550 (sub_6A66C0 returns this); mgr = *(owner+40)
static const uintptr_t RVA_sub_772190   = 0x00372190; // sign-in slot sync (hardcoded to user 0 -> slot 0)

static const BYTE PATCH_ORIG[59] = {
    0x38,0x5D,0x01,0x74,0x36,0x8D,0x54,0x24,0x0C,0x52,0x53,0xE8,0x58,0xC0,0xCD,0xFF,0x3D,0x8F,0x04,0x00,
    0x00,0x74,0x24,0x68,0xA0,0x00,0x00,0x00,0xE8,0xC5,0x56,0x17,0x00,0x8B,0xC8,0x83,0xC4,0x04,0x33,0xC0,
    0x3B,0xCB,0x74,0x06,0x56,0xE8,0x24,0xE8,0xFF,0xFF,0x8B,0xF8,0x8B,0xCE,0xE8,0xEB,0x0F,0x00,0x00 };

static const BYTE PATCH_NEW[59] = {
    0x31,0xFF,0x8D,0x54,0x24,0x0C,0x52,0x57,0xE8,0x5B,0xC0,0xCD,0xFF,0x3D,0x8F,0x04,0x00,0x00,0x74,0x20,
    0x68,0xA0,0x00,0x00,0x00,0xE8,0xC8,0x56,0x17,0x00,0x59,0x89,0xC1,0x56,0x89,0xF8,0xE8,0x2D,0xE8,0xFF,
    0xFF,0x57,0x89,0xC7,0x89,0xF1,0xE8,0xF3,0x0F,0x00,0x00,0x5F,0x47,0x83,0xFF,0x04,0x72,0xC8,0x90 };

// ------------------------------------------------------------------ XDK types
typedef uint64_t XUID;

// XUSER_SIGNIN_INFO — 40 bytes, layout confirmed from the binary's reads.
#pragma pack(push, 1)
struct XUSER_SIGNIN_INFO {
    XUID   xuid;                 // +0  must be non-zero & unique per user
    DWORD  dwInfoFlags;          // +8  bit0=LIVE, bit1(0x2)=GUEST  -> keep 0
    DWORD  UserSigninState;      // +12 0=none,1=local,2=live       -> 1
    DWORD  dwGuestNumber;        // +16
    DWORD  dwSponsorUserIndex;   // +20
    CHAR   szUserName[16];       // +24 XUSER_NAME_SIZE
};
#pragma pack(pop)
static_assert(sizeof(XUSER_SIGNIN_INFO) == 40, "XUSER_SIGNIN_INFO must be 40 bytes");

// xlive exports (all __stdcall)
typedef DWORD (WINAPI *fnGetSigninState)(DWORD dwUserIndex);
typedef DWORD (WINAPI *fnGetSigninInfo )(DWORD dwUserIndex, DWORD dwFlags, XUSER_SIGNIN_INFO* pInfo);
typedef DWORD (WINAPI *fnGetName       )(DWORD dwUserIndex, LPSTR szName, DWORD cch);
typedef DWORD (WINAPI *fnCheckPrivilege)(DWORD dwUserIndex, DWORD priv, BOOL* pfResult);
typedef DWORD (WINAPI *fnShowSigninUI  )(DWORD cPanes, DWORD dwFlags);

static fnGetSigninState g_origState = nullptr;
static fnGetSigninInfo  g_origInfo  = nullptr;
static fnGetName        g_origName  = nullptr;
static fnCheckPrivilege g_origPriv  = nullptr;
static fnShowSigninUI   g_origShow  = nullptr;

// ------------------------------------------------------------------ XInput (for pad presence)
#pragma pack(push,1)
struct XI_GAMEPAD { WORD wButtons; BYTE bLT, bRT; SHORT sLX, sLY, sRX, sRY; };
struct XI_STATE   { DWORD dwPacketNumber; XI_GAMEPAD Gamepad; };
#pragma pack(pop)
#define XI_START 0x0010
typedef DWORD (WINAPI *fnXInputGetState)(DWORD, XI_STATE*);
static fnXInputGetState g_xiGetState = nullptr;   // our own copy (LoadLibrary) for PadConnected
static fnXInputGetState g_origGameXI = nullptr;   // the game's original IAT target (forwarded to)
static BYTE*  g_base = nullptr;                    // Fable3.exe image base (for global pokes)
static volatile LONG g_joinedFlag = 0;             // 1 once player 2 spawned (pre/post-join log correlation)
// Build 22 crashed on spawn: it probed every input-process instance the first time it was seen,
// which includes the frame sub_BC0500 is still constructing hero 2's processes. A half-built
// instance holds GARBAGE in this+12 rather than null, so a `<= 0x10000` guard sails past it and the
// E+172 read faults. Two defences now: this quiet window around the join, plus VirtualQuery-checked
// reads and SEH around the whole gate walk (see PtrOk / SafeGate).
static volatile LONG g_probeQuietUntil = 0;
static WORD   g_prevButtons[4] = {0,0,0,0};       // per-pad previous button mask (Start edge detect)

static bool PadConnected(DWORD idx) {
    if (!g_xiGetState) return false;
    XI_STATE s; memset(&s, 0, sizeof(s));
    return g_xiGetState(idx, &s) == ERROR_SUCCESS; // 0x48F = ERROR_DEVICE_NOT_CONNECTED
}

// Sign-in policy:
//   g_fakeSignin == true  : idx 1..3 report "signed in" whenever a pad is connected (eager).
//   g_fakeSignin == false : idx 1..3 report "signed in" ONLY after the game asked to sign that
//                           pad in (XShowSigninUI marked it) -> matches the console sequence.
// Toggle: create a file "p2_signin_off.txt" next to Fable3.exe to select the stateful (false) mode.
static bool         g_fakeSignin = true;
static volatile LONG g_marked[4] = {0,0,0,0}; // set when the game requests sign-in

static bool PadPresent(DWORD idx) { return idx >= 1 && idx <= 3 && PadConnected(idx); }
static bool IsActiveGuest(DWORD idx) { return PadPresent(idx); } // XInput diag helper (pad-based)
// Whether we should present idx as a signed-in local user right now.
static bool GuestSignedIn(DWORD idx) {
    return PadPresent(idx) && (g_fakeSignin || InterlockedCompareExchange(&g_marked[idx], 0, 0) != 0);
}

// ------------------------------------------------------------------ hooks
// Sign-in hooks are now pure pass-throughs to XLiveLessNess (which owns Eden=idx0, Liah=idx1),
// plus first-query logging so we can see exactly what XLLN reports for player 2.
static LONG g_stateLogged[4] = {0,0,0,0};
static DWORD WINAPI Hook_XUserGetSigninState(DWORD idx) {
    DWORD r = g_origState ? g_origState(idx) : (idx == 0 ? 1u : 0u);
    if (idx > 0 && idx < 4 && InterlockedExchange(&g_stateLogged[idx], 1) == 0)
        Log("[hook] XUserGetSigninState(idx=%u) -> %u (XLLN)  pad_connected=%d", idx, r, PadConnected(idx));
    return r;
}

// During PopulateSigninSlot we drive the game's user-0-hardcoded sign-in sync (sub_772190) but want
// it to fetch a DIFFERENT controller's profile. While g_populateIdx>=0 (on the populating thread),
// remap XUserGetSigninInfo(0) -> that controller so sub_772190 reads player 2's real XLLN profile.
static volatile LONG g_populateIdx = -1;
static volatile LONG g_populateTid = 0;

static LONG g_infoLogged[4] = {0,0,0,0};
static DWORD WINAPI Hook_XUserGetSigninInfo(DWORD idx, DWORD flags, XUSER_SIGNIN_INFO* pInfo) {
    LONG pop = InterlockedCompareExchange(&g_populateIdx, -1, -1);
    if (pop >= 0 && idx == 0 && (LONG)GetCurrentThreadId() == InterlockedCompareExchange(&g_populateTid, 0, 0))
        idx = (DWORD)pop; // scoped redirect: user 0 query -> the controller we're populating
    DWORD r = g_origInfo ? g_origInfo(idx, flags, pInfo) : (DWORD)E_FAIL;
    if (idx > 0 && idx < 4 && InterlockedExchange(&g_infoLogged[idx], 1) == 0) {
        if (r == ERROR_SUCCESS && pInfo)
            Log("[hook] XUserGetSigninInfo(idx=%u,flags=%u) -> OK  user='%s' state=%u flags=%x xuidLo=%08x",
                idx, flags, pInfo->szUserName, pInfo->UserSigninState, pInfo->dwInfoFlags, (unsigned)pInfo->xuid);
        else
            Log("[hook] XUserGetSigninInfo(idx=%u,flags=%u) -> err %08x (XLLN; flags=2 fails for offline users)", idx, flags, r);
    }
    return r;
}

static DWORD WINAPI Hook_XUserGetName(DWORD idx, LPSTR szName, DWORD cch) {
    return g_origName ? g_origName(idx, szName, cch) : (DWORD)E_FAIL;
}

// Walk the engine's hero vector: playerList = *(*(dword_1DBDD4C+0xC)+0x40); slots of 12 bytes,
// hero ptr at slot+4, hero+52 = owning controller index.
static bool GetHeroVector(DWORD* pBegin, DWORD* pEnd) {
    if (!g_base) return false;
    DWORD mgr = *(DWORD*)(g_base + RVA_mgrSingleton); if (mgr <= 0x10000) return false;
    DWORD a   = *(DWORD*)(mgr + 0x0C);                if (a   <= 0x10000) return false;
    DWORD pl  = *(DWORD*)(a + 0x40);                  if (pl  <= 0x10000) return false;
    DWORD begin = *(DWORD*)(pl + 0x0C), end = *(DWORD*)(pl + 0x10);
    if (begin <= 0x10000 || end < begin || (end - begin) % 12 || (end - begin) > 12 * 8) return false;
    *pBegin = begin; *pEnd = end; return true;
}
static bool HeroExistsForIndex(LONG idx) {
    DWORD b, e; if (!GetHeroVector(&b, &e)) return false;
    for (DWORD s = b; s < e; s += 12) { DWORD h = *(DWORD*)(s + 4); if (h > 0x10000 && *(int*)(h + 52) == idx) return true; }
    return false;
}
typedef int (__cdecl *fnJoinCouch)(unsigned int); // sub_BC0500(controllerIdx)

// --------- Phase 1: drive hero 2's locomotion from controller 1 (per-creature) ---------
// hero2 = *(*(EM+0xF4)); EM = *(*(*(*(dword_1DBDD4C+0xC)+0x1C)+0x4)+0x4)  (from sub_673E80/sub_658E40/sub_6BCBA0)
static DWORD GetHero2Entity() {
    if (!g_base) return 0;
    DWORD s = *(DWORD*)(g_base + RVA_mgrSingleton); if (s <= 0x10000) return 0; // dword_1DBDD4C
    DWORD a = *(DWORD*)(s + 0x0C);                   if (a <= 0x10000) return 0;
    DWORD w = *(DWORD*)(a + 0x1C);                   if (w <= 0x10000) return 0; // world (sub_673E80)
    DWORD b = *(DWORD*)(w + 0x04);                   if (b <= 0x10000) return 0;
    DWORD em= *(DWORD*)(b + 0x04);                   if (em<= 0x10000) return 0; // entity mgr (sub_658E40)
    DWORD p = *(DWORD*)(em + 0xF4);                  if (p <= 0x10000) return 0; // hero2 holder (sub_6BCBA0)
    DWORD h = *(DWORD*)p;                            return (h > 0x10000) ? h : 0;
}
// ECS component-by-type: *( *(entity+0x58) + 8*(*(BYTE*)(*(entity+0xA8)+typeId)) + 4 )
static DWORD GetComponentOf(DWORD entity, int typeId) {
    if (entity <= 0x10000) return 0;
    DWORD tt = *(DWORD*)(entity + 0xA8);  if (tt  <= 0x10000) return 0; // type->slot table
    BYTE  slot = *(BYTE*)(tt + typeId);
    DWORD arr = *(DWORD*)(entity + 0x58); if (arr <= 0x10000) return 0; // component array
    DWORD c = *(DWORD*)(arr + 8u * slot + 4);
    return (c > 0x10000) ? c : 0;
}
static DWORD GetHeroEntity(unsigned holderOff) { // 0xEC=hero1, 0xF4=hero2
    if (!g_base) return 0;
    DWORD s = *(DWORD*)(g_base + RVA_mgrSingleton); if (s <= 0x10000) return 0;
    DWORD a = *(DWORD*)(s + 0x0C);                   if (a <= 0x10000) return 0;
    DWORD w = *(DWORD*)(a + 0x1C);                   if (w <= 0x10000) return 0;
    DWORD b = *(DWORD*)(w + 0x04);                   if (b <= 0x10000) return 0;
    DWORD em= *(DWORD*)(b + 0x04);                   if (em<= 0x10000) return 0;
    DWORD p = *(DWORD*)(em + holderOff);             if (p <= 0x10000) return 0;
    DWORD h = *(DWORD*)p;                            return (h > 0x10000) ? h : 0;
}
static const int TYPE_CECPLAYERCONTROL = 134; // movement component (accumulator +0x0C/+0x10)
static const int TYPE_CONTROLSCHEME    = 37;  // input/control-scheme component; holds joystick at +0x14
// Mirror of the game's Player.GetJoystickDeviceID (sub_912C10): the hero's type-37 component holds a
// joystick pointer at +0x14; that device's assigned controller id is at +0x34. Null joystick => -1
// (== "any/primary pad"), which is our prime suspect for hero 1 accepting every pad.
static int HeroDeviceId(DWORD hero) {
    DWORD comp = GetComponentOf(hero, TYPE_CONTROLSCHEME);
    if (!comp) return -3;                       // no control-scheme component
    DWORD joy = *(DWORD*)(comp + 0x14);
    if (joy <= 0x10000) return -1;              // matches sub_912C10's null path
    return *(int*)(joy + 0x34);
}
// CECPlayerControl move-direction (GetLastControlDirection) = +0xC0 (x), +0xC4 (y).
// Confirm on hero 1 (read) as you move pad 0, and DRIVE hero 2 from pad 1.
static void DriveHero2FromPad1() {
    DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
    DWORD c1 = h1 ? GetComponentOf(h1, TYPE_CECPLAYERCONTROL) : 0;
    DWORD c2 = h2 ? GetComponentOf(h2, TYPE_CECPLAYERCONTROL) : 0;
    // Read pad 1 stick
    float lx = 0, ly = 0;
    if (g_xiGetState) {
        XI_STATE st; memset(&st, 0, sizeof(st));
        if (g_xiGetState(1, &st) == ERROR_SUCCESS) {
            lx = (float)st.Gamepad.sLX / 32767.0f;
            ly = (float)st.Gamepad.sLY / 32767.0f;
            const float DZ = 0.24f;
            if (lx > -DZ && lx < DZ) lx = 0;
            if (ly > -DZ && ly < DZ) ly = 0;
        }
    }
    // One-shot: dump hero1's control-component vtable as IDA addresses (VA - base + 0x400000)
    // so we can pin the exact class + per-frame Update method with no guessing.
    static LONG vtDumped = 0;
    if (c1 && InterlockedExchange(&vtDumped, 1) == 0) {
        DWORD vt = *(DWORD*)c1;
        DWORD ida_vt = vt - (DWORD)(uintptr_t)g_base + 0x400000;
        DWORD vt2 = c2 ? *(DWORD*)c2 : 0;
        Log("[vt] c1=%08x vtable=%08x (ida=%08x)  c2=%08x vtable2=%08x sameClass=%d",
            c1, vt, ida_vt, c2, vt2, (int)(vt2 == vt));
        for (int i = 0; i < 24; i++) {
            DWORD slot = *(DWORD*)(vt + 4u * i);
            Log("[vt]   [%d] = %08x (ida=%08x)", i, slot, slot - (DWORD)(uintptr_t)g_base + 0x400000);
        }
    }
    // Drive hero 2. The REAL input field is the accumulator +0x0C/+0x10: the per-frame latch
    // sub_104D230 copies +0x0C/+0x10 -> +0xC0/+0xC4 (LastControlDirection, read by locomotion)
    // and then clears +0x0C/+0x10. Writing +0xC0 alone was clobbered by the latch reading
    // hero2's empty accumulator. So feed the accumulator every frame; also set +0xC0 as fallback.
    if (c2) {
        *(volatile float*)(c2 + 0x0C) = lx; *(volatile float*)(c2 + 0x10) = ly; // accumulator (input)
        *(volatile float*)(c2 + 0xC0) = lx; *(volatile float*)(c2 + 0xC4) = ly; // latched (fallback)
    }
    // Diagnostic: log hero1's live accumulator + latched dir, and what we wrote to hero2.
    static LONG frame = 0;
    if ((InterlockedIncrement(&frame) % 15) == 0 && c1) {
        int ax1 = (int)(*(float*)(c1 + 0x0C) * 1000.0f), ay1 = (int)(*(float*)(c1 + 0x10) * 1000.0f);
        int lx1 = (int)(*(float*)(c1 + 0xC0) * 1000.0f), ly1 = (int)(*(float*)(c1 + 0xC4) * 1000.0f);
        if (ax1 || ay1 || lx1 || ly1 || lx != 0 || ly != 0)
            Log("[cpc] h1 c1=%08x accum(%d,%d) latch(%d,%d)  |  h2 c2=%08x wrote(%d,%d)",
                c1, ax1, ay1, lx1, ly1, c2, (int)(lx * 1000.0f), (int)(ly * 1000.0f));
    }
}

// --------- Hang diagnostic: dump the stack of the join thread if it stalls ---------
// Setting byte_1C86BDB=1 before the synthetic join makes sub_BBFA10 take its native coop path,
// which freezes. To learn exactly WHERE (which is a wait-loop vs which is a deadlock), a watchdog
// thread suspends the join thread after a timeout and logs its EIP + in-module return addresses.
static volatile LONG g_joinPhase = 0;   // 0=idle 1=join running 2=dump done
static DWORD         g_joinTid   = 0;
static void DumpModuleStack(HANDLE hThread) {
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(hThread, &ctx)) { Log("[hang] GetThreadContext err=%u", GetLastError()); return; }
    DWORD base = (DWORD)(uintptr_t)g_base;
    Log("[hang] EIP=%08x (rva=%08x)  ESP=%08x EBP=%08x", ctx.Eip, ctx.Eip - base + 0x400000, ctx.Esp, ctx.Ebp);
    DWORD lo = base + 0x1000, hi = base + 0x1C00000;   // in-module range (code+data)
    int found = 0;
    __try {
        for (DWORD p = ctx.Esp; p < ctx.Esp + 0x800 && found < 48; p += 4) {
            DWORD v = *(DWORD*)p;
            if (v >= lo && v < hi) { Log("[hang]   [%08x]=%08x rva=%08x", p, v, v - base + 0x400000); found++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[hang] stack read fault after %d frames", found); }
}
static DWORD WINAPI JoinWatchdog(LPVOID) {
    for (;;) {
        if (InterlockedCompareExchange(&g_joinPhase, 1, 1) == 1) {
            DWORD tid = g_joinTid;
            Sleep(3000);
            if (InterlockedCompareExchange(&g_joinPhase, 1, 1) == 1 && tid) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                if (h) {
                    if (SuspendThread(h) != (DWORD)-1) {
                        Log("[hang] join stalled >3s on tid=%u -> stack dump:", tid);
                        DumpModuleStack(h);
                        ResumeThread(h);
                    }
                    CloseHandle(h);
                }
                InterlockedExchange(&g_joinPhase, 2); // dump once
            }
        }
        Sleep(150);
    }
}

// --------- Deferred join: run the native join on the MAIN thread, off the input lock ---------
// Root cause of every freeze: our XInput hook runs on the game's INPUT thread while it holds the
// device critical section; calling sub_BC0500 there makes sub_7718B0 re-take that lock -> self
// deadlock (confirmed by a stack dump: EIP in ntdll wait, chain sub_BC0500->sub_BBFA10->sub_7718B0).
// Instead we set a pending flag on the input thread and execute the join on the MAIN thread right
// after it wakes from waiting on the input-done event -- exactly where the game's own drop-in
// handler (sub_B9B0C0) runs. There the device lock is released and entity mutation is safe.
typedef DWORD (WINAPI *fnWFSO)(HANDLE, DWORD);
typedef DWORD (WINAPI *fnWFSOEx)(HANDLE, DWORD, BOOL);
static fnWFSO        g_origWFSO   = nullptr;
static fnWFSOEx      g_origWFSOEx = nullptr;
static HANDLE        g_inputEvent = nullptr;
static volatile LONG g_pendingJoin = -1;   // controller idx to join; -1 = none
// Controller idx to SIGN IN (no join) -- the native "Player Two, press Start to join" path. Same
// main-thread hand-off as g_pendingJoin, because PopulateSigninSlot runs the game's own
// sub_772190, which allocates; it has only ever been exercised on the main thread.
static volatile LONG g_pendingSignin = -1;
static volatile LONG g_signinDone[4] = {0,0,0,0};   // sign a pad in once, not on every Start press

// ---------------- THE GAME'S OWN PENDING-COUCH-JOIN SLOT ----------------
// Both native join sites do the same two-way choice: join NOW, or record a PENDING join and let
// the game do it once the world is ready.
//
//     sub_B9B0C0 / sub_B74740:
//         if ( <game is mid-transition> )  sub_9EE630(idx);   // record pending
//         else                             sub_BC0500(idx);   // join immediately
//
//     sub_9EE630(wrapper, idx):  *(*(wrapper + 4) + 0x20) = idx
//     sub_9F0010 (post-load):    if (c[0x20] != -1) { sub_BC0500(c[0x20]); c[0x20] = -1; }
//
// So there is a single field that means "player N pressed join; do it when the world is up", and
// the whole chain to it is plain pointer walks:
//     game        = dword_1DBDD4C                   (sub_673C50)
//     wrapper     = *(*(game + 0x0C) + 0x24)        (sub_673ED0)
//     coordinator = *(wrapper + 4)
//     coordinator[0x20] = controller index          (sub_9EE630)
//
// The coordinator is the couch/co-op join class: its constructor sub_9F07E0 initialises this field
// to -1, registers network packet handlers 312/313, and branches on byte_1C86BDB -- the same
// split-screen flag this mod already sets.
//
// Setting this is the least invasive thing the project has tried: we fabricate nothing and spawn
// nothing, we only tell the game what the character-select prompt is supposed to tell it, and the
// GAME performs the join through its own post-load path. That should also avoid the §20.1 black
// screen, which came from spawning a hero mid-menu before a level load.
static const uintptr_t RVA_g_gameObj = 0x019BDD4C;   // dword_1DBDD4C
static bool PtrOk(DWORD p, SIZE_T n);                // defined later
// Read-only view of the same slot, for the [phase] heartbeat. -2 = chain unavailable.
static int GetPendingCoopJoin() {
    __try {
        DWORD game = *(DWORD*)(g_base + RVA_g_gameObj);
        if (!PtrOk(game, 0x10)) return -2;
        DWORD a = *(DWORD*)(game + 0x0C);
        if (!PtrOk(a, 0x28)) return -2;
        DWORD wrapper = *(DWORD*)(a + 0x24);
        if (!PtrOk(wrapper, 8)) return -2;
        DWORD coord = *(DWORD*)(wrapper + 4);
        if (!PtrOk(coord, 0x24)) return -2;
        return *(int*)(coord + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
static bool SetPendingCoopJoin(unsigned idx) {       // isolated so __try needs no C++ unwinding
    __try {
        DWORD game = *(DWORD*)(g_base + RVA_g_gameObj);
        if (!PtrOk(game, 0x10))    { Log("[pending] game object null (%08x)", game); return false; }
        DWORD a = *(DWORD*)(game + 0x0C);
        if (!PtrOk(a, 0x28))       { Log("[pending] game+0x0C null (%08x)", a); return false; }
        DWORD wrapper = *(DWORD*)(a + 0x24);
        if (!PtrOk(wrapper, 8))    { Log("[pending] wrapper null (%08x)", wrapper); return false; }
        DWORD coord = *(DWORD*)(wrapper + 4);
        if (!PtrOk(coord, 0x24))   { Log("[pending] coordinator null (%08x)", coord); return false; }
        int before = *(int*)(coord + 0x20);
        *(volatile DWORD*)(coord + 0x20) = idx;
        int after = *(int*)(coord + 0x20);
        Log("[pending] coordinator=%08x  pendingJoin %d -> %d  (the GAME should now join pad %u "
            "when the world finishes loading)", coord, before, after, idx);
        return after == (int)idx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[pending] FAULTED (0x%x)", (unsigned)GetExceptionCode());
        return false;
    }
}

// RunDeferredJoin now executes on the MAIN thread (via the D3D Present hook). On the main thread the
// native join sub_BC0500 -> sub_BBFA10 can run its full coop path: sub_7718B0's cross-thread wait is
// serviced by the input thread (no stall), and the input bind sub_752CD0 runs with the state
// sub_BBFA10 sets up around it (no crash). byte_1C86BDB=1 ungates that native per-player bind.
// Populate the game's sign-in slot for controller `idx` so sub_771D60(idx) reports a real, non-guest
// signed-in local user -- the gate that promotes player 2 to a true local player (unlocking the
// native per-player input pump + separation) and stops the "not a gamer profile" warning box.
//
// The PC build's sync sub_772190 is hardcoded to user 0 -> slot 0. We reuse it verbatim (so the
// game does its own std::string/xuid handling -- no fragile manual struct faking): a fake manager
// whose only-read fields +20/+24 point at the REAL slot[idx], plus a scoped XUserGetSigninInfo(0)->
// (idx) redirect so it fetches controller idx's XLLN profile (e.g. "Liah") into that slot.
typedef void (__thiscall *fnSigninSync)(void* mgr);
static bool PopulateSigninSlot(unsigned idx) {
    DWORD owner = *(DWORD*)(g_base + RVA_signinOwner); if (owner <= 0x10000) { Log("[signin] populate: owner null"); return false; }
    DWORD mgr   = *(DWORD*)(owner + 40);               if (mgr   <= 0x10000) { Log("[signin] populate: mgr null");   return false; }
    DWORD base  = *(DWORD*)(mgr + 20), end = *(DWORD*)(mgr + 24);
    if (base <= 0x10000 || end < base) { Log("[signin] populate: bad slot array"); return false; }
    if ((int)idx >= (int)((end - base) / 32)) { Log("[signin] populate: idx %u out of range", idx); return false; }
    DWORD slot = base + 32u * idx;
    DWORD fake[8] = {0};
    fake[5] = slot;          // *(this+20): sub_772190 writes THIS slot as if it were slot 0
    fake[6] = slot + 32;     // *(this+24): makes the internal size check == 1 (valid, non-empty)
    InterlockedExchange(&g_populateTid, (LONG)GetCurrentThreadId());
    InterlockedExchange(&g_populateIdx, (LONG)idx);
    ((fnSigninSync)(g_base + RVA_sub_772190))(fake);
    InterlockedExchange(&g_populateIdx, -1);
    InterlockedExchange(&g_populateTid, 0);
    BYTE* s = (BYTE*)slot;
    int ok = ((s[1] || s[0]) && !s[2]);
    Log("[signin] populate idx %u -> b0=%u b1=%u guest=%u  771D60=%d", idx, s[0], s[1], s[2], ok);
    return ok != 0;
}

// coopObj = *(dword_1DC2550 + 16): the split-screen/coop-state object. sub_6A8780() (the gate that
// lets sub_74F1D0 bind the 2nd player's pad) == coopObj[60] && coopObj[61]. coopObj[0xB8] is the
// already-open local-player-count gate (sub_673CB0).
static DWORD GetCoopObj() {
    DWORD owner = *(DWORD*)(g_base + RVA_signinOwner); if (owner <= 0x10000) return 0;
    DWORD c = *(DWORD*)(owner + 16); return (c > 0x10000) ? c : 0;
}
// Input context the per-frame pump (sub_753010/sub_752E80) operates on: *(*(dword_1DBDD4C+8)+0xA8).
// Its +0x168 holds the 2nd player's controller index (-1 = none => player-2 input pump is skipped).
static DWORD GetInputContext() {
    DWORD mgr = *(DWORD*)(g_base + RVA_mgrSingleton); if (mgr <= 0x10000) return 0;
    DWORD p = *(DWORD*)(mgr + 8); if (p <= 0x10000) return 0;
    DWORD c = *(DWORD*)(p + 0xA8); return (c > 0x10000) ? c : 0;
}

static bool g_coopFix = true;    // call the game's real coop activation at join ("nocoopfix.txt" disables)
static bool g_coopActive = true; // make the engine consider co-op ACTIVE ("nocoopactive.txt" disables)

// sub_683770(coopObj) == (coop[60] && coop[62]) || coop[148]  -- "co-op is ACTIVE".
// sub_6A87A0() is just this on the singleton. Fallback patch: make it return TRUE outright.
// Used only if coop[148] cannot be set safely (see RunDeferredJoin).
static const uintptr_t RVA_COOPACTIVE = 0x00283770;
static const BYTE ORIG_COOPACTIVE[6] = {0x80, 0x79, 0x3C, 0x00, 0x74, 0x06}; // cmp [ecx+3Ch],0 / jz
static const BYTE NEW_COOPACTIVE[6]  = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}; // mov eax,1 / retn

static bool ApplyBytes(BYTE* base, uintptr_t rva, const BYTE* orig, size_t guardN,
                       const BYTE* patch, size_t patchN, const char* name);
static bool PtrOk(DWORD p, SIZE_T n);

// ---------------------------- COUCH GAME MODE (coop + 0xB8) ----------------------------
// The coop singleton carries a GAME MODE word that nothing in this project had found:
//     GetMultiplayerGameMode() = sub_6A87D0() = *(DWORD*)(coop + 0xB8)
//     IsInCouchGame()          = sub_6A8A90() = (mode == 1)
// Found via the script API table in sub_6A6A70, which also CORRECTS two names these notes had
// wrong for three sessions:
//     sub_6A87A0 = IsInLiveGame   (not "coop active")
//     sub_6A8760 = IsClient       (not "the save gate")
// That second one finally explains the build-35 crash: coop[62] made IsClient() true, so the
// game believed it was a NETWORK CLIENT and the content-package/save system went looking for a
// host's data. It was never really about saving.
//
// Why the mode matters here: the engine branches on mode==1 specifically when it is dealing with
// the SECOND hero. sub_6BCC20 is the clearest case --
//     if (entity == secondHero) { if (GetGameMode() != 1) { ...remote-player path... } else ... }
// so with mode 0 our hero 2 is handled as a REMOTE hero. That is consistent with the remote-hero
// script rules it carried (001D0080) and with couch-partner targeting never considering it.
// The engine's own name for player 2 is the "couch partner" / "henchman":
//     IsHeroWithinInteractionDistance     (sub_812640) -> sub_6BCB80 = hero at mgr+0xEC
//     IsHenchmanWithinInteractionDistance (sub_812670) -> sub_6BCBA0 = hero at mgr+0xF4
//     SetClientOrCouchPartnerCanTarget / ClientOrCouchPartnerCanTarget (Targeted component +44)
//
// The mode is set by the engine INSIDE the join we already call: sub_BBFA10 @0xBC00FA does
// sub_673C90(1), guarded on the freshly spawned hero entity. Nothing else in the binary writes
// +0xB8 (verified by scanning every store encoding to disp32 0xB8 in .text). So either that
// guard fails for us, or the mode is set and this is a dead end -- which is exactly what the
// logging below decides, without guessing.
//
// SAFETY: +0xB8 does NOT feed sub_6A8760 (IsClient), so this cannot repeat the build-35 crash.
// Every mode==1 branch found does strictly LESS work: sub_684620 and sub_684A20 skip their
// online-activation calls, sub_10761F0 skips the interaction-mode camera focus.
typedef int (__stdcall *fnSetGameMode)(int);   // sub_673C90 -- disasm-verified: retn 4, arg on stack
static const uintptr_t RVA_sub_673C90 = 0x00273C90;
static bool g_couchMode = true;   // "nocouchmode.txt" disables

static bool SafeSetGameMode(int mode) {   // isolated so __try needs no C++ unwinding
    __try {
        ((fnSetGameMode)(g_base + RVA_sub_673C90))(mode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[couch] sub_673C90 FAULTED (0x%x)", (unsigned)GetExceptionCode());
        return false;
    }
}

// The game's real "enter local co-op" activation. sub_684650(coopObj) sets coop[60]=1, coop[61]=1,
// coop[62]=0 AND calls sub_6840C0(1) -> coop[178]=1 plus the coop camera/viewport wiring, and flips
// the coop-active global. Our spawn historically hand-poked only coop[60]/[61], so the engine never
// actually entered coop mode -- every subsystem that gates on coop-active (interaction present, UI,
// pause, reactions) ignored player 2 while movement (pure entity control) still worked. This is the
// missing step. Its guard runs only while coop[60]||coop[61] is still 0, so it must be called BEFORE
// those flags are set. __thiscall(coopObj) -> __fastcall(coopObj, edx).
typedef void (__fastcall *fnCoopActivate)(void* coop, void* edx);
static const uintptr_t RVA_sub_684650 = 0x00284650;
static bool SafeCoopActivate(DWORD coop) {   // isolated so __try needs no C++ unwinding
    __try {
        ((fnCoopActivate)(g_base + RVA_sub_684650))((void*)coop, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[coop] sub_684650 FAULTED (0x%x)", (unsigned)GetExceptionCode());
        return false;
    }
}

static void RunDeferredJoin(unsigned idx) {
    // Hero 2's input processes are constructed inside sub_BC0500 below. Do not probe them while
    // that is in flight -- a half-built instance holds garbage in this+12. 12s of quiet covers the
    // join plus the tail of component wiring; the probe re-arms itself afterwards.
    InterlockedExchange(&g_probeQuietUntil, (LONG)(GetTickCount() + 12000));
    if (HeroExistsForIndex((LONG)idx)) { Log("[join] pad %u already owns a hero; skip", idx); return; }
    g_joinTid = GetCurrentThreadId();                // arm watchdog (in case anything stalls)
    InterlockedExchange(&g_joinPhase, 1);
    // Make controller `idx` a real signed-in local user BEFORE the join, so sub_BBFA10 skips the
    // sign-in warning and runs its full native coop path (spawn + per-player input bind).
    PopulateSigninSlot(idx);
    // Open the split-screen input gate BEFORE the join so sub_74F1D0 (inside sub_BBFA10) binds the
    // joining pad to player 2 -- the PC build sets these flags in sub_68ED30, which runs after the
    // bind, so the native bind always saw them false.
    DWORD coop = GetCoopObj();
    if (coop) {
        Log("[coop] pre-activate coopObj=%08x  60=%u 61=%u 62=%u 148=%08x 178=%u 169=%u 155=%u gate[B8]=%d",
            coop, *(BYTE*)(coop + 60), *(BYTE*)(coop + 61), *(BYTE*)(coop + 62),
            *(DWORD*)(coop + 148), *(BYTE*)(coop + 178), *(BYTE*)(coop + 169),
            *(BYTE*)(coop + 155), *(int*)(coop + 0xB8));
        if (g_coopFix) {
            // Enter coop mode via the game's own activation (must run BEFORE we poke 60/61, or its
            // guard short-circuits). Sets 60/61/62, coop[178]=1, coop camera. SEH-guarded.
            if (SafeCoopActivate(coop)) Log("[coop] sub_684650 (enter coop mode) invoked");
            Log("[coop] post-activate  60=%u 61=%u 62=%u 148=%08x 178=%u",
                *(BYTE*)(coop + 60), *(BYTE*)(coop + 61), *(BYTE*)(coop + 62),
                *(DWORD*)(coop + 148), *(BYTE*)(coop + 178));
        }
        // Safety: ensure the pad-bind gate flags are set regardless of whether sub_684650 ran.
        *(volatile BYTE*)(coop + 60) = 1;
        *(volatile BYTE*)(coop + 61) = 1;
        // ---------------- THE REAL GATE: coop-active ----------------
        // sub_683770(coop) == (coop[60] && coop[62]) || coop[148], and sub_6A87A0() is exactly that
        // on the singleton. It was FALSE for us (62=0, 148=0), and hundreds of interaction/UI/hero
        // functions gate on it -- which is why every player-2 action except movement no-ops.
        //
        // coop[62] is NOT the way to set it. coop[62] also feeds sub_6A8760() (= coop[60]&&coop[62]),
        // and sub_7521F0 -- CSaveLoadManager::CContentPackageHandle -- branches on sub_6A8760(). That
        // is precisely what produced "Saving..." then CRASH in build 35: the game went looking for
        // player 2's save data, which our synthetic guest does not have. coop[62] stays 0.
        //
        // coop[148] is the engine's own JOINED-CO-OP-PLAYER COUNT. sub_68F950 does "++coop[148]" when
        // a client join is confirmed (and rejects past 0x80); sub_68EF70 does "--coop[148]" on leave.
        // It feeds coop-active but NOT sub_6A8760 -> no save-system wakeup. Setting it to 1 states the
        // literal truth for us: one other player has joined.
        //
        // Cost: coop[148] != 0 also opens the co-op packet bus -- sub_686DE0 and sub_687640 gate their
        // sends on it and enqueue onto the two std::lists at coop+292 and coop+320 (torn down by
        // sub_69BBC0 in sub_68EF70). Those are members, so they should be constructed even with no
        // session, but this build verifies that before trusting it rather than assuming.
        *(volatile BYTE*)(coop + 62) = 0;   // explicit: never wake the save system
        if (g_coopActive) {
            DWORD h292 = 0, h312 = 0, h320 = 0, h340 = 0;
            bool listsOk = false;
            if (PtrOk(coop + 292, 4) && PtrOk(coop + 340, 4)) {
                h292 = *(DWORD*)(coop + 292); h312 = *(DWORD*)(coop + 312);
                h320 = *(DWORD*)(coop + 320); h340 = *(DWORD*)(coop + 340);
                listsOk = PtrOk(h292, 8) && PtrOk(h312, 8) && PtrOk(h320, 8) && PtrOk(h340, 8);
            }
            Log("[coop] packet-queue heads 292=%08x 312=%08x 320=%08x 340=%08x -> %s",
                h292, h312, h320, h340, listsOk ? "VALID" : "NOT-INITIALISED");
            if (listsOk) {
                *(volatile DWORD*)(coop + 148) = 1;
                Log("[coop] coop[148]=1 (one joined player) -> coop-active TRUE; save gate 6A8760 stays FALSE (60=%u 62=%u 148=%u)",
                    *(BYTE*)(coop + 60), *(BYTE*)(coop + 62), *(DWORD*)(coop + 148));
            } else {
                // Same coop-active result, but the packet bus reads coop[148] directly and so stays shut.
                if (ApplyBytes(g_base, RVA_COOPACTIVE, ORIG_COOPACTIVE, sizeof(ORIG_COOPACTIVE),
                               NEW_COOPACTIVE, sizeof(NEW_COOPACTIVE), "sub_683770 -> always coop-active"))
                    Log("[coop] fell back to patching sub_683770; packet bus left closed");
            }
        }
        Log("[coop] final  60=%u 61=%u 62=%u 148=%u 155=%u 157=%u 178=%u",
            *(BYTE*)(coop + 60), *(BYTE*)(coop + 61), *(BYTE*)(coop + 62),
            *(DWORD*)(coop + 148), *(BYTE*)(coop + 155), *(BYTE*)(coop + 157),
            *(BYTE*)(coop + 178));
        // ---- COUCH MODE: declare this a couch game BEFORE the join (see the block at g_couchMode).
        // Set it first so the join runs its couch branches rather than its remote-hero ones; the
        // engine's own ordering sets it mid-join, but sub_684620's live-activation path is guarded
        // on `mode != 1`, so having it already true here also suppresses the online path we do not
        // want. Logged either side so a null result still tells us whether the join sets it itself.
        if (g_couchMode) {
            int before = *(volatile int*)(coop + 0xB8);
            if (before == 1) {
                Log("[couch] game mode already 1 (couch) before join -- nothing to do");
            } else if (SafeSetGameMode(1)) {
                Log("[couch] game mode %d -> %d via sub_673C90(1): IsInCouchGame() now %s",
                    before, *(volatile int*)(coop + 0xB8),
                    (*(volatile int*)(coop + 0xB8) == 1) ? "TRUE" : "STILL FALSE");
            }
        }
    }
    // Enable the game's NATIVE per-player input bind. sub_BBFA10's post-spawn block at 0xBC0434 runs
    //   if ( !session_is_online() && byte_1C86BDB && !hero_preexisted )  ->  sub_752CD0(idx)
    // sub_752CD0(idx) -> sub_6B6C00(mgr,idx) finds the hero whose +52==idx (hero 2) and binds its
    // input context (hero+64) to controller `idx`. THIS is the "detach pad from P1, attach to P2"
    // routing. It does NOT re-check sign-in, so it runs even though player 2 is only a local guest.
    //
    // NB: earlier notes marked this a crash. The couchcoop.log evidence disproves that -- every run
    // that set the flag still reached "[heroes] count=2 +52=1" and "sub_BC0500 returned". The prior
    // "[hang]" dumps were all the *dismissable* sign-in warning box (sub_7718B0), not a fault.
    volatile BYTE* localCoop = (volatile BYTE*)(g_base + RVA_b_localCoop);
    BYTE prev = *localCoop; *localCoop = 1;
    Log("[join] tid=%u byte_1C86BDB %u->1  sub_BC0500(%u) [native bind ON]", g_joinTid, prev, idx);
    int hero = ((fnJoinCouch)(g_base + 0x007C0500))(idx);
    InterlockedExchange(&g_joinPhase, 0);
    Log("[join] sub_BC0500 returned hero=%08x", (unsigned)hero);
    // Did the join's own sub_673C90(1) at 0xBC00FA run? If g_couchMode is OFF this reads the
    // engine's unaided answer, which is the whole question. If it is ON and this says 0, then
    // something in the join actively CLEARED it and that is a different bug worth knowing about.
    {
        DWORD c2 = GetCoopObj();
        if (c2 && PtrOk(c2 + 0xB8, 4))
            Log("[couch] post-join game mode = %d  (IsInCouchGame=%s, couchmode fix %s)",
                *(volatile int*)(c2 + 0xB8),
                (*(volatile int*)(c2 + 0xB8) == 1) ? "TRUE" : "FALSE",
                g_couchMode ? "ON" : "OFF");
    }
    // Verify / fallback: ensure the input context's 2nd-player pad field is set so sub_752E80 pumps
    // player 2. If the native bind set it, we just log; if not (-1), set it directly to `idx`.
    DWORD ctx = GetInputContext();
    if (ctx) {
        int cur = *(int*)(ctx + 0x168);
        Log("[coop] post-join context=%08x +0x168(2nd pad)=%d", ctx, cur);
        if (cur == -1) { *(volatile int*)(ctx + 0x168) = (int)idx; Log("[coop] forced context+0x168 = %u", idx); }
    }
    InterlockedExchange(&g_joinedFlag, 1); // mark for [queue] log correlation (pre/post join)
    // Leave byte_1C86BDB=1: local coop is genuinely active now (its setter sub_6163B0 is dead code,
    // so nothing else toggles it; only join/coop paths read it).
}

// The input POLL runs on a dedicated input thread; we record its id so we never run the join there.
static volatile LONG g_inputPollTid = 0;

// Primary executor: the game's drop-in handler sub_B9B0C0 is dispatched per input event on the MAIN
// thread (it is where the native sub_BC0500 join is called from -> proven safe thread/phase). We swap
// its callback pointer, forward to the original, then run any queued join right here.
typedef void (__thiscall *fnDropIn)(int thisptr, int a2);
static fnDropIn g_origDropIn = nullptr;
static void __fastcall Hook_DropIn(int thisptr, int /*edx*/, int a2) {
    if (g_origDropIn) g_origDropIn(thisptr, a2);          // preserve native behavior
    // Run the queued spawn from the game's own drop-in dispatch (same context it natively uses).
    if (g_pendingJoin >= 0) {
        LONG idx = InterlockedExchange(&g_pendingJoin, -1);
        if (idx >= 0) RunDeferredJoin((unsigned)idx);
    }
}

// -------------------- D3D9 Present: main-thread, per-frame executor --------------------
// Our other hooks (XInput poll, drop-in dispatch) run on the input thread, where the native join
// stalls/crashes. IDirect3DDevice9::Present runs on the main render thread once per frame -- the
// correct place to run the join. We hook it via COM vtable-pointer swaps (no code patching):
//   Direct3DCreate9 -> IDirect3D9::CreateDevice[16] -> IDirect3DDevice9::Present[17].
typedef long (__stdcall *fnPresent)(void* dev, const void*, const void*, void*, const void*);
typedef long (__stdcall *fnCreateDevice)(void* self, unsigned, unsigned, void*, unsigned long, void*, void**);
typedef void* (__stdcall *fnD3DCreate9)(unsigned);
static fnPresent      g_origPresent      = nullptr;
static fnCreateDevice g_origCreateDevice = nullptr;
static fnD3DCreate9   g_origD3DCreate9   = nullptr;

static void SwapVtbl(void* obj, int index, void* hook, void** saveOrig) {
    void** vtbl = *(void***)obj;
    DWORD old;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old)) {
        if (saveOrig && !*saveOrig) *saveOrig = vtbl[index];
        vtbl[index] = hook;
        VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    }
}

static long __stdcall Hook_Present(void* dev, const void* a, const void* b, void* c, const void* d) {
    if (g_pendingJoin >= 0) {
        LONG idx = InterlockedExchange(&g_pendingJoin, -1);
        if (idx >= 0) { Log("[d3d] Present tid=%u -> run join", GetCurrentThreadId()); RunDeferredJoin((unsigned)idx); }
    }
    return g_origPresent(dev, a, b, c, d);
}
static long __stdcall Hook_CreateDevice(void* self, unsigned adapter, unsigned devtype, void* focus,
                                        unsigned long flags, void* pp, void** ppDevice) {
    long hr = g_origCreateDevice(self, adapter, devtype, focus, flags, pp, ppDevice);
    if (hr >= 0 && ppDevice && *ppDevice && !g_origPresent) {
        SwapVtbl(*ppDevice, 17, (void*)&Hook_Present, (void**)&g_origPresent);   // Present = vtbl[17]
        Log("[d3d] device created -> Present hooked (orig=%08x)", (unsigned)(uintptr_t)g_origPresent);
    }
    return hr;
}
static void* __stdcall Hook_D3DCreate9(unsigned sdk) {
    void* d3d9 = g_origD3DCreate9 ? g_origD3DCreate9(sdk) : nullptr;
    if (d3d9 && !g_origCreateDevice) {
        SwapVtbl(d3d9, 16, (void*)&Hook_CreateDevice, (void**)&g_origCreateDevice);  // CreateDevice = vtbl[16]
        Log("[d3d] Direct3DCreate9 -> CreateDevice hooked");
    }
    return d3d9;
}

// The engine's own "are we in a live session" predicates, used to gate the synthetic join (see the
// comment at the trigger). Both are no-arg and read only globals, so they are safe to call from the
// input thread.
typedef int (__cdecl *fnPred0)(void);
static const uintptr_t RVA_sub_674320 = 0x00274320;   // world/session ready
static const uintptr_t RVA_sub_674340 = 0x00274340;   // that AND the session check sub_BC0500 uses
// "nojoin.txt" hands the Start press back to the game instead of running our synthetic join. Use it
// to test whether the PC build has its own couch-join flow (e.g. at the new-game character select)
// that our trigger has been swallowing all along.
static bool g_joinTrigger = true;

// Diagnostic: observe the game's own controller polling. Proves whether our multi-pad
// enumeration made the game poll index 1+, and whether Start on pad 2 lands on a non-zero index.
static LONG g_xiPolled[4]  = {0,0,0,0};
static LONG g_startLogged[4] = {0,0,0,0};
static DWORD WINAPI Hook_GameXInputGetState(DWORD idx, XI_STATE* st) {
    DWORD r = g_origGameXI ? g_origGameXI(idx, st) : (DWORD)ERROR_DEVICE_NOT_CONNECTED;
    InterlockedExchange(&g_inputPollTid, (LONG)GetCurrentThreadId()); // remember the input-poll thread
    // ---- [phase] heartbeat ----------------------------------------------------------------
    // The watchdog's [coopnow]/[heroes] logging only runs once a world exists, so a session that
    // black-screens or never finishes loading produced ~2800 log lines telling us NOTHING about
    // what phase the game was in. This ticks from the input poll, which always runs.
    //   674320 = world/session ready, 674340 = live session, pending = the game's own join slot.
    // If the screen goes black and 674340 stays 0, the LOAD never completed -- a different bug
    // from "loaded but rendered nothing", and worth distinguishing before theorising again.
    if (g_base) {
        static DWORD s_phaseTick = 0;
        static int   s_lastReady = -9, s_lastLive = -9, s_lastPend = -9;
        DWORD nowP = GetTickCount();
        if (nowP - s_phaseTick > 2000) {
            s_phaseTick = nowP;
            int ready = 0, live = 0;
            __try {
                ready = ((fnPred0)(g_base + RVA_sub_674320))() ? 1 : 0;
                live  = ((fnPred0)(g_base + RVA_sub_674340))() ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { ready = live = -1; }
            int pend = GetPendingCoopJoin();
            if (ready != s_lastReady || live != s_lastLive || pend != s_lastPend) {
                s_lastReady = ready; s_lastLive = live; s_lastPend = pend;
                Log("[phase] worldReady(674320)=%d  liveSession(674340)=%d  pendingJoin=%d",
                    ready, live, pend);
            }
        }
    }
    if (idx < 4) {
        if (InterlockedExchange(&g_xiPolled[idx], 1) == 0)
            Log("[xinput] game polled index %u -> %s", idx, r == ERROR_SUCCESS ? "CONNECTED" : "not-connected");
        WORD b = (r == ERROR_SUCCESS && st) ? st->Gamepad.wButtons : 0;
        bool startEdge = (b & XI_START) && !(g_prevButtons[idx] & XI_START); // fresh press
        g_prevButtons[idx] = b;
        // On each fresh Start from a non-primary pad, directly invoke the real couch-join
        // sub_BC0500(idx) -- the same function the in-game drop-in handler (sub_B9B0C0) calls.
        // This runs on the game's input-poll thread (the frame's input phase). Skip if that pad
        // already owns a hero (avoid duplicate spawns).
        if (idx >= 1 && startEdge && g_base) {
            // REVERTED to the build-38 behaviour, deliberately.
            //
            // Builds 39-42 gated this join to in-world only and then chased "why does the game
            // refuse to join at character select". It never refused. The prompt used to disappear
            // because THIS join spawned hero 2 on the spot, and the game's prompt is satisfied by a
            // second hero existing. The only real bug was, and is, the BLACK SCREEN AFTER CHARACTER
            // CREATION. Gating this away removed the one thing that worked.
            //
            // Also removed with it: the menu sign-in (build 41) and the pending-slot write (build
            // 42). Both were answers to a question nobody asked, and the sign-in visibly changed
            // the button glyph in the game's own prompt -- it was altering what profile/device the
            // game believed was present. Neither is needed: RunDeferredJoin does its own
            // PopulateSigninSlot. The engine's pending-join slot (§20.8) is still correctly
            // identified and still worth trying LATER, as a possible cure for the black screen --
            // but as one change at a time, not stacked on top of everything else.
            //
            // The [phase] heartbeat above stays: it is pure observation and is what will actually
            // diagnose the black screen.
            if (HeroExistsForIndex((LONG)idx)) {
                Log("[trigger] pad %u already owns a hero; skip", idx);
            } else if (InterlockedCompareExchange(&g_pendingJoin, (LONG)idx, -1) == -1) {
                // Do NOT call the join here -- we're on the input thread with the device lock held.
                // Hand it to the main thread (see Hook_WaitForSingleObject / RunDeferredJoin).
                Log("[trigger] Start edge pad %u -> queued join for main thread  (input tid=%u)",
                    idx, GetCurrentThreadId());
            }
        }
    }
    // NOTE: the per-frame field-poke driver (DriveHero2FromPad1) is DISABLED. It could write into
    // hero 2's component after that pointer went stale (entity torn down) -> heap corruption / hang,
    // and it can never deliver full input anyway. The correct fix routes pad N -> hero N at the
    // game's own input-binding layer; see the co-op input-context work below.
    return r;
}

static LONG g_privLogged[4] = {0,0,0,0};
static DWORD WINAPI Hook_XUserCheckPrivilege(DWORD idx, DWORD priv, BOOL* pfResult) {
    if (idx > 0 && idx < 4 && InterlockedExchange(&g_privLogged[idx], 1) == 0)
        Log("[hook] XUserCheckPrivilege(idx=%u, priv=%u)", idx, priv);
    return g_origPriv ? g_origPriv(idx, priv, pfResult) : (DWORD)E_FAIL;
}

// Log + forward so XLLN runs its auto-login (signs in Eden=0 and Liah=1).
static DWORD WINAPI Hook_XShowSigninUI(DWORD cPanes, DWORD dwFlags) {
    Log("[hook] XShowSigninUI(cPanes=%u, dwFlags=%u)  <-- game requested sign-in", cPanes, dwFlags);
    return g_origShow ? g_origShow(cPanes, dwFlags) : ERROR_SUCCESS;
}

// ------------------------------------------------------------------ install helpers
static void HookIATSlot(BYTE* base, uintptr_t rva, void* hook, void** savedOrig) {
    void** slot = reinterpret_cast<void**>(base + rva);
    DWORD old;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *savedOrig = *slot;      // original xlive/XLLN pointer (for index-0 forwarding)
        *slot = hook;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
}

// --- Sign-in gate patches so the native join proceeds instead of hanging on a sign-in warning box ---
// sub_7718B0 = the XShowMessageBoxUI sign-in warning; XLLN can't complete it -> the join spins forever.
// It is called ONLY from the join (sub_BBFA10, 2 sites), so force it to return 1 ("continue") globally.
// NOTE: guard only bytes 0..5 -- byte 4..5 are the low word of a relocated absolute operand
// (mov eax, dword_1D81668); ASLR relocation is a multiple of 0x10000 so the low word is stable,
// but bytes 6..7 change at load time. We still overwrite all 8 bytes.
static const BYTE ORIG_7718B0[6] = {0x83,0xEC,0x58,0xA1,0x68,0x16};
static const BYTE NEW_7718B0[8]  = {0xB8,0x01,0x00,0x00,0x00,0xC2,0x08,0x00}; // mov eax,1 ; ret 8
// The bind gate: sub_BBFA10 @0xBBFB2D `call sub_771D60` (is-signed-in) gates the native input bind.
// sub_771D60 has 30+ callers so we do NOT patch it globally -- only force THIS call site to true.
static const BYTE ORIG_BINDGATE[5] = {0xE8,0x2E,0x22,0xBB,0xFF};
static const BYTE NEW_BINDGATE[5]  = {0xB0,0x01,0x90,0x90,0x90};              // mov al,1 ; nop;nop;nop

// guardN bytes are compared (must be relocation-stable); patchN bytes are written.
static bool ApplyBytes(BYTE* base, uintptr_t rva, const BYTE* orig, size_t guardN,
                       const BYTE* patch, size_t patchN, const char* name) {
    BYTE* p = base + rva;
    if (memcmp(p, orig, guardN) != 0) {
        if (memcmp(p, patch, patchN) == 0) { Log("[patch] %s: already applied", name); return true; }
        Log("[patch] %s: REFUSED (cur %02x %02x %02x %02x %02x %02x)", name, p[0], p[1], p[2], p[3], p[4], p[5]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, patchN, PAGE_EXECUTE_READWRITE, &old)) { Log("[patch] %s: VirtualProtect failed", name); return false; }
    memcpy(p, patch, patchN);
    VirtualProtect(p, patchN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, patchN);
    Log("[patch] %s: applied", name);
    return true;
}

static bool ApplyInputPatch(BYTE* base) {
    BYTE* p = base + RVA_INPUT_PATCH;
    Log("[patch] site=%08x  cur=%02x %02x %02x %02x %02x  (expect 38 5D 01 74 36)",
        (unsigned)(uintptr_t)p, p[0], p[1], p[2], p[3], p[4]);
    if (memcmp(p, PATCH_ORIG, sizeof(PATCH_ORIG)) != 0) {
        if (memcmp(p, PATCH_NEW, sizeof(PATCH_NEW)) == 0) Log("[patch] already applied");
        else Log("[patch] REFUSED: original bytes do not match this build");
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, sizeof(PATCH_NEW), PAGE_EXECUTE_READWRITE, &old)) { Log("[patch] VirtualProtect failed"); return false; }
    memcpy(p, PATCH_NEW, sizeof(PATCH_NEW));
    VirtualProtect(p, sizeof(PATCH_NEW), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(PATCH_NEW));
    Log("[patch] applied OK (multi-pad enumeration)");
    return true;
}

// Watch the join-intent globals so we can see whether pressing Start records a pending
// player-2 join and whether the engine consumes it (resets to -1). Read-only.
static DWORD WINAPI Watch(LPVOID base) {
    volatile LONG* primary = (volatile LONG*)((BYTE*)base + RVA_g_primaryPad);
    volatile LONG* pending = (volatile LONG*)((BYTE*)base + RVA_g_pendingPad);
    volatile BYTE* coop    = (volatile BYTE*)((BYTE*)base + RVA_b_coopActive);
    LONG lp = 0x7FFFFFFF, lpr = 0x7FFFFFFF; BYTE lc = 0xFF;
    int lastHeroCount = -999;
    for (;;) {
        LONG pr = *primary, pn = *pending; BYTE c = *coop;
        if (pr != lpr) { Log("[watch] primary pad idx (1C86BE4) = %d", pr); lpr = pr; }
        if (pn != lp)  { Log("[watch] pending guest idx (1C86BE8) = %d", pn); lp = pn; }
        if (c  != lc)  { Log("[watch] coop-active (1C86BDA) = %u", c); lc = c; }
        // Walk the hero vector; log each hero's +52 (owning controller index) when the count changes.
        DWORD mgr = *(DWORD*)((BYTE*)base + RVA_mgrSingleton);
        if (mgr > 0x10000) {
            DWORD a = *(DWORD*)(mgr + 0x0C);
            if (a > 0x10000) {
                DWORD pl = *(DWORD*)(a + 0x40);
                if (pl > 0x10000) {
                    DWORD begin = *(DWORD*)(pl + 0x0C), end = *(DWORD*)(pl + 0x10);
                    if (begin > 0x10000 && end >= begin && (end - begin) % 12 == 0 && (end - begin) <= 12 * 8) {
                        int count = (int)((end - begin) / 12);
                        if (count != lastHeroCount) {
                            lastHeroCount = count;
                            Log("[heroes] count=%d", count);
                            for (int k = 0; k < count; k++) {
                                DWORD hero = *(DWORD*)(begin + k * 12 + 4);
                                if (hero > 0x10000)
                                    Log("[heroes]  hero[%d]=%08x  +52(ctrlIdx)=%d", k, hero, *(int*)(hero + 52));
                            }
                        }
                    }
                }
            }
        }
        Sleep(40);
    }
}

// ================= INPUT EVENT QUEUE: the merge point =================
// sub_188EFB0 (CInputManagerDX per-frame poll driver) hands ONE shared queue -- inputManager+44 --
// to EVERY device, so pad0/pad1/keyboard/mouse all push into a single stream:
//     for (i = mgr[3]; i != mgr[4]; i += 2)  device->Poll(*i, mgr+11 /*=mgr+44*/, time);
// Each event is 72 bytes and KEEPS its source pad index at +0x28 (stamped by sub_188C110 and
// explicitly propagated by sub_188A1F0 @0x188A2F0). The downstream binding matcher just never
// consults that tag -- which is why pad 2 drives player 1 and the menus.
//
// Queue layout (queue = mgr+44; fields from sub_5CA020's a2[2]/a2[3]/a2[4]):
//   begin = *(mgr+52)   end = *(mgr+56)   capacityEnd = *(mgr+60)   count = (end-begin)/72
static const unsigned EVT_SIZE = 72;
static const unsigned EVT_CTRL = 0x28;   // source controller index
static const unsigned EVT_TYPE = 0x2C;   // event type (0x0D/0x0E/0x0F/9/10/11/12/15...)
static const uintptr_t RVA_sub_188EFB0   = 0x0148EFB0; // poll driver (tail-jmp target)
static const uintptr_t RVA_JMP_POLLDRV   = 0x0148DDFD; // `jmp sub_188EFB0` inside sub_188DDF0
static const uintptr_t RVA_sub_752E80    = 0x00352E80; // player-2 input pump
static const uintptr_t RVA_CALL_PUMP2_A  = 0x00353373; // `call sub_752E80` in sub_753010
static const uintptr_t RVA_CALL_PUMP2_B  = 0x00353D79; // `call sub_752E80` in sub_753A80

static volatile LONG g_inputMgr = 0;     // captured CInputManagerDX (owner of the shared queue)

typedef void (__thiscall *fnPollDriver)(void* mgr);
typedef int  (__thiscall *fnPump2)(void* ctx, char a2, char a3, char a4);
static fnPollDriver g_origPollDriver = nullptr;
static fnPump2      g_origPump2      = nullptr;

// Read the queue bounds; returns event count (0 if unavailable).
static unsigned QueueInfo(DWORD mgr, DWORD* pBegin) {
    if (mgr <= 0x10000) return 0;
    DWORD begin = *(DWORD*)(mgr + 52), end = *(DWORD*)(mgr + 56);
    if (begin <= 0x10000 || end < begin) return 0;
    DWORD bytes = end - begin;
    if (bytes % EVT_SIZE || bytes > EVT_SIZE * 4096) return 0;
    if (pBegin) *pBegin = begin;
    return bytes / EVT_SIZE;
}

// ---- Guard-page probe: identify WHO consumes the input event queue ----
// The decompiler already misled us once (the sub_74xxxx/sub_75xxxx "input pump" was really
// CSaveLoadManager), so find the consumer empirically instead: right after the poll fills the
// queue, mark its page PAGE_GUARD. The next access faults, and the VEH reports the exact faulting
// EIP -- that instruction IS the consumer. PAGE_GUARD is one-shot (it clears itself), so we simply
// log and continue; the instruction then re-executes normally.
#ifndef STATUS_GUARD_PAGE_VIOLATION
#define STATUS_GUARD_PAGE_VIOLATION ((DWORD)0x80000001L)
#endif
// RESULT: the probe caught sub_D89DE0+0x90 (VA 0xD89E70) -- see RVA_TAGWIPE below. Probe is now
// off by default; create "probe.txt" next to Fable3.exe to re-enable it for future hunts.
static bool g_probeEnabled = false;
static volatile LONG g_probeHits = 0;
static const LONG    PROBE_MAX_HITS = 40;
static void ArmQueueProbe(DWORD begin) {
    if (!g_probeEnabled) return;
    if (InterlockedCompareExchange(&g_probeHits, 0, 0) >= PROBE_MAX_HITS) return;
    DWORD old, page = begin & ~0xFFFu;
    VirtualProtect((LPVOID)page, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
}

// ============================ THE FIX ============================
// sub_D89DE0 walks every queued event and WIPES its source-pad tag before dispatch:
//     for each 72-byte event:  *(DWORD*)(event + 0x28) = 0;      // mov [ecx+28h], ebp   (ebp==0)
// That single store is why every pad collapses onto player 1 (and why any pad works in menus):
// downstream, all events claim to be controller 0. NOP it and each event keeps the pad that
// actually produced it, so the game's own per-controller routing can do its job.
// sub_D89DE0's per-event loop does BOTH of these with ebp==0:
//     d89e70   mov [ecx+28h], ebp      ; wipe the event's source-pad tag
//     d89e7c   mov [esp+20h], ebp      ; file the event under bucket key 0
//     d89e80   call sub_D92EA0         ; (bucket lookup/create)
// Meanwhile sub_B6DD20 dispatches per listener by ITS controller:
//     v18 = entity->field_52 ; sub_D89480(v18, ...) ; sub_B6CF30(listener, ...)
// So player 1 (ctrl 0) finds bucket 0 and works, while hero 2 asks for bucket 1 and gets nothing.
// Fix: put the event's real pad in edx (dead from d89e66..d89e8c) and use it as the bucket key.
// ebp must stay 0 -- it is still used as the zero comparand at d89e88 (`cmp ebx, ebp`).
static const uintptr_t RVA_TAGWIPE = 0x00989E70;
static const BYTE ORIG_TAGWIPE[3] = {0x89, 0x69, 0x28};   // mov [ecx+28h], ebp
static const BYTE NEW_TAGWIPE[3]  = {0x8B, 0x51, 0x28};   // mov edx, [ecx+28h]   (edx = event's pad)

static const uintptr_t RVA_BUCKETKEY = 0x00989E7C;
static const BYTE ORIG_BUCKETKEY[4] = {0x89, 0x6C, 0x24, 0x20}; // mov [esp+20h], ebp  (key = 0)
static const BYTE NEW_BUCKETKEY[4]  = {0x89, 0x54, 0x24, 0x20}; // mov [esp+20h], edx  (key = pad)

// Compact the shared queue in place, keeping only events whose +0x28 == keepCtrl. Returns kept.
// The queue is a flat [begin,end) array of 72-byte records, so we can simply slide survivors down
// and rewrite `end` (*(mgr+56)).
static unsigned FilterQueueTo(DWORD mgr, int keepCtrl, unsigned* pDropped) {
    DWORD begin = 0;
    unsigned n = QueueInfo(mgr, &begin);
    if (pDropped) *pDropped = 0;
    if (!n) return 0;
    BYTE* w = (BYTE*)begin;
    unsigned kept = 0;
    for (unsigned k = 0; k < n; k++) {
        BYTE* e = (BYTE*)(begin + k * EVT_SIZE);
        if (*(int*)(e + EVT_CTRL) == keepCtrl) {
            if (w != e) memcpy(w, e, EVT_SIZE);
            w += EVT_SIZE;
            kept++;
        }
    }
    *(DWORD*)(mgr + 56) = (DWORD)(uintptr_t)w;   // new end
    if (pDropped) *pDropped = n - kept;
    return kept;
}

// CONFIRMED: dropping non-pad0 events removes pad 2 from hero 1 AND the menus, before and after the
// join -- so this queue is the stream feeding player 1. Now we STASH those events instead of
// dropping them, and replay them into player 2's pump on the main thread (see Hook_GameInput).
// NOW OFF BY DEFAULT: with the tag-wipe patch in place, pad-1 events must reach the dispatcher
// WITH their real tag, so we must NOT strip them here. Create "filterpad2.txt" next to Fable3.exe
// to fall back to the old drop-pad-1 behaviour (isolates pad 2 from player 1, but feeds nobody).
static bool g_filterQueue = false;

// --- pad-1 event stash (written on the input thread, replayed on the main thread) ---
static const unsigned STASH_MAX = 128;
static BYTE  g_stash[STASH_MAX * EVT_SIZE];
static volatile LONG g_stashCount = 0;
static CRITICAL_SECTION g_stashCs;
static bool g_stashCsReady = false;

// sub_753EC0: the game's per-frame input dispatcher, called once per frame from sub_65B950.
static const uintptr_t RVA_sub_753EC0   = 0x00353EC0;
static const uintptr_t RVA_CALL_GAMEINP = 0x0025B9A0; // `call sub_753EC0` in sub_65B950
typedef char (__thiscall *fnGameInput)(void* ctx);
static fnGameInput g_origGameInput = nullptr;

// Runs in place of the tail `jmp sub_188EFB0`. After the original poll, the queue holds this
// frame's events from ALL devices -- exactly where the per-pad tags are still intact.
static LONG g_qLogCount = 0;
static void __fastcall Hook_PollDriver(void* mgr, void* /*edx*/) {
    InterlockedExchange(&g_inputMgr, (LONG)(uintptr_t)mgr);
    g_origPollDriver(mgr);                       // original: poll every device into mgr+44
    DWORD m = (DWORD)(uintptr_t)mgr, begin = 0;
    unsigned n = QueueInfo(m, &begin);
    if (!n) return;
    unsigned cnt[5] = {0,0,0,0,0};               // tag histogram: ctrl0..3, other
    for (unsigned k = 0; k < n && k < 512; k++) {
        int c = *(int*)(begin + k * EVT_SIZE + EVT_CTRL);
        cnt[(c >= 0 && c < 4) ? c : 4]++;
    }
    bool interesting = (cnt[1] || cnt[2] || cnt[3]);
    unsigned dropped = 0;
    if (g_filterQueue && interesting) {
        // Stash the 2nd player's events for main-thread replay, then leave only pad 0 in the live
        // queue. Manager lock (mgr+76) held: sub_188EFB0 releases it before returning, so the
        // consumer could otherwise read mid-compaction.
        int pad2 = 1;
        DWORD ctxq = GetInputContext();
        if (ctxq) { int p = *(int*)(ctxq + 0x168); if (p >= 0 && p < 4) pad2 = p; }
        EnterCriticalSection((LPCRITICAL_SECTION)(m + 76));
        if (g_stashCsReady) {
            EnterCriticalSection(&g_stashCs);
            LONG sc = g_stashCount;
            for (unsigned k = 0; k < n; k++) {
                BYTE* e = (BYTE*)(begin + k * EVT_SIZE);
                if (*(int*)(e + EVT_CTRL) == pad2 && sc < (LONG)STASH_MAX)
                    memcpy(g_stash + (sc++) * EVT_SIZE, e, EVT_SIZE);
            }
            InterlockedExchange(&g_stashCount, sc);
            LeaveCriticalSection(&g_stashCs);
        }
        FilterQueueTo(m, 0, &dropped);
        LeaveCriticalSection((LPCRITICAL_SECTION)(m + 76));
    }
    // Log whenever the queue is non-empty (not just for non-pad0 events) -- otherwise, if the join
    // re-tags pad 2's events as ctrl0, we would see NOTHING and wrongly assume the filter still ran.
    // Also report the first event's type so we can tell sticks (0x0D/0x0E) from buttons.
    if ((InterlockedIncrement(&g_qLogCount) % 20) == 0) {
        int t0 = (n > 0) ? *(int*)(begin + EVT_TYPE) : -1;
        Log("[queue] joined=%d events=%u  ctrl0=%u ctrl1=%u ctrl2=%u ctrl3=%u other=%u  dropped=%u  type0=%d",
            (int)InterlockedCompareExchange(&g_joinedFlag, 0, 0),
            n, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], dropped, t0);
    }
    // Arm LAST -- after all of our own reads above -- so the first fault is the game's consumer.
    ArmQueueProbe(begin);
}

// Runs in place of `call sub_752E80` (player-2 pump) at both pump call sites. Tells us whether the
// player-2 branch executes at all in gameplay, and what it sees.
static LONG g_pump2Calls = 0;
static int __fastcall Hook_Pump2(void* ctx, void* /*edx*/, char a2, char a3, char a4) {
    LONG n = InterlockedIncrement(&g_pump2Calls);
    if (n <= 5 || (n % 600) == 0) {
        DWORD c = (DWORD)(uintptr_t)ctx;
        int pad2 = (c > 0x10000) ? *(int*)(c + 0x168) : -999;
        DWORD begin = 0;
        unsigned q = QueueInfo((DWORD)g_inputMgr, &begin);
        unsigned tagged = 0;
        for (unsigned k = 0; k < q && k < 512; k++)
            if (*(int*)(begin + k * EVT_SIZE + EVT_CTRL) == pad2) tagged++;
        Log("[pump2] call#%d ctx=%08x +0x168(pad)=%d  queue=%u events (%u tagged for pad %d)",
            n, c, pad2, q, tagged, pad2);
    }
    return g_origPump2(ctx, a2, a3, a4);
}

// Runs in place of `call sub_753EC0` in sub_65B950 -- the game's once-per-frame input dispatch, on
// the MAIN thread. After the game's own pass (which now only sees pad-0 events), replay the stashed
// pad-1 events into the shared queue and invoke the player-2 pump the game never calls.
static LONG g_replayLog = 0;
static char __fastcall Hook_GameInput(void* ctx, void* /*edx*/) {
    char r = g_origGameInput(ctx);                       // original per-frame input dispatch
    LONG sc = InterlockedCompareExchange(&g_stashCount, 0, 0);
    if (!g_filterQueue || !sc) return r;
    DWORD m = (DWORD)g_inputMgr, c = (DWORD)(uintptr_t)ctx;
    if (m <= 0x10000 || c <= 0x10000) return r;

    // sub_752E80's own guards -- if these fail it returns immediately and hero 2 stays dead.
    DWORD cmds = *(DWORD*)(c + 4);
    int   cmdN = cmds > 0x10000 ? *(int*)(cmds + 4) : -1;
    int   pad2 = *(int*)(c + 0x168);
    if ((InterlockedIncrement(&g_replayLog) % 120) == 1)
        Log("[replay] stash=%d ctx=%08x +4=%08x cmdCount=%d +0x168=%d", sc, c, cmds, cmdN, pad2);
    if (cmds <= 0x10000 || cmdN <= 0 || pad2 < 0) { InterlockedExchange(&g_stashCount, 0); return r; }

    DWORD begin = *(DWORD*)(m + 52), capEnd = *(DWORD*)(m + 60);
    if (begin <= 0x10000 || capEnd < begin) { InterlockedExchange(&g_stashCount, 0); return r; }
    unsigned capacity = (capEnd - begin) / EVT_SIZE;
    if ((unsigned)sc > capacity) sc = (LONG)capacity;

    EnterCriticalSection((LPCRITICAL_SECTION)(m + 76));
    DWORD savedEnd = *(DWORD*)(m + 56);                  // preserve whatever the game left queued
    EnterCriticalSection(&g_stashCs);
    memcpy((void*)begin, g_stash, (size_t)sc * EVT_SIZE); // present ONLY pad-1 events
    InterlockedExchange(&g_stashCount, 0);
    LeaveCriticalSection(&g_stashCs);
    *(DWORD*)(m + 56) = begin + (DWORD)sc * EVT_SIZE;
    LeaveCriticalSection((LPCRITICAL_SECTION)(m + 76));

    // !! DO NOT CALL sub_752E80 HERE !!
    // sub_752E80 is the player-2 SAVE routine, not an input pump. Invoking it per-frame spammed the
    // game's "Saving..." indicator. The whole sub_74xxxx/sub_75xxxx cluster is CSaveLoadManager
    // (strings "HeroSave"/"SaveName"/"JourneyName"/"WriteSaveGame"), which is why sub_771D60
    // (signed-in) gates it -- you need a profile to save to. See HANDOFF §9 correction.

    EnterCriticalSection((LPCRITICAL_SECTION)(m + 76));
    *(DWORD*)(m + 56) = savedEnd;                        // restore the queue end
    LeaveCriticalSection((LPCRITICAL_SECTION)(m + 76));
    return r;
}

// Repoint a rel32 call/jmp at `rva` to `newTarget`. Guarded: verifies the opcode AND that the
// existing operand resolves to `expectTarget`, so it refuses on a different build.
static bool PatchRel32(BYTE* base, uintptr_t rva, BYTE opcode, void* expectTarget,
                       void* newTarget, const char* name) {
    BYTE* p = base + rva;
    if (p[0] != opcode) {
        Log("[hook] %s: REFUSED (opcode %02x, expected %02x)", name, p[0], opcode);
        return false;
    }
    BYTE* oldTgt = p + 5 + (INT32)(*(DWORD*)(p + 1));
    if (oldTgt != (BYTE*)expectTarget) {
        if (oldTgt == (BYTE*)newTarget) { Log("[hook] %s: already patched", name); return true; }
        Log("[hook] %s: REFUSED (target %08x, expected %08x)", name,
            (unsigned)(uintptr_t)oldTgt, (unsigned)(uintptr_t)expectTarget);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[hook] %s: VirtualProtect failed", name); return false;
    }
    *(DWORD*)(p + 1) = (DWORD)((BYTE*)newTarget - (p + 5));
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("[hook] %s: patched -> %08x", name, (unsigned)(uintptr_t)newTarget);
    return true;
}

// ================= ACTION-PATH CENSUS (READ-ONLY) =================
// Movement works on pad 1 but no button does. Static analysis says the whole binding layer IS
// pad-aware, so the loss must be measurable at one of exactly two places. sub_B6DD20 (the per-frame
// input update) does both of them, one after the other:
//
//   b6dda0  mov ecx,[esi+0A8h] ; push edi ; call sub_D89DE0(ctx, rawEventList)
//             -> walks every 72-byte raw event, buckets it by its +0x28 pad tag (our fix), matches
//                it against every registered binding and emits 64-byte ACTION records per pad.
//   b6de83  mov ecx,[esi+0A8h] ; push eax(padIdx) ; push ebp(out) ; call sub_D89480(ctx,pad,out)
//             -> copies that pad's surviving actions into a 40-byte list, which sub_B6CF30 then
//                hands to the listener.  padIdx comes from the listener's entity (+0x34), so it is
//                0 for hero 1, 1 for hero 2, and -1 for listeners with no entity.
//
// So we census both ends, per pad:
//   [raw] which (pad,type) events actually reach the matcher   -- 11/12 = sticks, 13 = press,
//         15 = release, 16 = trigger.  Answers "do pad-1 button events even exist?"
//   [act] how many action records each pad's listeners receive, plus the action-id histogram
//         (record[0] is the binding's id).  Answers "does the matcher emit them for pad 1?"
//
// Verdict table:
//   raw pad1 t13 > 0, act pad1 acts == 0            -> matcher/binding layer drops pad-1 buttons
//   raw pad1 t13 > 0, act pad1 ids == act pad0 ids  -> the action reaches hero 2; the gate is in
//                                                      the listener/gameplay handler
//   raw pad1 t13 == 0                               -> the button events never leave the device
// Both hooks are pure reads (no vtable calls, no entity walks) -- unlike the sub_B6CF30 hook that
// crashed at the pause menu, which dereferenced listener-owned pointers.
static const uintptr_t RVA_sub_D89DE0  = 0x00989DE0;
static const uintptr_t RVA_CALL_D89DE0 = 0x0076DDA0; // call sub_D89DE0 in sub_B6DD20
static const uintptr_t RVA_sub_D89480  = 0x00989480;
static const uintptr_t RVA_CALL_D89480 = 0x0076DE83; // call sub_D89480 in sub_B6DD20

static const unsigned ACT_SIZE = 40;   // delivered action record (sub_B6CF30 strides by 40)
static const int  CEN_SLOTS = 6;       // 0..3 = pad, 4 = -1 (listener has no entity), 5 = other
static const int  CEN_TYPES = 32;
static const int  CEN_IDS   = 16;      // distinct action ids tracked per slot

static bool g_censusOn = false;
static bool g_gateTrace = false;   // opt-in: "gatetrace.txt" (dead end + crash history, see HANDOFF §11)
static LONG g_cenRaw[CEN_SLOTS][CEN_TYPES];
static LONG g_cenCalls[CEN_SLOTS];
static LONG g_cenActs[CEN_SLOTS];
static DWORD g_cenId[CEN_SLOTS][CEN_IDS];
static LONG  g_cenIdN[CEN_SLOTS][CEN_IDS];
static int   g_cenIdUsed[CEN_SLOTS];
// ---- listener-class census ----
// The action records DO reach hero 2 (proved by the [act] pad1 rows), so the loss is in whichever
// listener object is supposed to act on them. sub_B6DD20 calls sub_D89480(ctx,padIdx,out) and then
// immediately sub_B6CF30(listener,out) for the SAME listener, on the same thread, so the padIdx we
// just saw identifies the listener that is about to be fed. Record each listener's vtable pointer
// (one dword read -- the object is live, sub_B6CF30 itself reads it) and bucket by pad. Mapping the
// vtable back to its RTTI name tells us exactly which listener class hero 1 has and hero 2 lacks.
static const uintptr_t RVA_sub_B6CF30  = 0x0076CF30;
static const uintptr_t RVA_CALL_B6CF30 = 0x0076DE8C; // call sub_B6CF30 in sub_B6DD20
static const int CEN_VTS = 12;
static DWORD g_cenVt[CEN_SLOTS][CEN_VTS];
static LONG  g_cenVtCalls[CEN_SLOTS][CEN_VTS];
static LONG  g_cenVtActs[CEN_SLOTS][CEN_VTS];
static int   g_cenVtUsed[CEN_SLOTS];
static const DWORD ID_INTERACT = 13;          // action id CInputProcessInteract fires on
static LONG  g_cenVtId13[CEN_SLOTS][CEN_VTS]; // per-listener count of id-13 records seen
static int   g_cenLastSlot = 5;      // slot from the most recent sub_D89480 call
static int   g_lastPadIdx  = -1;     // raw controller index from that same call (raw-event fix)
static int   g_cenListenerDumps = 0; // stop repeating the (stable) listener roster after a few
static DWORD g_cenLast = 0;
static LONG  g_cenFlushes = 0;
static const LONG CEN_MAX_FLUSHES = 150;   // ~5 minutes at 2s, then go quiet on its own

static int CenSlot(int ctrl) {
    if (ctrl >= 0 && ctrl <= 3) return ctrl;
    if (ctrl == -1) return 4;
    return 5;
}
static void CenNoteId(int slot, DWORD id) {
    for (int i = 0; i < g_cenIdUsed[slot]; i++)
        if (g_cenId[slot][i] == id) { g_cenIdN[slot][i]++; return; }
    if (g_cenIdUsed[slot] < CEN_IDS) {
        int i = g_cenIdUsed[slot]++;
        g_cenId[slot][i] = id; g_cenIdN[slot][i] = 1;
    }
}
static const char* CenName(int slot) {
    static const char* n[CEN_SLOTS] = {"pad0", "pad1", "pad2", "pad3", "pad-1", "pad?"};
    return n[slot];
}
static void GateFlush();   // handler gate trace, defined below; flushed on the same 2s tick

// Emit one compact snapshot and reset. Called at a frame boundary, at most every 2s.
static void CenFlush() {
    char line[512];
    for (int s = 0; s < CEN_SLOTS; s++) {
        bool anyRaw = false;
        for (int t = 0; t < CEN_TYPES; t++) if (g_cenRaw[s][t]) { anyRaw = true; break; }
        if (anyRaw) {
            int o = wsprintfA(line, "[raw] %s:", CenName(s));
            for (int t = 0; t < CEN_TYPES; t++)
                if (g_cenRaw[s][t] && o < 460)
                    o += wsprintfA(line + o, " t%d=%d", t, (int)g_cenRaw[s][t]);
            Log("%s", line);
        }
        if (g_cenActs[s]) {   // idle windows (calls>0, acts==0) are pure noise now
            int o = wsprintfA(line, "[act] %s: calls=%d acts=%d ids:",
                              CenName(s), (int)g_cenCalls[s], (int)g_cenActs[s]);
            for (int i = 0; i < g_cenIdUsed[s] && o < 440; i++)
                o += wsprintfA(line + o, " %08x(%d)", g_cenId[s][i], (int)g_cenIdN[s][i]);
            if (!g_cenIdUsed[s]) wsprintfA(line + o, " none");
            Log("%s", line);
        }
        // Listener classes on this pad, reported as IDA VAs (imagebase 0x400000) so they can be
        // looked up directly. acts = action records that class was handed this window. The set is
        // stable once both heroes exist, so only report it while it is still settling.
        // Only report a pad's roster in windows where the interact action actually flowed --
        // that is when the drop-off is visible, and it keeps quiet windows out of the log.
        LONG anyId13 = 0;
        for (int i = 0; i < g_cenVtUsed[s]; i++) anyId13 += g_cenVtId13[s][i];
        for (int i = 0; i < g_cenVtUsed[s] && anyId13; i++) {
            DWORD va = 0x400000 + (g_cenVt[s][i] - (DWORD)(uintptr_t)g_base);
            Log("[lsnr] %s #%d: vt=%08x calls=%d acts=%d id13=%d", CenName(s), i, va,
                (int)g_cenVtCalls[s][i], (int)g_cenVtActs[s][i], (int)g_cenVtId13[s][i]);
        }
    }
    GateFlush();
    g_cenListenerDumps++;
    memset(g_cenVtId13, 0, sizeof(g_cenVtId13));
    memset(g_cenVtCalls, 0, sizeof(g_cenVtCalls));
    memset(g_cenVtActs, 0, sizeof(g_cenVtActs));
    memset(g_cenVtUsed, 0, sizeof(g_cenVtUsed));
    memset((void*)g_cenRaw, 0, sizeof(g_cenRaw));
    memset((void*)g_cenCalls, 0, sizeof(g_cenCalls));
    memset((void*)g_cenActs, 0, sizeof(g_cenActs));
    memset(g_cenIdN, 0, sizeof(g_cenIdN));
    memset(g_cenIdUsed, 0, sizeof(g_cenIdUsed));
}

// Also track how many times each listener saw the INTERACT action (id 13). sub_D89480 refuses to
// emit an action whose source event is already consumed (`event+0x38`, checked at d89522), so the
// listener at which id 13 stops appearing is the one that consumed it -- i.e. hero 1's real
// "Press A" handler. Listeners are visited in a fixed order, so the drop-off names it directly.
static void CenNoteVt(int slot, DWORD vt, LONG acts, LONG id13) {
    for (int i = 0; i < g_cenVtUsed[slot]; i++)
        if (g_cenVt[slot][i] == vt) {
            g_cenVtCalls[slot][i]++; g_cenVtActs[slot][i] += acts; g_cenVtId13[slot][i] += id13;
            return;
        }
    if (g_cenVtUsed[slot] < CEN_VTS) {
        int i = g_cenVtUsed[slot]++;
        g_cenVt[slot][i] = vt; g_cenVtCalls[slot][i] = 1;
        g_cenVtActs[slot][i] = acts; g_cenVtId13[slot][i] = id13;
    }
}

typedef void (__fastcall *fnBuildActions)(void* ctx, void* edx, int rawList);
typedef void (__fastcall *fnFetchActions)(void* ctx, void* edx, int padIdx, int outList);
static fnBuildActions g_origD89DE0 = nullptr;
static fnFetchActions g_origD89480 = nullptr;

// sub_D89DE0(ctx, rawEventList) -- once per frame. Census the raw queue, then run the real thing.
static void __fastcall Hook_D89DE0(void* ctx, void* edx, int rawList) {
    if (g_censusOn && rawList > 0x10000) {
        DWORD b = *(DWORD*)(rawList + 8), e = *(DWORD*)(rawList + 12);
        if (b > 0x10000 && e >= b) {
            DWORD bytes = e - b;
            if (bytes % EVT_SIZE == 0 && bytes <= EVT_SIZE * 4096) {
                for (DWORD k = 0; k < bytes / EVT_SIZE; k++) {
                    BYTE* ev = (BYTE*)(b + k * EVT_SIZE);
                    int t = *(int*)(ev + EVT_TYPE);
                    if (t < 0 || t >= CEN_TYPES) t = CEN_TYPES - 1;
                    g_cenRaw[CenSlot(*(int*)(ev + EVT_CTRL))][t]++;
                }
            }
        }
    }
    g_origD89DE0(ctx, edx, rawList);
    if (g_censusOn && InterlockedCompareExchange(&g_cenFlushes, 0, 0) < CEN_MAX_FLUSHES) {
        DWORD now = GetTickCount();
        if (now - g_cenLast >= 2000) {
            g_cenLast = now;
            InterlockedIncrement(&g_cenFlushes);
            CenFlush();
        }
    }
}

// sub_D89480(ctx, padIdx, outList) -- once per listener per frame. Census what each pad's
// listeners are actually handed. Runs AFTER the original so outList is populated.
static void __fastcall Hook_D89480(void* ctx, void* edx, int padIdx, int outList) {
    g_origD89480(ctx, edx, padIdx, outList);
    int slot = CenSlot(padIdx);
    g_cenLastSlot = slot;            // consumed by Hook_B6CF30, which runs next for this listener
    g_lastPadIdx  = padIdx;          // paired with the listener there, to build the LpFind cache
    if (!g_censusOn) return;         // the two lines above must run even with logging off
    g_cenCalls[slot]++;
    if (outList <= 0x10000) return;
    DWORD b = *(DWORD*)(outList + 8), e = *(DWORD*)(outList + 12);
    if (b <= 0x10000 || e < b) return;
    DWORD bytes = e - b;
    if (bytes % ACT_SIZE || bytes > ACT_SIZE * 2048) return;
    DWORD n = bytes / ACT_SIZE;
    g_cenActs[slot] += (LONG)n;
    for (DWORD k = 0; k < n; k++)
        CenNoteId(slot, *(DWORD*)(b + k * ACT_SIZE));
}

// ================= THE UI MERGE (player 2's buttons) =================
// Measured: pad 1's button events reach hero 2 correctly and produce action records identical to
// hero 1's. What hero 2 lacks is a listener that consumes them for UI/"Press A" -- NUI::CGUIInput,
// which sub_B9C760 builds exactly ONE of, bound to controller 0.
//
// We do NOT try to construct a second CGUIInput (it owns focus/selection state for the whole UI
// subsystem). Two facts make that unnecessary:
//   * the prompt/focus side already works per-hero -- player 2 sees button prompts near NPCs;
//   * Fable III's co-op menus are full-screen and SHARED (one player acts, the other watches), so
//     player 2 does not need independent selection state.
// So: when the singleton CGUIInput is about to be fed, append pad 1's actions to its list.
// sub_D89480 pushes onto the vector (sub_D8F640 -> *(out+3) += 40) and never clears -- the clear is
// done by sub_B6DD20 before each fetch -- so calling it a second time with pad 1 merges cleanly.
// ctx is *(mgr+0xA8); mgr is the `this` already in ecx at this call site.
static const uintptr_t RVA_VT_CGUIINPUT = 0x0171A254; // ??_7CGUIInput@NUI@@6B@ (VA 0x1B1A254)
static bool g_uiMerge = false;                         // opt-in via "uimerge.txt" (see InstallQueueHooks)
static volatile LONG g_uiMergeHits = 0;
// Byte range within CGUIInput's action list holding the records the merge appended from pad 1.
// Hook_GuiAction turns this into "the action being handled right now came from player 2", which is
// what lets the interaction fix route the press to hero 2 instead of the primary hero.
static volatile DWORD g_pad1Lo = 0, g_pad1Hi = 0;
static volatile LONG  g_curActionPad = 0;              // 1 while servicing a pad-1 action record
// MEASURED (build 49): the record handed to CGUIInput::HandleAction sits at a FIXED scratch address
// (act=08c9d4c0 every time) nowhere near the merged range [07fc0f20,07fc1150) -- the dispatcher
// copies each record before dispatching it, so provenance by pointer can never work. Provenance by
// ORDER can: the merge appends pad 1's records after pad 0's, and the dispatcher walks the list in
// order, so record index >= the pre-merge count means "this one came from pad 1".
static volatile LONG g_pad1FirstIdx = -1;              // index of the first pad-1 record, -1 = none
static volatile LONG g_actionSeq    = 0;               // records dispatched since the last fetch

// ============ THE RAW-EVENT FIX (player 2's buttons) ============
// Proved by the id-13 census: CInputProcessInteract is not *given* the interact action and refused,
// it is SKIPPED from the action pass entirely (23 calls vs 30) in exactly the frames interact input
// flows. sub_B6DD20 skips a listener from the action pass when it already consumed a RAW event in
// sub_B6D020. So "Press A" is handled on the raw path (listener->vtbl[0x18]), not the action path.
//
// And the raw path is hardcoded to player one:
//     b6d076  call sub_6B7310        ; GetPlayer(1)
//     b6d07f  cmp dword ptr [esi+24h], 1   ; is a pad event
//     b6d085  mov ecx, [esi+28h]     ; the event's source pad
//     b6d088  cmp ecx, [eax+34h]     ; player ONE's controller index
//     b6d08b  jnz -> skip this event
// Every pad-1 raw event is discarded there, for every listener. That is why movement works (action
// path, fixed by the sub_D89DE0 bucket patch) while interaction does not (raw path, player 1 only).
//
// Fix: make the comparison per-listener instead of global. sub_6B7310's result is used for nothing
// but `[eax+34h]`, so we hook its call site *inside sub_B6D020* and hand back a stand-in whose
// +0x34 is the CURRENT listener's controller. Listeners with no entity (pad -1) fall through to the
// real GetPlayer(1), preserving stock behaviour for global listeners.
//
// The listener's controller is not known until sub_D89480 runs later in the same iteration, so we
// learn it from the previous frame: Hook_D89480 records the pad, Hook_B6CF30 pairs it with the
// listener. The roster is stable frame to frame, so by frame 2 every listener is known.
static const uintptr_t RVA_sub_B6D020   = 0x0076D020;
static const uintptr_t RVA_CALL_B6D020  = 0x0076DE25; // call sub_B6D020 in sub_B6DD20
static const uintptr_t RVA_sub_6B7310   = 0x002B7310;
static const uintptr_t RVA_CALL_6B7310  = 0x0076D076; // call sub_6B7310 inside sub_B6D020

static bool g_rawFix = true;              // "norawfix.txt" next to Fable3.exe disables
static int  g_curListenerPad = -1;        // controller of the listener sub_B6D020 is running for
static DWORD g_standInPlayer[20];         // only +0x34 is ever read by the filter
static volatile LONG g_rawFixHits = 0;

static const int LP_MAX = 40;
static DWORD g_lpListener[LP_MAX];
static int   g_lpPad[LP_MAX];
static int   g_lpUsed = 0;
static void LpNote(DWORD listener, int pad) {
    for (int i = 0; i < g_lpUsed; i++)
        if (g_lpListener[i] == listener) { g_lpPad[i] = pad; return; }
    if (g_lpUsed < LP_MAX) { g_lpListener[g_lpUsed] = listener; g_lpPad[g_lpUsed] = pad; g_lpUsed++; }
}
static int LpFind(DWORD listener) {
    for (int i = 0; i < g_lpUsed; i++)
        if (g_lpListener[i] == listener) return g_lpPad[i];
    return -1;
}

typedef int   (__fastcall *fnRawPass)(void* mgr, void* edx, int listener, int rawList, void* flag);
typedef DWORD (__fastcall *fnGetPlayer1)(void* mgr, void* edx);
static fnRawPass    g_origB6D020 = nullptr;
static fnGetPlayer1 g_origGetP1  = nullptr;

// Of every listener in the roster, ONLY CGUIInput has a real raw-event handler -- every
// CInputProcess* class has nullsub_4601 at vtbl[0x18]. So the raw path exists solely to feed the
// UI, and filtering it to player 1's controller is what stops pad 2 from ever driving the UI.
// The filter treats a null GetPlayer(1) as "no filter" (`test eax,eax ; jz loc_B6D08D`), so for
// CGUIInput we return null and let every pad's raw events through. Every other listener keeps the
// per-listener controller, which is stock behaviour for hero 1 and correct for hero 2.
static bool g_curIsGuiInput = false;

// --- A-press consumption census ---
// Raw events carry a consumed flag at +0x38; sub_B6D020 skips already-consumed events and the
// handler that acts on one sets it. Snapshot A-press events (type 13, button 11) around each
// listener's raw pass: whoever flips 0->1 during their pass IS the game's real Press-A consumer.
static const int RAWBTN_A = 11;      // XInput bit 0x1000 -> id 11 (Start=0x10 -> 5, verified)
static int __fastcall Hook_B6D020(void* mgr, void* edx, int listener, int rawList, void* flag) {
    int  savePad = g_curListenerPad;
    bool saveGui = g_curIsGuiInput;
    g_curListenerPad = (listener > 0x10000) ? LpFind((DWORD)listener) : -1;
    g_curIsGuiInput  = (listener > 0x10000 && g_base &&
                        *(DWORD*)listener == (DWORD)(uintptr_t)(g_base + RVA_VT_CGUIINPUT));
    DWORD evA[8]; BYTE pre[8]; int n = 0;
    if (g_censusOn && rawList > 0x10000) {
        DWORD b = *(DWORD*)(rawList + 8), e = *(DWORD*)(rawList + 12);
        if (b > 0x10000 && e >= b && (e - b) % EVT_SIZE == 0 && (e - b) <= EVT_SIZE * 4096) {
            for (DWORD k = b; k < e && n < 8; k += EVT_SIZE) {
                if (*(int*)(k + EVT_TYPE) == 13 && *(BYTE*)(k + 8) == RAWBTN_A) {   // id is a BYTE (sub_188F5A0)
                    evA[n] = k; pre[n] = *(BYTE*)(k + 0x38); n++;
                }
            }
        }
    }
    int r = g_origB6D020(mgr, edx, listener, rawList, flag);
    for (int i = 0; i < n; i++) {
        if (!pre[i] && *(BYTE*)(evA[i] + 0x38) && listener > 0x10000) {
            DWORD va = 0x400000 + (*(DWORD*)listener - (DWORD)(uintptr_t)g_base);
            Log("[consume] A-press pad%d consumed by listener vt=%08x (raw pass)",
                *(int*)(evA[i] + EVT_CTRL), va);
        }
    }
    g_curListenerPad = savePad;
    g_curIsGuiInput  = saveGui;
    return r;
}
static DWORD __fastcall Hook_GetPlayer1(void* mgr, void* edx) {
    if (g_rawFix && g_joinedFlag) {
        if (g_curIsGuiInput) {                 // UI: accept every pad's raw events
            if (InterlockedIncrement(&g_rawFixHits) <= 3)
                Log("[rawfix] CGUIInput raw path unfiltered (all pads)");
            return 0;
        }
        if (g_curListenerPad >= 0) {           // everything else: filter to its OWN controller
            g_standInPlayer[13] = (DWORD)g_curListenerPad;   // offset 0x34
            return (DWORD)(uintptr_t)g_standInPlayer;
        }
    }
    return g_origGetP1(mgr, edx);
}

// sub_B6CF30(mgr /*ecx, unused by the callee*/, listener, outList) -- delivers the fetched actions.
// Reads the listener's vtable pointer, performs the UI merge, then hands off.
typedef unsigned (__fastcall *fnDeliver)(void* mgr, void* edx, int listener, int outList);
static fnDeliver g_origB6CF30 = nullptr;
static unsigned __fastcall Hook_B6CF30(void* mgr, void* edx, int listener, int outList) {
    if (listener > 0x10000) {
        // Pair this listener with the pad sub_D89480 just fetched for it. Feeds LpFind, which the
        // raw-event fix uses on the NEXT frame (the roster is stable, so one frame of lag is fine).
        LpNote((DWORD)listener, g_lastPadIdx);
        // --- the UI merge: give the singleton CGUIInput pad 1's actions too ---
        if (g_uiMerge && g_cenLastSlot == 0 && g_joinedFlag &&
            *(DWORD*)listener == (DWORD)(uintptr_t)(g_base + RVA_VT_CGUIINPUT)) {
            DWORD ctx = *(DWORD*)((DWORD)(uintptr_t)mgr + 0xA8);
            if (ctx > 0x10000 && g_origD89480 && outList > 0x10000) {
                // Remember WHICH records came from pad 1, so the interaction fix downstream can tell
                // whose press it is servicing. Counting records instead of caching the old end
                // pointer keeps this correct even if the vector reallocates during the append.
                DWORD b0 = *(DWORD*)(outList + 8), e0 = *(DWORD*)(outList + 12);
                DWORD nBefore = (e0 >= b0) ? (e0 - b0) / ACT_SIZE : 0;
                g_origD89480((void*)ctx, nullptr, 1, outList);   // append pad 1's actions
                DWORD b1 = *(DWORD*)(outList + 8), e1 = *(DWORD*)(outList + 12);
                g_pad1Lo = b1 + nBefore * ACT_SIZE;
                g_pad1Hi = e1;
                // Order-based provenance (see g_pad1FirstIdx): everything from index nBefore on is
                // pad 1's. Reset the dispatch counter so indices line up with this fetch.
                InterlockedExchange(&g_pad1FirstIdx, (e1 > g_pad1Lo) ? (LONG)nBefore : -1);
                InterlockedExchange(&g_actionSeq, 0);
                if (e1 > g_pad1Lo && InterlockedIncrement(&g_uiMergeHits) <= 5)
                    Log("[uimerge] CGUIInput fed pad-1 actions (+%d records)",
                        (int)((e1 - g_pad1Lo) / ACT_SIZE));
            }
        }
        if (g_censusOn) {
            LONG acts = 0, id13 = 0;
            if (outList > 0x10000) {
                DWORD b = *(DWORD*)(outList + 8), e = *(DWORD*)(outList + 12);
                if (b > 0x10000 && e >= b && (e - b) % ACT_SIZE == 0 && (e - b) <= ACT_SIZE * 2048) {
                    acts = (LONG)((e - b) / ACT_SIZE);
                    for (LONG k = 0; k < acts; k++)
                        if (*(DWORD*)(b + k * ACT_SIZE) == ID_INTERACT) id13++;
                }
            }
            CenNoteVt(g_cenLastSlot, *(DWORD*)listener, acts, id13);
        }
    }
    return g_origB6CF30(mgr, edx, listener, outList);
}

// ================= HANDLER GATE TRACE (READ-ONLY) =================
// [caps] showed hero 1 and hero 2 with byte-identical capability words, so every entity-level gate
// passes for both and the handlers must be running. Trace them from the inside: replace the two
// vtable slots with thunks that walk the SAME decision chain the handler is about to walk, record
// which test fails first, then hand off to the original untouched.
//
//   CInputProcessInteract::Handle = sub_1355000, vtable NUI slot +4 of 0x1B64478, action id 13
//       g0 P=*(this+12)!=0 -> g1 E=*P!=0 -> g2 E+172&2 -> g3 E+44&0x20 -> g4 comp37
//       -> g5 sub_9134F0(comp37,3) -> g6 E+55&1 -> FIRE (queues CAbilityPlayerInteract)
//   CInputProcessCombat::Handle   = sub_1354000, vtable slot +4 of 0x1B64430, action id 74
//       g0 P -> g1 E -> g2 E+172&2 -> g3 E+40&0x10000000 -> g5 sub_8C7C60() -> FIRE
//
// sub_9134F0 and sub_8C7C60 are side-effect-free predicates the game evaluates on these very
// objects every frame (sub_8C7C60 passes a2=0, which is the branch that skips its only writer), so
// calling them here costs nothing and answers the question exactly.
static const uintptr_t RVA_VT_INTERACT = 0x0176447C; // CInputProcessInteract vtable slot +4
static const uintptr_t RVA_VT_COMBAT   = 0x01764434; // CInputProcessCombat   vtable slot +4
static const uintptr_t RVA_sub_1355000 = 0x00F55000; // CInputProcessInteract::Handle
static const uintptr_t RVA_sub_1354000 = 0x00F54000; // CInputProcessCombat::Handle
static const uintptr_t RVA_sub_9134F0  = 0x005134F0;
static const uintptr_t RVA_sub_8C7C60  = 0x004C7C60;

static const int GATE_N   = 8;   // g0..g6 plus FIRE at index 7
static const int IP_MAX   = 8;   // distinct input-process instances we track
static DWORD g_ipThis[IP_MAX];
static DWORD g_ipEnt [IP_MAX];
static int   g_ipKind[IP_MAX];   // 0 = interact, 1 = combat
static LONG  g_ipGate[IP_MAX][GATE_N];
static int   g_ipUsed = 0;

typedef char (__fastcall *fnPred1)(void* thisp, void* edx, int a2); // sub_9134F0(comp, id)
typedef int  (__cdecl   *fnPred0)(void);                            // sub_8C7C60()
static fnPred1 g_sub9134F0 = nullptr;
static fnPred0 g_sub8C7C60 = nullptr;

static int  g_ipProbe [IP_MAX];   // gate result from the per-window positive-control probe
static BYTE g_ipProbed[IP_MAX];
static DWORD g_ipDevice[IP_MAX];  // control scheme's device, for the g5 registry key
static LONG  g_ipProbes[IP_MAX];  // probes done, capped -- we only need a handful

static const LONG PROBES_PER_INSTANCE = 24;

// True only if [p, p+n) is committed, readable, and not a guard page.
static bool PtrOk(DWORD p, SIZE_T n) {
    if (p <= 0x10000 || (p & 3)) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    DWORD end = (DWORD)(uintptr_t)mbi.BaseAddress + (DWORD)mbi.RegionSize;
    return p + n <= end;
}

// Find-or-create the slot for this input-process instance. Returns -1 if the table is full.
static int GateSlot(DWORD thisp, int kind) {
    for (int i = 0; i < g_ipUsed; i++)
        if (g_ipThis[i] == thisp) return i;
    if (g_ipUsed >= IP_MAX) return -1;
    int i = g_ipUsed++;
    g_ipThis[i] = thisp; g_ipEnt[i] = 0; g_ipKind[i] = kind;
    g_ipProbe[i] = -1; g_ipProbed[i] = 0;
    for (int g = 0; g < GATE_N; g++) g_ipGate[i][g] = 0;
    return i;
}

// Walk the handler's own gate chain and return the index of the first failure (7 = all passed).
// Every dereference is PtrOk-validated; -3 means "could not evaluate safely", never a crash.
static int InteractGate(DWORD thisp, DWORD* pEnt, DWORD* pDev) {
    *pEnt = 0; if (pDev) *pDev = 0;
    if (!PtrOk(thisp + 12, 4)) return -3;
    DWORD P = *(DWORD*)(thisp + 12);
    if (!PtrOk(P, 4)) return 0;
    DWORD E = *(DWORD*)P;                       *pEnt = E;
    if (!PtrOk(E + 40, 140)) return 1;           // covers +40..+172
    if (!(*(BYTE*)(E + 172) & 2)) return 2;
    if (!(*(DWORD*)(E + 44) & 0x20)) return 3;
    if (!PtrOk(E + 0xA8, 4) || !PtrOk(E + 0x58, 4)) return 4;
    DWORD c37 = GetComponentOf(E, TYPE_CONTROLSCHEME);
    if (!PtrOk(c37 + 0x14, 4)) return 4;
    DWORD dev = *(DWORD*)(c37 + 0x14);
    if (pDev) *pDev = dev;
    if (!PtrOk(dev + 0x30, 8)) return 5;         // sub_9134F0's own null-device check
    if (!g_sub9134F0 || !g_sub9134F0((void*)c37, nullptr, 3)) return 5;
    if (!(*(BYTE*)(E + 55) & 1)) return 6;
    return 7;                                    // would queue CAbilityPlayerInteract
}
static int CombatGate(DWORD thisp, DWORD* pEnt, DWORD* pDev) {
    *pEnt = 0; if (pDev) *pDev = 0;
    if (!PtrOk(thisp + 12, 4)) return -3;
    DWORD P = *(DWORD*)(thisp + 12);
    if (!PtrOk(P, 4)) return 0;
    DWORD E = *(DWORD*)P;                       *pEnt = E;
    if (!PtrOk(E + 40, 140)) return 1;
    if (!(*(BYTE*)(E + 172) & 2)) return 2;
    if (!(*(DWORD*)(E + 40) & 0x10000000)) return 3;
    if (!g_sub8C7C60 || !g_sub8C7C60()) return 5;
    return 7;                                    // would run sub_1353670 (the attack)
}

// SEH shells: a fault anywhere above (or inside the game predicate) becomes -2, not a crash.
static int SafeGate(int kind, DWORD thisp, DWORD* pEnt, DWORD* pDev) {
    int g;
    __try {
        g = kind ? CombatGate(thisp, pEnt, pDev) : InteractGate(thisp, pEnt, pDev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g = -2;
    }
    return g;
}
static bool ProbeAllowed() {
    return (LONG)GetTickCount() - InterlockedCompareExchange(&g_probeQuietUntil, 0, 0) > 0;
}

typedef int (__fastcall *fnHandle)(void* thisp, void* edx, DWORD* action);
static fnHandle g_origInteract = nullptr;
static fnHandle g_origCombat   = nullptr;

// The handler is invoked for EVERY action record and bails immediately unless the id matches, so we
// see hero 1's instance here even when its action 13 gets consumed upstream. That gives the positive
// control the last run lacked: probe the gate chain once per window per instance, regardless of id.
// Shared body: note the instance, probe it at most once per window (and only outside the spawn
// quiet period), and count the gate outcome when the matching action id actually arrives.
static void GateObserve(int kind, void* thisp, DWORD* action, DWORD wantId) {
    if (!g_censusOn || !ProbeAllowed()) return;
    DWORD t = (DWORD)(uintptr_t)thisp;
    int i = GateSlot(t, kind);
    if (i < 0) return;
    bool hit = (action && *action == wantId);
    if (g_ipProbed[i] && !hit) return;
    if (g_ipProbes[i] >= PROBES_PER_INSTANCE && !hit) return;
    DWORD e = 0, dev = 0;
    int g = SafeGate(kind, t, &e, &dev);
    if (e) g_ipEnt[i] = e;
    if (dev) g_ipDevice[i] = dev;
    if (!g_ipProbed[i]) { g_ipProbe[i] = g; g_ipProbed[i] = 1; g_ipProbes[i]++; }
    if (hit && g >= 0 && g < GATE_N) g_ipGate[i][g]++;
}
static int __fastcall Hook_Interact(void* thisp, void* edx, DWORD* action) {
    GateObserve(0, thisp, action, 13);
    return g_origInteract(thisp, edx, action);
}
static int __fastcall Hook_Combat(void* thisp, void* edx, DWORD* action) {
    GateObserve(1, thisp, action, 74);
    return g_origCombat(thisp, edx, action);
}

static void GateFlush() {
    for (int i = 0; i < g_ipUsed; i++) {
        char line[400];
        // probe = where the chain stands right now for this instance, independent of whether the
        // matching action id actually arrived. 7 means it would fire.
        int o = wsprintfA(line, "[gate] %s this=%08x E=%08x dev=%08x probe=%s%d",
                          g_ipKind[i] ? "combat  " : "interact", g_ipThis[i], g_ipEnt[i],
                          g_ipDevice[i],
                          g_ipProbe[i] == 7 ? "PASS-g" : (g_ipProbe[i] < 0 ? "skip" : "fail@g"),
                          g_ipProbe[i]);
        for (int g = 0; g < GATE_N; g++) {
            if (!g_ipGate[i][g]) continue;
            if (g == 7) o += wsprintfA(line + o, " FIRED=%d", (int)g_ipGate[i][g]);
            else        o += wsprintfA(line + o, " fail@g%d=%d", g, (int)g_ipGate[i][g]);
            g_ipGate[i][g] = 0;
        }
        g_ipProbed[i] = 0;
        Log("%s", line);
    }
}

// Swap one vtable entry. Guarded like PatchRel32: refuses unless the slot still holds the expected
// original, so it cannot corrupt a different build (and is idempotent if we somehow run twice).
static bool PatchVtableSlot(BYTE* base, uintptr_t rvaSlot, void* expectOrig,
                            void* hook, const char* name) {
    void** slot = (void**)(base + rvaSlot);
    if (*slot != expectOrig) {
        if (*slot == hook) { Log("[vt] %s: already patched", name); return true; }
        Log("[vt] %s: REFUSED (slot=%08x, expected %08x)", name,
            (unsigned)(uintptr_t)*slot, (unsigned)(uintptr_t)expectOrig);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Log("[vt] %s: VirtualProtect failed", name); return false;
    }
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("[vt] %s: patched -> %08x", name, (unsigned)(uintptr_t)hook);
    return true;
}

// ============ THE ABILITY-GATE FIX (player 2's buttons — the real path) ============
// Found by walking the actual perform-interact chain backwards in IDA (no launches spent):
//
//   The unidentified 12th listener (vt 0x1b10604) is CPlayerModeNoMove. Its action handler
//   sub_1121F70 is CPlayerModeBase::HandleAction, SHARED by ~30 CPlayerMode* vtables --
//   including CPlayerModeControlEntitySimple, which hero 2 HAS and which the census proved
//   receives the full id-13 action stream. Its switch maps EVERY gameplay button to an ability:
//       13 -> CAbilityPlayerInteract   0x14 -> CAbilityMeleeAttack   0x48 -> RangedAimSet ...
//   So the CInputProcess* classes were never the gameplay path at all; player modes are.
//   (The 23-vs-30 "skip" that pointed at CInputProcessInteract was an iterator-invalidation
//   abort: sub_B6CE70 returns "listener set changed mid-dispatch" -- player 1's interact pushes
//   CPlayerModeNoMove as a NEW listener mid-frame, truncating the rest of that iteration.)
//
//   The chain: HandleAction case13 -> sub_1121E10 (queues on the MODE'S OWN entity; CanStart of
//   CAbilityPlayerInteract is literally `return 1`) -> sub_7F3A50(entity, ability) -> and the
//   FIRST thing sub_7F3A50 does is gate on sub_7F3210(entity):
//       if (!(entity+44 & 0x20)) return 1;
//       comp37 = component(entity, 37);
//       return !sub_9134F0(comp37, 2) && !((entity+52 >> 28) & 1);
//   sub_9134F0(comp37, KEY) is the per-DEVICE registry lookup we met as "g5" (key 3 there).
//   Here the sense is INVERTED: an entry of type 2 for your device means your abilities are
//   SUPPRESSED (the cutscene/handover input lock). Movement is not an ability, which is exactly
//   why hero 2 can walk but cannot press any button. Hero 1's device gets its unlock when the
//   intro hands over control; hero 2 joins later and never does.
//
// So: hook the `call sub_7F3210` site inside sub_7F3A50. Log the verdict per hero, and when
// HERO 2 is refused precisely because of the type-2 device entry (not the entity bit-28 flag),
// return 1 instead. Also hook the mode-handler's `call sub_7F3A50` site to see every queue
// attempt (entity, ability class, final result) -- if the gate passes and it still does nothing,
// that log localises the next stage. Both are E8 rel32 call-site patches, conventions verified
// against the disassembly (cdecl, caller-cleaned: add esp,8 / add esp,0Ch).
static const uintptr_t RVA_sub_7F3210       = 0x003F3210; // ability-dispatch entry gate
static const uintptr_t RVA_CALL_7F3210      = 0x003F3A6F; // its call site inside sub_7F3A50
static const uintptr_t RVA_sub_7F3A50_q     = 0x003F3A50; // ability dispatch (entity, ability)
static const uintptr_t RVA_CALL_7F3A50_MODE = 0x00D21F54; // its call site in sub_1121E10 (mode handlers)

static bool g_abilityFix = true;            // "noabilityfix.txt" next to Fable3.exe disables the
                                            // override; the logging stays either way
typedef char (__fastcall *fnLockCheck)(void* comp37, void* edx, int key); // game sub_9134F0
typedef int  (__cdecl *fnAbilityGate)(int entity, int ability);
typedef char (__cdecl *fnAbilityQueue)(int entity, int ability, int a3);
static fnLockCheck    g_fnLockCheck     = nullptr;
static fnAbilityGate  g_origAbilityGate = nullptr;
static fnAbilityQueue g_origModeQueue   = nullptr;

// Small shared log budget so a held button cannot flood the file.
static volatile LONG g_agBudget = 0;
static DWORD g_agTick = 0;
static bool AgLogOk() {
    DWORD now = GetTickCount();
    if (now - g_agTick > 1000) { g_agTick = now; InterlockedExchange(&g_agBudget, 0); }
    return InterlockedIncrement(&g_agBudget) <= 8;
}

static int __cdecl Hook_AbilityGate(int entity, int ability) {
    int r = g_origAbilityGate(entity, ability);
    if (r || !g_joinedFlag) return r;                    // allowed, or pre-join: nothing to do
    DWORD h2 = GetHeroEntity(0xF4);
    if (!h2 || (DWORD)entity != h2) return r;            // only ever touch hero 2's refusals
    int lock2 = -1, bit28 = -1;
    __try {
        DWORD c37 = GetComponentOf((DWORD)entity, TYPE_CONTROLSCHEME);
        lock2 = (c37 && g_fnLockCheck) ? (g_fnLockCheck((void*)c37, nullptr, 2) & 1) : -2;
        bit28 = (int)((*(DWORD*)(entity + 52) >> 28) & 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return r;                                        // can't evaluate safely: leave stock
    }
    bool fix = g_abilityFix && lock2 == 1 && bit28 == 0;
    if (AgLogOk())
        Log("[abilitygate] hero2 REFUSED  devlock2=%d bit28=%d -> %s",
            lock2, bit28, fix ? "OVERRIDE: pass" : "left refused");
    return fix ? 1 : r;
}

// ============ CanPerform TRACE: CAbilityActionUnsheatheMeleeWeapon ============
// MEASURED (build 39): weapon-draw is the clean divergence.
//     hero1  CAbilityUnsheatheMeleeWeapon -> handler=CAbilityActionUnsheatheMeleeWeapon
//     hero2  CAbilityUnsheatheMeleeWeapon -> handler=NULL
// while CAbilitySprintSet resolved to the SAME handler for both. So sub_7F3660's rule matcher finds
// no candidate for hero 2, and the candidate's own condition is vtable slot 2 = sub_13CC4B0.
//
// sub_13CC4B0 (CanPerform) has many exits. Most read fields that the [caps] dump already shows to be
// IDENTICAL across the two heroes (+60&0x400, +52>>28&1, +40&0x10000000, +44 bits). The one gate
// whose inputs have never been read is:
//       sub_9141E0(comp37) == comp37[27] && comp37[49]        -- !sub_9141E0 -> return 0
// comp37 is the per-hero control-scheme component, and [caps] shows comp37+48 = 00010101, i.e.
// byte[49] = 1 for BOTH heroes. That leaves comp37[27] as the prime suspect: a control-scheme flag
// that is set when a player is fully wired up and that hero 2 may never have received.
//
// This hooks the condition itself (vtable slot 2) and logs its verdict per hero together with the
// per-hero inputs, so one press of X by each player identifies the exact failing clause.
// CORRECTION (build 40): comp37[27] was NOT it -- [27]=1 and [49]=1 on BOTH heroes. And the
// "clean divergence" from build 39 was a sampling artifact: CanPerform returns 0 for hero 1 too,
// on almost every call. Hero 1 got exactly ONE CanPerform=1 (the frame the weapon actually drew);
// hero 2 got none in 6 samples. So handler=NULL is the NORMAL per-frame state for both heroes and
// only the successful frame matters. Two fixes here:
//   * a dedicated, generous log budget so hero 2's samples are not starved by the shared 8/sec one;
//   * a per-clause breakdown, so the failing clause is named instead of guessed.
// Conventions taken from the disassembly of sub_13CC4B0, not from Hex-Rays' prototypes:
//   13cc650  mov ecx, edi / call sub_8C7C40   -> __fastcall(comp28), no stack args
//   13cc65f  mov ecx, edi / call sub_8C7C90   -> __fastcall(comp28), no stack args
//   sub_BFB8F0 / sub_BFB9D0                   -> __cdecl(entity), caller-cleaned
//   sub_9141E0 / sub_914580                   -> __thiscall(comp37), no stack args
// sub_AA1510 needs the world object in ecx and is left out; if every clause below matches, the
// answer is in the `+44 & 0x2000000` block or sub_AA1510 and those get instrumented next.
static const uintptr_t RVA_VT_UNSHEATHE_CAN = 0x01730C40; // vtbl 0x1B30C38 slot 2 (VA 0x1B30C40)
static const uintptr_t RVA_sub_13CC4B0      = 0x00FCC4B0;
static const uintptr_t RVA_sub_BFB8F0       = 0x007FB8F0;
static const uintptr_t RVA_sub_BFB9D0       = 0x007FB9D0;
static const uintptr_t RVA_sub_8C7C40       = 0x004C7C40;
static const uintptr_t RVA_sub_8C7C90       = 0x004C7C90;
static const uintptr_t RVA_sub_9141E0       = 0x005141E0;
static const uintptr_t RVA_sub_914580       = 0x00514580;
typedef char (__stdcall  *fnCanPerform)(int entity, int ability, int* out);
typedef char (__cdecl    *fnPredCdecl)(int entity);
typedef char (__fastcall *fnPredEcx)(void* ecx, void* edx);
static fnCanPerform g_origCanUnsheathe = nullptr;

static volatile LONG g_cpBudget = 0;
static DWORD g_cpTick = 0;
static bool CpLogOk() {                       // own budget: 40/sec, independent of AgLogOk
    DWORD now = GetTickCount();
    if (now - g_cpTick > 1000) { g_cpTick = now; InterlockedExchange(&g_cpBudget, 0); }
    return InterlockedIncrement(&g_cpBudget) <= 40;
}

static char __fastcall Hook_CanUnsheathe(void* self, void* edx, int entity, int ability, int* out) {
    char r = g_origCanUnsheathe ? g_origCanUnsheathe(entity, ability, out) : 0;
    if (CpLogOk()) {
        int c1 = -1, c2 = -1, c3 = -1, c4 = -1, c5 = -1, c6 = -1, c7 = -1, c8 = -1, c9 = -1, c10 = -1;
        DWORD c37 = 0, c28 = 0;
        __try {
            c37 = GetComponentOf((DWORD)entity, TYPE_CONTROLSCHEME);
            c28 = GetComponentOf((DWORD)entity, 28);
            c1 = (*(DWORD*)(entity + 60) & 0x400) ? 1 : 0;            // -> bail if 1
            c2 = ((fnPredCdecl)(g_base + RVA_sub_BFB8F0))(entity) & 1;     // -> bail if 1
            c3 = ((fnPredCdecl)(g_base + RVA_sub_BFB9D0))(entity) & 1;     // -> bail if 1
            c4 = (int)((*(DWORD*)(entity + 52) >> 28) & 1);            // -> bail if 1
            c5 = (*(DWORD*)(entity + 40) & 0x10000000) ? 1 : 0;        // -> bail if 0
            if (c28) {
                c6 = ((fnPredEcx)(g_base + RVA_sub_8C7C40))((void*)c28, nullptr) & 1; // bail if 1
                c7 = ((fnPredEcx)(g_base + RVA_sub_8C7C90))((void*)c28, nullptr) & 1; // bail if 0
            }
            if (c37) {
                c8  = ((fnPredEcx)(g_base + RVA_sub_9141E0))((void*)c37, nullptr) & 1; // bail if 0
                c9  = ((fnPredEcx)(g_base + RVA_sub_914580))((void*)c37, nullptr) & 1; // bail if 1
                c10 = (*(DWORD*)(entity + 44) & 0x2000000) ? 1 : 0;    // enters the comp-57 block
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        // (see Hook_AA1510 below -- computing the manager by hand faulted, so it is captured at the
        //  real call site where ecx already holds it)
        // ---- the clause the previous build could not reach: sub_AA1510(mgr, entity, 0x40400) ----
        // sub_AA1510(a1,a2) = (*(DWORD*)sub_AA1140(mgr, a1) & a2) != 0, and CanPerform bails when
        // it is TRUE. sub_AA1140 looks the entity up in the manager's map at mgr+164 (keyed on
        // entity+24) and -- critically -- RETURNS `mgr` ITSELF when the entity is not found. So for
        // an unregistered entity sub_AA1510 masks the MANAGER'S VTABLE POINTER instead of that
        // entity's flag word. 0x40400 is bits 10 and 18; a vtable address hits one of those most of
        // the time, i.e. an unregistered hero reads as "blocked" almost always.
        // The manager is sub_658E80(world) = *(*(world+4)+12), and sub_AA6360 (co-op teardown)
        // clears "RemoteHeroJoining"/"RemoteHeroDogJoining" on it -- so it is hero-join state.
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        const char* who = ((DWORD)entity == h1) ? "hero1"
                        : ((DWORD)entity == h2) ? "hero2" : "other";
        Log("[canperf] %s CanPerform=%d | 60&400=%d BFB8F0=%d BFB9D0=%d 52>>28=%d 40&1000_0000=%d "
            "| 8C7C40=%d 8C7C90=%d | 9141E0=%d 914580=%d 44&200_0000=%d | c37=%08x c28=%08x",
            who, (int)r, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c37, c28);
    }
    return r;
}

// ---- the last unmeasured clause, captured at its own call site ----
//   13cc687   mov ecx, eax        ; ecx = the manager (sub_658E80(world))
//             call sub_AA1510     ; __stdcall(entity, 0x40400), ecx passed through to sub_AA1140
// CanPerform bails when this returns TRUE. Hooking here avoids re-deriving the manager pointer --
// the previous build computed it by hand as *(*(world+4)+12), which faulted.
//
// sub_AA1140(mgr, entity) looks the entity up in mgr+164 keyed on entity+24 and RETURNS `mgr`
// ITSELF on a miss, so an unregistered entity ends up masking the manager's vtable pointer. With
// the real mgr in hand we can both log sub_AA1510's verdict and check for that miss directly.
static const uintptr_t RVA_sub_AA1510  = 0x006A1510;
static const uintptr_t RVA_sub_AA1140  = 0x006A1140;

// Repoint EVERY rel32 call/jmp in the game's executable sections that targets `targetRva`.
// sub_AA1510 has 60+ call sites, so a hand-maintained address list is impractical and a prologue
// trampoline is the thing this project has been burned by. This is safe because it only rewrites an
// operand whose existing target already resolves EXACTLY to the function we mean to hook -- a
// mid-instruction false positive would have to match a specific 32-bit displacement by chance.
// The count is logged so it can be sanity-checked against IDA's xref count.
static int RepointAllCallsTo(BYTE* base, uintptr_t targetRva, void* hook, const char* name) {
    IMAGE_DOS_HEADER*   dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt  = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) {
        Log("[patch] %s: bad PE headers, skipped", name);
        return 0;
    }
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* target = base + targetRva;
    int n = 0, failed = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        BYTE* b = base + sec[s].VirtualAddress;
        DWORD sz = sec[s].Misc.VirtualSize;
        if (sz < 5) continue;
        for (DWORD i = 0; i + 5 <= sz; i++) {
            if (b[i] != 0xE8 && b[i] != 0xE9) continue;      // call rel32 / jmp rel32
            BYTE* site = b + i;
            INT32 rel  = *(INT32*)(site + 1);
            if (site + 5 + rel != target) continue;
            INT32 newRel = (INT32)((BYTE*)hook - (site + 5));
            DWORD old;
            if (VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
                *(INT32*)(site + 1) = newRel;
                VirtualProtect(site + 1, 4, old, &old);
                n++;
            } else {
                failed++;
            }
        }
    }
    Log("[patch] %s: repointed %d call sites (%d failed)", name, n, failed);
    return n;
}
// ==================== sub_825100 -- THE LOCAL-vs-REMOTE CHOKEPOINT ====================
// Found while chasing the missing interaction prompt, and it may be the master switch for the whole
// remainder rather than one more symptom.
//
// AddDesiredEmotionIcon -- the floating "press A" prompt -- is sub_919E00, registered on the script
// class "Player" (table sub_924780). Its top-level shape is:
//
//     if ( sub_825100(hero) )  { ...build and show the icon locally...          }
//     else                     { ...sub_6A8640(): SEND CNetDesiredEmotionIconPacket... }
//
// So if sub_825100 says "not local" for hero 2, hero 2's prompt is not suppressed -- it is
// TRANSMITTED, to a peer that does not exist. The prompt is being posted to the void.
//
// sub_825100 is __thiscall(heroEntity) (ecx = entity; verified in disasm; +172/+173 field reads in
// sub_825080 are this binary's entity-flags idiom) and it has 40+ call sites across presentation,
// abilities and inventory. That is why this is worth more than a prompt fix: the same predicate
// decides local-vs-network for a great deal of what player 2 does, which would also explain the
// missing equipment and the shared appearance.
//
// sub_825080 -- the first thing it consults -- gates on sub_6A87A0() = IsInLiveGame, which is TRUE
// for us only because we set coop[148] to open the menu gate (§16.4). A real couch game would be
// IsInCouchGame=1, IsInLiveGame=0; we are currently 1/1, a combination the game only ever produces
// when genuinely online. Rather than clear coop[148] and regress the menus, this hook attacks the
// decision itself and logs enough to tell us whether that trade needs revisiting.
typedef char (__fastcall *fnIsLocal)(void* entity, void* edx);
static const uintptr_t RVA_sub_825100 = 0x00425100;
static fnIsLocal g_origIsLocal = nullptr;
static bool g_localFix = true;          // "nolocalfix.txt" disables
static volatile LONG g_localFixHits = 0;

static char __fastcall Hook_IsLocal(void* entity, void* edx) {
    char r = g_origIsLocal ? g_origIsLocal(entity, edx) : 1;
    if (!entity) return r;
    // Cache the hero pointers: this runs on many call sites per frame, so do not walk the entity
    // manager every time.
    static DWORD s_tick = 0, s_h1 = 0, s_h2 = 0;
    DWORD now = GetTickCount();
    if (now - s_tick > 250) { s_tick = now; s_h1 = GetHeroEntity(0xEC); s_h2 = GetHeroEntity(0xF4); }
    DWORD e = (DWORD)(uintptr_t)entity;
    if (e != s_h1 && e != s_h2) return r;        // only care about the two heroes
    char orig = r;
    int forced = 0;
    if (g_localFix && e == s_h2 && s_h1 && !r) { // hero 2 is a LOCAL player on this machine
        r = 1;
        forced = 1;
        InterlockedIncrement(&g_localFixHits);
    }
    // Log each distinct (which-hero, original-verdict) pair once a second: without dedup this is
    // called far too often to read, and the interesting event is a CHANGE in the verdict.
    {
        static DWORD s_logTick[2] = {0, 0};
        static char  s_last[2]    = {-1, -1};
        int i = (e == s_h2) ? 1 : 0;
        if (s_last[i] != orig || now - s_logTick[i] > 1000) {
            s_last[i] = orig; s_logTick[i] = now;
            Log("[islocal] hero%d entity=%08x -> sub_825100=%d%s", i + 1, e, (int)orig,
                forced ? "  FORCED->1 (local co-op player)" : "");
        }
    }
    return r;
}

typedef char (__fastcall *fnAA1510)(void* mgr, void* edx, int entity, int mask);
typedef DWORD (__fastcall *fnAA1140)(void* mgr, void* edx, int entity);
static fnAA1510 g_origAA1510 = nullptr;
// CONFIRMED (build 42): hero2 -> BLOCKED=1 (6/6), hero1 -> BLOCKED=0 (2/2), same manager, same
// 0x40400 mask. This is the failing clause. The manager is the SCRIPT RULES manager -- the strings
// around its state names are "AddScriptRules"/"RemoveScriptRules"/"CutsceneRules"/
// "EntitiesExcludedFromNoInteractionRule"/"SetClientInSpectatorMode" -- i.e. gameplay RESTRICTIONS.
// sub_AA5B70(mgr, entity, mask, name, scope) adds them; sub_AA5CB0 removes them.
//
// Remaining question: does hero 2 genuinely carry restriction bits, or is it the sub_AA1140 miss
// (lookup returns the manager itself, so the mask lands on its vtable pointer)? Calling sub_AA1140
// directly faulted twice, so instead read the whole word through the game's OWN sub_AA1510 by
// probing one bit at a time -- no calling-convention risk, since this exact call already works.
//   * a clean handful of bits  => real script rules applied to hero 2 -> find and undo the ADD
//   * a code-address-shaped word => the unregistered-miss case -> register hero 2 instead
static DWORD g_ruleSweepTick[2] = {0, 0};
static DWORD ReadRuleWord(void* mgr, int entity) {
    DWORD w = 0;
    for (int b = 0; b < 32; b++)
        if (g_origAA1510(mgr, nullptr, entity, (int)(1u << b)) & 1) w |= (1u << b);
    return w;
}
// CONFIRMED (build 43), via the game's own sub_AA1510 probed one bit at a time:
//     hero1 ruleword = 00000000      (no script rules at all)
//     hero2 ruleword = 001D0080      (bits 7, 16, 18, 19, 20)
// Bit 18 (0x40000) is exactly what CanPerform's 0x40400 mask catches. Not a garbage vtable read --
// a clean bitfield, and hero 1's is zero. So hero 2 genuinely carries five script-rule restrictions
// that hero 1 does not, and the ability system refuses every ability for a restricted entity.
// (It is not AddRemoteHeroJoiningScriptRules: sub_AA6060 applies mask 0x200000, which hero 2 lacks.)
//
// THE FIX, first as a proof of concept at this one call site: make rule queries about hero 2 answer
// with HERO 1's rules instead of its own. Mirroring rather than blanket-clearing keeps legitimate
// lockouts intact -- during a cutscene hero 1 is restricted too, so hero 2 still is. It is exactly
// the mod's thesis: treat player 2 the way the game treats player 1.
// CONFIRMED IN-GAME (build 44): with only the CanPerform site hooked, player 2 unsheathed a weapon
// for the first time. Generalised in build 45 by repointing ALL sub_AA1510 call sites, after which
// player 2 gained menus, sprint and magic. Only NPC interact (A) still fails -- see the interaction
// exit traces below.
// Toggle: "norulefix.txt" next to Fable3.exe disables it.
static bool g_ruleFix = true;
static volatile LONG g_ruleFixHits = 0;
static char __fastcall Hook_AA1510(void* mgr, void* edx, int entity, int mask) {
    char r;
    int mirrored = 0;
    // Now on 60+ call sites, some of them hot, so the hero lookup (6 pointer hops each) is cached
    // for ~250ms rather than run twice per query.
    static DWORD s_heroTick = 0, s_hero1 = 0, s_hero2 = 0;
    DWORD nowTick = GetTickCount();
    if (nowTick - s_heroTick > 250) {
        s_heroTick = nowTick;
        s_hero1 = GetHeroEntity(0xEC);
        s_hero2 = GetHeroEntity(0xF4);
    }
    DWORD hero1 = s_hero1, hero2 = s_hero2;
    if (g_ruleFix && hero1 && hero2 && (DWORD)entity == hero2 && g_origAA1510) {
        r = g_origAA1510(mgr, edx, (int)hero1, mask);   // hero 2 inherits hero 1's script rules
        mirrored = 1;
        if (InterlockedIncrement(&g_ruleFixHits) <= 5)
            Log("[rulefix] hero2 rule query mirrored to hero1 (mask=%08x -> %d)",
                (unsigned)mask, (int)(r & 1));
    } else {
        r = g_origAA1510 ? g_origAA1510(mgr, edx, entity, mask) : 0;
    }
    if (CpLogOk()) {
        DWORD h1 = hero1, h2 = hero2;
        int idx = ((DWORD)entity == h1) ? 0 : ((DWORD)entity == h2) ? 1 : -1;
        const char* who = (idx == 0) ? "hero1" : (idx == 1) ? "hero2" : "other";
        DWORD word = 0; int swept = 0;
        if (idx >= 0) {
            DWORD now = GetTickCount();
            if (now - g_ruleSweepTick[idx] > 1000) {   // 32 extra calls: once per second per hero
                g_ruleSweepTick[idx] = now;
                __try { word = ReadRuleWord(mgr, entity); swept = 1; }
                __except (EXCEPTION_EXECUTE_HANDLER) { swept = -1; }
            }
        }
        Log("[aa1510] %s mask=%08x -> BLOCKED=%d mirrored=%d | ruleword=%08x swept=%d",
            who, (unsigned)mask, (int)(r & 1), mirrored, word, swept);
    }
    return r;
}

// ============ ABILITY-HANDLER RESOLUTION TRACE ============
// [modequeue] ret=1 has been read as "the ability worked". Re-reading sub_7F3A50 says something
// weaker and much more interesting:
//
//     if (!sub_7F3210(entity)) return 0;              // gate -- hero 2 passes ([abilitygate] empty)
//     sub_7F3660(&handler, entity, ability, params);  // RESOLVE a handler for this entity
//     if (handler) { ret = 1; ... if (!byte_1DC9D4C) (*(handler+12))(handler, entity, ...); }
//
// ret is set the moment `handler` is non-null, BEFORE the handler is invoked. So ret=1 only proves
// "some handler matched", not "the right handler ran, and it did anything".
//
// sub_7F3660 is a data-driven rule matcher: it walks a global registry (dword_1DC9D20/2C/30) and
// asks each candidate `(*(cand+8))(cand, entity, ability, &buf)` -- a CONDITION evaluated against
// THIS ENTITY -- then random-picks among the matches. So hero 1 and hero 2 can legitimately resolve
// to DIFFERENT handlers for the same button, and hero 2 may be matching a no-op reaction.
//
// This logs the resolved handler per hero. Same ability + different handler = the answer, and the
// handler address tells us which rule to look up.
static const uintptr_t RVA_sub_7F3660  = 0x003F3660;
static const uintptr_t RVA_CALL_7F3660 = 0x003F3B2C; // call site inside sub_7F3A50 (cdecl, 4 args)
typedef int* (__cdecl *fnResolve)(int* out, int entity, int ability, int* params);
static fnResolve g_origResolve = nullptr;
static int* __cdecl Hook_Resolve(int* out, int entity, int ability, int* params) {
    int* r = g_origResolve(out, entity, ability, params);
    // Only the unsheathe-melee ability is under investigation; logging every ability drowns the
    // budget and it was that noise that let build 39's sampling artifact look like a divergence.
    bool interesting = false;
    __try {
        if (ability > 0x10000)
            interesting = (0x400000 + (*(DWORD*)ability - (DWORD)(uintptr_t)g_base)) == 0x01B4E644;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (interesting && CpLogOk()) {
        DWORD abVa = 0, h = 0, hVa = 0;
        __try {
            if (ability > 0x10000)
                abVa = 0x400000 + (*(DWORD*)ability - (DWORD)(uintptr_t)g_base);
            if (out && PtrOk((DWORD)out, 4)) {
                h = *(DWORD*)out;                       // the resolved handler (v31)
                if (h > 0x10000 && PtrOk(h, 4))
                    hVa = 0x400000 + (*(DWORD*)h - (DWORD)(uintptr_t)g_base); // its vtable
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        const char* who = ((DWORD)entity == h1) ? "hero1"
                        : ((DWORD)entity == h2) ? "hero2" : "other";
        // byte_1DC9D4C is VA 0x1DC9D4C -> RVA 0x19C9D4C. The earlier 0x009C9D4C was off by
        // 0x1000000 and printed garbage ("kill=91"); it never affected behaviour, only the log.
        Log("[resolve] %s E=%08x ability_vt=%08x -> handler=%08x handler_vt=%08x kill=%d",
            who, (unsigned)entity, abVa, h, hVa, (int)*(BYTE*)(g_base + 0x019C9D4C));
    }
    return r;
}

static char __cdecl Hook_ModeQueue(int entity, int ability, int a3) {
    char r = g_origModeQueue(entity, ability, a3);
    if (AgLogOk()) {
        DWORD va = 0;
        __try {
            if (ability > 0x10000) {
                DWORD vt = *(DWORD*)ability;             // ability object's vtable -> IDA VA
                va = 0x400000 + (vt - (DWORD)(uintptr_t)g_base);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { va = 0; }
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        const char* who = ((DWORD)entity == h1) ? "hero1"
                        : ((DWORD)entity == h2) ? "hero2" : "other";
        Log("[modequeue] %s E=%08x ability_vt=%08x ret=%d", who, entity, va, (int)r);
    }
    return r;
}

// ============ INTERACT-PERFORM TRACE (the last stage) ============
// Build 28 proved the whole chain up to here works for hero 2: the mode handler fires
// CAbilityPlayerInteract on hero 2's own entity, the sub_7F3210 gate passes ([abilitygate]
// stayed empty), and the dispatcher resolves the handler ([modequeue] hero2 ... ret=1).
// The one remaining stage is CAbilityActionPlayerInteract::Perform = sub_13CBCF0 (vtbl+0xC of
// 0x1B30BFC), whose deciding inputs are, in order:
//     kill   = byte_1DC9D4C                    (global: skips Perform entirely)
//     e52    = *(entity+52) & 4                ("player-controlled" flag -- return 0 if clear)
//     c98    = component(entity, 98)           (character-interaction component)
//     target = (*(c98vtbl+0x54))(c98)          (current interact target -- return 0 if null/dead)
// Log all of them for every Perform call plus the real return value. Runs only on actual
// A-presses, on the game thread, inside the same call frame the game uses -- so reading what
// Perform itself is about to read adds no new risk. Vtable-slot patch, same proven pattern.
static const uintptr_t RVA_VT_PERFORM_INTERACT = 0x01730C08; // CAbilityActionPlayerInteract vtbl+0xC
static const uintptr_t RVA_sub_13CBCF0 = 0x00FCBCF0;         // its Perform (retn 0Ch verified)
static const uintptr_t RVA_b_killswitch = 0x019C9D4C;        // byte_1DC9D4C

typedef char (__fastcall *fnPerform)(void* self, void* edx, int entity, int a3, int a4);
static fnPerform g_origPerform = nullptr;

// ---- CGUIInput::HandleAction trace (vtable slot 1 = sub_B9BFD0) ----
// The UI merge appends pad-1 action records to the CGUIInput singleton's list, and the log confirms
// it fires ("[uimerge] ... +6 records"). What it does NOT prove is that those records reach the
// handler. CGUIInput's vtable (0x1B1A254) has 8 slots; slot 1 is sub_B9BFD0(this, action*), slot 6
// is the raw handler sub_B9B0C0. Swapping slot 1 tells us exactly which action ids arrive.
//
// Ids of interest: 14 (0x0E) = Start/pause -- sub_B9BFD0 special-cases it -- and 13 (0x0D) = interact.
// If pad 2's Start never appears here, the merge is a no-op in practice and the UI layer is not the
// place to fix this. If it DOES appear and nothing happens, the block is inside the screen stack.
static const uintptr_t RVA_VT_CGUIINPUT_ACT = 0x0171A258; // CGUIInput vtable slot 1 (VA 0x1B1A258)
static const uintptr_t RVA_sub_B9BFD0       = 0x0079BFD0;
typedef void (__fastcall *fnGuiAct)(void* self, void* edx, void* action);
static fnGuiAct g_origGuiAct = nullptr;
static volatile LONG g_guiActLogged = 0;
static void __fastcall Hook_GuiAction(void* self, void* edx, void* action) {
    // Decide provenance FIRST, then log it, then dispatch. The whole chain
    // CGUIInput::HandleAction -> sub_D78BA0 -> HUD screen -> whatever services the press
    // runs synchronously on this thread, so a scoped flag is enough to carry "this is player 2".
    LONG idx    = InterlockedIncrement(&g_actionSeq) - 1;
    LONG first  = InterlockedCompareExchange(&g_pad1FirstIdx, -1, -1);
    LONG isPad1 = (first >= 0 && idx >= first) ? 1 : 0;
    __try {
        if (action && PtrOk((DWORD)action, 40)) {
            DWORD id = *(DWORD*)action;
            if (id == 14 || id == 13 || InterlockedIncrement(&g_guiActLogged) <= 40)
                Log("[guiact] id=%u (0x%02x) idx=%d pad1First=%d -> pad=%d",
                    id, id, (int)idx, (int)first, (int)isPad1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    LONG prevPad = InterlockedExchange(&g_curActionPad, isPad1);
    if (g_origGuiAct) g_origGuiAct(self, edx, action);
    InterlockedExchange(&g_curActionPad, prevPad);
}

// ============ INTERACTION EXIT TRACE (the last broken button) ============
// After the rule fix, player 2 has menus, sprint and magic -- but A on an NPC still does nothing,
// while CAbilityActionPlayerInteract::Perform (sub_13CBCF0) keeps returning 1. Perform has many
// `return 1` exits, most of which do nothing visible, so its return value cannot distinguish
// "interacted" from "fell out early". The two exits that actually START an interaction are:
//     0x13CC2DB   call sub_12EDBF0(hero)                       -- reached via target+76 & 0x40
//     0x13CC475   call sub_D70CB0(this, 2, hero, target, 0, 0) -- the interaction state machine
// Hooking both says plainly whether hero 2 ever gets there. Note sub_D70CB0's `this` comes from the
// sub_7D4090() singleton, so only ONE interaction can be live at a time -- consistent with co-op
// menus being full-screen and shared.
static const uintptr_t RVA_sub_D70CB0   = 0x00970CB0;
static const uintptr_t RVA_CALL_D70CB0  = 0x00FCC475;
static const uintptr_t RVA_sub_12EDBF0  = 0x00EEDBF0;
static const uintptr_t RVA_CALL_12EDBF0 = 0x00FCC2DB;
typedef int (__fastcall *fnD70CB0)(void* self, void* edx, int type, int hero, int target, int a5, int a6);
typedef int (__stdcall  *fn12EDBF0)(int hero);
static fnD70CB0  g_origD70CB0  = nullptr;
static fn12EDBF0 g_orig12EDBF0 = nullptr;

typedef int (__cdecl *fnPred0)(void);
static const char* WhoHero(DWORD e) {
    DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
    return (e == h1) ? "hero1" : (e == h2) ? "hero2" : "other";
}
static int __fastcall Hook_D70CB0(void* self, void* edx, int type, int hero, int target, int a5, int a6) {
    if (AgLogOk())
        Log("[ixstart] sub_D70CB0 %s type=%d target=%08x  <- interaction state machine",
            WhoHero((DWORD)hero), type, (unsigned)target);
    return g_origD70CB0(self, edx, type, hero, target, a5, a6);
}
static int __stdcall Hook_12EDBF0(int hero) {
    if (AgLogOk())
        Log("[ixstart] sub_12EDBF0 %s  <- target+76&0x40 interaction", WhoHero((DWORD)hero));
    return g_orig12EDBF0(hero);
}

// ============ THE ACTUAL INTERACTION ENTRY ============
// MEASURED (build 46): [ixstart] was EMPTY for both heroes, and the target words were identical --
// +76 & 0x20 == 0, so BOTH heroes leave sub_13CBCF0 at the same early `return 1`. Player 1's NPC
// interaction therefore does not come from CAbilityActionPlayerInteract::Perform at all. That whole
// path, inherited from earlier sessions and carried forward by me, is not the mechanism.
//
// The real entry is sub_CB44A0 = CECCharacterInteraction::RequestInteraction(component, target):
//     if (sub_6A87A0() && this[152] == -1) { ...start a timed request... }
//     ...
//     result = (sub_673CB0() == 1) ? sub_CB2320(target) : sub_CB1EB0(1,1,1);
//     if (byte_1DD3D0C || result) {
//         sub_6BC710(target) ? sub_BDC080(374,target)+sub_CB3E00(this)
//       : sub_825100(target) ? sub_CB0F70(target)
//                            : sub_CB0A50(this, target);   // CNetRequestCharacterInteractionPacket
//     }
// Its callers include sub_1133BE0 -- the HUD screen action handler, i.e. the CGUIInput singleton
// path -- which is exactly the dispatch player 2 has never been able to reach on its own.
// this+4 is the owning entity, so this says plainly whether hero 2 ever gets here, and if it does,
// which of the two gate values differs.
//
// MEASURED (build 47): every RequestInteraction call was for hero1 -- owner=068d9f00 -- including
// when player 2 pressed A, and the owner reported player 2 interacting with PLAYER 1's prompt. So
// player 2's press reaches the interaction system fine; the HUD handler just resolves the component
// from the primary hero. THE FIX: when the action being serviced came from pad 1 (see
// g_curActionPad), swap hero 1's CECCharacterInteraction component for hero 2's.
// The component's type id is discovered once at runtime by finding which id maps to the component
// the game itself handed us, then the same id is used on hero 2 -- no hardcoded ECS id to get wrong.
// Toggle: "noixfix.txt" next to Fable3.exe disables it.
// MEASURED (build 49): [requestix] stayed EMPTY while the owner successfully used the outfit stand
// as player 2 -- and it applied to player 1. So sub_CB44A0 is not the only interaction entry either;
// different interactables reach the HUD through different handlers. Chasing them one at a time is
// the wrong shape of fix.
//
// They all share one thing: every HUD handler asks the engine "who is the player?" and gets the
// PRIMARY hero. sub_6BCB80(entityMgr) = *(mgr+236) is that question. So while we are servicing an
// action that came from pad 1, answer it with hero 2. One hook, applied at every call site, covers
// outfit stands, NPC interactions and anything else the HUD does on the player's behalf.
// The window is narrow -- only for the duration of a pad-1 UI action -- and "noixfix.txt" disables it.
// MEASURED (build 50): the swap fires correctly ([heroswap] x8, order-based provenance works) and
// player 1's outfit STILL changed. The owner identified why: the interaction prompt only appears
// when PLAYER 1 is near the stand. The target is bound to hero 1 during the per-frame prompt/candidate
// scan, long before the button is pressed -- so retargeting at press time is too late by design.
// The scan, not the press, is the thing that has to become hero-2-aware.
// Left in place but OPT-IN ("ixfix.txt" enables): it is a broad change to "who is the player" across
// every call site, and it currently buys nothing.
static bool g_ixFix = false;
static const uintptr_t RVA_sub_6BCB80 = 0x002BCB80;
typedef int (__fastcall *fnPrimaryHero)(void* self, void* edx);
static fnPrimaryHero g_origPrimaryHero = nullptr;
static volatile LONG g_primarySwapHits = 0;
static int __fastcall Hook_PrimaryHero(void* self, void* edx) {
    int r = g_origPrimaryHero(self, edx);
    if (g_ixFix && InterlockedCompareExchange(&g_curActionPad, 0, 0) == 1) {
        __try {
            DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
            if (h1 && h2 && (DWORD)r == h1) {
                if (InterlockedIncrement(&g_primarySwapHits) <= 8)
                    Log("[heroswap] pad-1 action: primary hero %08x -> hero2 %08x", (unsigned)r, h2);
                return (int)h2;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

static const uintptr_t RVA_sub_CB44A0 = 0x008B44A0;
typedef int (__fastcall *fnRequestIx)(void* self, void* edx, int target);
static fnRequestIx g_origRequestIx = nullptr;
static int  g_ixTypeId = -1;
static volatile LONG g_ixFixHits = 0;

// Which ECS type id yields `comp` on `entity`? Mirrors GetComponentOf's lookup, bounded and guarded.
static int FindCompTypeId(DWORD entity, DWORD comp) {
    if (entity <= 0x10000 || comp <= 0x10000) return -1;
    DWORD tt  = *(DWORD*)(entity + 0xA8); if (tt  <= 0x10000) return -1;
    DWORD arr = *(DWORD*)(entity + 0x58); if (arr <= 0x10000) return -1;
    for (int t = 0; t < 400; t++) {
        if (!PtrOk(tt + t, 1)) break;
        BYTE slot = *(BYTE*)(tt + t);
        if (!PtrOk(arr + 8u * slot + 4, 4)) continue;
        if (*(DWORD*)(arr + 8u * slot + 4) == comp) return t;
    }
    return -1;
}

static int __fastcall Hook_RequestIx(void* self, void* edx, int target) {
    // Route player 2's press to player 2's hero.
    if (g_ixFix && InterlockedCompareExchange(&g_curActionPad, 0, 0) == 1) {
        __try {
            DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
            DWORD owner = *(DWORD*)((DWORD)self + 4);
            if (h1 && h2 && owner == h1) {
                if (g_ixTypeId < 0) g_ixTypeId = FindCompTypeId(h1, (DWORD)self);
                DWORD c2 = (g_ixTypeId >= 0) ? GetComponentOf(h2, g_ixTypeId) : 0;
                if (c2 > 0x10000) {
                    if (InterlockedIncrement(&g_ixFixHits) <= 8)
                        Log("[ixfix] pad-1 press: RequestInteraction retargeted hero1 comp=%08x -> hero2 comp=%08x (typeId=%d)",
                            (unsigned)(uintptr_t)self, c2, g_ixTypeId);
                    self = (void*)c2;
                } else if (InterlockedIncrement(&g_ixFixHits) <= 8) {
                    Log("[ixfix] pad-1 press: could NOT resolve hero2's interaction component (typeId=%d)", g_ixTypeId);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Own budget: AgLogOk is shared with [modequeue]/[ixstart]/[abilitygate] and starved this to a
    // single line last run, which is not enough to conclude anything from.
    if (CpLogOk()) {
        DWORD owner = 0; int f148 = -1, f152 = 0; int coopActive = -1, localMode = -1;
        __try {
            owner = *(DWORD*)((DWORD)self + 4);
            f148  = *(BYTE*)((DWORD)self + 148);
            f152  = *(int*)((DWORD)self + 152);
            coopActive = ((fnPred0)(g_base + 0x002A87A0))() & 1;   // sub_6A87A0
            localMode  = ((fnPred0)(g_base + 0x00273CB0))();       // sub_673CB0
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[requestix] %s owner=%08x target=%08x pad=%d | this+148=%d this+152=%d | 6A87A0=%d 673CB0=%d",
            WhoHero(owner), owner, (unsigned)target,
            (int)InterlockedCompareExchange(&g_curActionPad, 0, 0),
            f148, f152, coopActive, localMode);
    }
    return g_origRequestIx(self, edx, target);
}

static char __fastcall Hook_InteractPerform(void* self, void* edx, int entity, int a3, int a4) {
    DWORD e52 = 0, c98 = 0, tgt = 0, tgtf = 0; int who = 0;
    __try {
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        who = ((DWORD)entity == h1) ? 1 : ((DWORD)entity == h2) ? 2 : 0;
        e52 = *(DWORD*)(entity + 52);
        c98 = GetComponentOf((DWORD)entity, 98);
        if (c98) {
            typedef DWORD (__fastcall *fnGetTgt)(DWORD self, void* edx);
            fnGetTgt get = (fnGetTgt)(*(DWORD**)c98)[0x54 / 4]; // same virtual Perform calls
            tgt = get(c98, nullptr);
            if (tgt > 0x10000) tgtf = *(BYTE*)(tgt + 172);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    char r = g_origPerform(self, edx, entity, a3, a4);
    int arm20 = -1, latch144 = -1;
    __try {   // comp98's tick path (sub_F1B8A0) opens the menu iff byte+20 arms; latch at +144
        if (c98) { arm20 = *(BYTE*)(c98 + 20); latch144 = *(BYTE*)(c98 + 144); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (AgLogOk())
        Log("[perform] hero%d E=%08x e52=%08x bit2=%d c98=%08x tgt=%08x tgtf=%02x kill=%d arm20=%d l144=%d -> ret=%d",
            who, entity, e52, (e52 & 4) ? 1 : 0, c98, tgt, tgtf,
            (int)*(volatile BYTE*)(g_base + RVA_b_killswitch), arm20, latch144, (int)r);
    // The target's own flag words decide which of sub_13CBCF0's many `return 1` exits is taken:
    //   +44 & 0x20      -> the "interacting with henchman" packet branch (else the +68 GameAction)
    //   +68 & 0x100000  -> that alternate GameAction
    //   +76 & 0x40      -> sub_12EDBF0 exit;  +76 & 0x20 == 0 -> plain `return 1`, nothing happens
    //   +48 & 0x800     -> required to continue toward sub_D70CB0
    // Pressing A on the SAME NPC as each player makes these directly comparable: identical target
    // words with different [ixstart] outcomes isolates the divergence to the hero side.
    if (tgt > 0x10000) {
        __try {
            Log("[ixtgt]  hero%d tgt=%08x +44=%08x(&20=%d) +48=%08x(&800=%d) +52=%08x(&8=%d) "
                "+68=%08x(&100000=%d) +76=%08x(&40=%d,&20=%d)",
                who, tgt,
                *(DWORD*)(tgt + 44), (*(DWORD*)(tgt + 44) & 0x20) ? 1 : 0,
                *(DWORD*)(tgt + 48), (*(DWORD*)(tgt + 48) & 0x800) ? 1 : 0,
                *(DWORD*)(tgt + 52), (*(DWORD*)(tgt + 52) & 8) ? 1 : 0,
                *(DWORD*)(tgt + 68), (*(DWORD*)(tgt + 68) & 0x100000) ? 1 : 0,
                *(DWORD*)(tgt + 76), (*(DWORD*)(tgt + 76) & 0x40) ? 1 : 0,
                (*(DWORD*)(tgt + 76) & 0x20) ? 1 : 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

// ============ INTERACTION-START TRACE (sub_BBE840) ============
// sub_BBE840 (retn 8, __thiscall on a per-hero component + 8-byte target handle) is the
// interaction-menu entry point with exactly THREE callers:
//   0x91545D  (sub_915430           -- unknown context)
//   0xF1BA3B  (sub_F1B8A0           -- comp98's per-frame tick: byte+20 && target && scheme)
//   0x13CBF5F (Perform's menu branch)
// Tag all three; resolve `this` against each hero's comp37/comp98. One run then shows which
// site fires for player 1's WORKING talk, and whether player 2 ever reaches any of them.
static const uintptr_t RVA_sub_BBE840 = 0x007BE840;
static const uintptr_t RVA_IXSITE_915 = 0x0051545D;
static const uintptr_t RVA_IXSITE_TICK = 0x00B1BA3B;
static const uintptr_t RVA_IXSITE_PERF = 0x00FCBF5F;

typedef void (__fastcall *fnEnterIx)(void* comp, void* edx, DWORD hA, DWORD hB);
static fnEnterIx g_origEnterIx = nullptr;

static void LogIxSite(const char* site, void* c) {
    if (!AgLogOk()) return;
    DWORD cc = (DWORD)(uintptr_t)c;
    const char* who = "other"; const char* comp = "?";
    __try {
        DWORD hs[2] = { GetHeroEntity(0xEC), GetHeroEntity(0xF4) };
        for (int i = 0; i < 2; i++) {
            if (!hs[i]) continue;
            if (cc == GetComponentOf(hs[i], 37)) { who = i ? "hero2" : "hero1"; comp = "c37"; break; }
            if (cc == GetComponentOf(hs[i], 98)) { who = i ? "hero2" : "hero1"; comp = "c98"; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("[enterix] via=%s %s(%s) this=%08x", site, who, comp, cc);
}
static void __fastcall Hook_Ix915(void* c, void* edx, DWORD a, DWORD b) {
    LogIxSite("site915", c); g_origEnterIx(c, edx, a, b);
}
static void __fastcall Hook_IxTick(void* c, void* edx, DWORD a, DWORD b) {
    LogIxSite("tick", c); g_origEnterIx(c, edx, a, b);
}
static void __fastcall Hook_IxPerf(void* c, void* edx, DWORD a, DWORD b) {
    LogIxSite("perform", c); g_origEnterIx(c, edx, a, b);
}

// ---- Perform branch markers ----
// Both heroes' Perform returns 1, but only player 1's interactions are visible. Perform's
// branches have distinct exits: the follow-toggle (sub_9134D0(4) at 0xFCBF02) and the world
// event-3 posts (sub_9F8AB0, retn 18h, at 0xFCC0BC / 0xFCC261) that actually hand the
// interaction to the game. Tag them; the next run shows which branch each hero takes.
static const uintptr_t RVA_sub_9F8AB0 = 0x005F8AB0;
static const uintptr_t RVA_EVSITE_A   = 0x00FCC0BC;
static const uintptr_t RVA_EVSITE_B   = 0x00FCC261;
static const uintptr_t RVA_sub_9134D0 = 0x005134D0;
static const uintptr_t RVA_FOLLOWSITE = 0x00FCBF02;

typedef int (__fastcall *fnPostEvent)(void* mgr, void* edx, int type, void* pos,
                                      int entity, int target, int a6, int a7);
static fnPostEvent g_origPostEvent = nullptr;
static void LogEvent3(const char* site, int entity, int target, int type) {
    if (!AgLogOk()) return;
    DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
    const char* who = ((DWORD)entity == h1) ? "hero1" : ((DWORD)entity == h2) ? "hero2" : "other";
    Log("[event3] %s %s E=%08x tgt=%08x type=%d", site, who, entity, target, type);
}
// THE GAMEACTION MIRROR (the candidate fix): quest scripts react to interactions by polling
// IsGameActionSentBy(hero, type) -- and they only ever ask about hero 1. Hero 2's posts are
// verified correct but no script ever matches them. So when hero 2 posts an interact action,
// post a duplicate with entity = hero 1: the NPC's script then sees "hero 1 interacted" and
// runs. Same call the game just made, same thread, same frame, only the entity differs.
// "nomirror.txt" next to Fable3.exe disables it.
static bool g_mirrorOn = true;
static int PostWithMirror(const char* site, void* m, void* edx, int t, void* p,
                          int e, int tg, int a6, int a7) {
    LogEvent3(site, e, tg, t);
    int r = g_origPostEvent(m, edx, t, p, e, tg, a6, a7);
    if (g_mirrorOn && g_joinedFlag) {
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        if (h1 && h2 && (DWORD)e == h2) {
            g_origPostEvent(m, edx, t, p, (int)h1, tg, a6, a7);
            if (AgLogOk()) Log("[mirror] hero2 action type=%d tgt=%08x reposted as hero1", t, tg);
        }
    }
    return r;
}
static int __fastcall Hook_PostEvA(void* m, void* edx, int t, void* p, int e, int tg, int a6, int a7) {
    return PostWithMirror("siteA", m, edx, t, p, e, tg, a6, a7);
}
static int __fastcall Hook_PostEvB(void* m, void* edx, int t, void* p, int e, int tg, int a6, int a7) {
    return PostWithMirror("siteB", m, edx, t, p, e, tg, a6, a7);
}
typedef char (__fastcall *fnLockRm)(void* comp, void* edx, int key);
static fnLockRm g_origLockRm = nullptr;
static char __fastcall Hook_FollowToggle(void* comp, void* edx, int key) {
    if (AgLogOk()) Log("[event3] follow-toggle branch (comp=%08x key=%d)", (DWORD)(uintptr_t)comp, key);
    return g_origLockRm(comp, edx, key);
}

static void InstallQueueHooks(BYTE* base) {
    {   // file switches next to Fable3.exe: filterpad2.txt = old drop behaviour, probe.txt = re-arm probe
        char path[MAX_PATH], *slash;
        if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
            slash = strrchr(path, '\\');
            lstrcpyA(slash ? slash + 1 : path, "filterpad2.txt");
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_filterQueue = true;
            lstrcpyA(slash ? slash + 1 : path, "probe.txt");
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) g_probeEnabled = true;
            lstrcpyA(slash ? slash + 1 : path, "nocensus.txt");
            g_censusOn = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            // UI merge is now OPT-IN: it demonstrably fed CGUIInput pad-1 records and nothing
            // happened, because CGUIInput bails immediately when no UI screen is open (this+8).
            // Don't ship a behaviour change that does nothing; "uimerge.txt" re-enables it.
            lstrcpyA(slash ? slash + 1 : path, "uimerge.txt");
            g_uiMerge = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "gatetrace.txt");
            g_gateTrace = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "norawfix.txt");
            g_rawFix = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "noabilityfix.txt");
            g_abilityFix = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nomirror.txt");
            g_mirrorOn = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nocoopfix.txt");
            g_coopFix = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nocoopactive.txt");
            g_coopActive = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "norulefix.txt");
            g_ruleFix = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "ixfix.txt");   // opt-in; see the note at g_ixFix
            g_ixFix = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nocouchmode.txt");
            g_couchMode = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nolocalfix.txt");
            g_localFix = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
            lstrcpyA(slash ? slash + 1 : path, "nojoin.txt");
            g_joinTrigger = (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
        }
    }
    Log("[queue] census=%s UI-merge=%s gate-trace=%s raw-event-fix=%s  ABILITY-FIX=%s  COOP-ACTIVE=%s",
        g_censusOn ? "ON" : "OFF", g_uiMerge ? "ON" : "OFF", g_gateTrace ? "ON" : "OFF",
        g_rawFix ? "ON" : "OFF", g_abilityFix ? "ON" : "OFF", g_coopActive ? "ON" : "OFF");
    Log("[queue] RULE-FIX (hero2 script rules mirror hero1) = %s", g_ruleFix ? "ON" : "OFF");
    Log("[queue] COUCH-MODE (coop+0xB8 = 1 -> IsInCouchGame) = %s", g_couchMode ? "ON" : "OFF");
    Log("[queue] LOCAL-FIX (sub_825100: hero2 is a LOCAL player) = %s", g_localFix ? "ON" : "OFF");
    Log("[queue] JOIN-TRIGGER (pad2 Start -> synthetic join) = %s%s", g_joinTrigger ? "ON" : "OFF",
        g_joinTrigger ? "  [in-world only; refused at menus]" : "  [Start passed through to the game]");
    // THE ROUTING FIX (two instructions in sub_D89DE0's per-event loop):
    //   1) keep the event's real source-pad index (in edx) instead of wiping it to 0
    //   2) file the event under THAT pad as the bucket key instead of hardcoded 0
    // Apply the key patch FIRST so we never end up with a half-applied pair.
    bool k = ApplyBytes(base, RVA_BUCKETKEY, ORIG_BUCKETKEY, sizeof(ORIG_BUCKETKEY),
                        NEW_BUCKETKEY, sizeof(NEW_BUCKETKEY), "bucket key = event pad (sub_D89DE0)");
    if (k)
        ApplyBytes(base, RVA_TAGWIPE, ORIG_TAGWIPE, sizeof(ORIG_TAGWIPE),
                   NEW_TAGWIPE, sizeof(NEW_TAGWIPE), "load event pad into edx (sub_D89DE0)");
    else
        Log("[patch] routing fix SKIPPED (bucket-key site did not match)");
    InitializeCriticalSection(&g_stashCs);
    g_stashCsReady = true;
    g_origPollDriver = (fnPollDriver)(base + RVA_sub_188EFB0);
    g_origPump2      = (fnPump2)(base + RVA_sub_752E80);
    g_origGameInput  = (fnGameInput)(base + RVA_sub_753EC0);
    PatchRel32(base, RVA_CALL_GAMEINP, 0xE8, base + RVA_sub_753EC0,
               (void*)&Hook_GameInput, "gameinput call@65B9A0");
    PatchRel32(base, RVA_JMP_POLLDRV, 0xE9, base + RVA_sub_188EFB0,
               (void*)&Hook_PollDriver, "polldriver jmp@188DDFD");
    PatchRel32(base, RVA_CALL_PUMP2_A, 0xE8, base + RVA_sub_752E80,
               (void*)&Hook_Pump2, "pump2 call@753373");
    PatchRel32(base, RVA_CALL_PUMP2_B, 0xE8, base + RVA_sub_752E80,
               (void*)&Hook_Pump2, "pump2 call@753D79");
    // These three are needed by the UI merge as well as the census, and have proven stable, so
    // install them unconditionally; only the logging is gated on g_censusOn.
    g_origD89DE0 = (fnBuildActions)(base + RVA_sub_D89DE0);
    g_origD89480 = (fnFetchActions)(base + RVA_sub_D89480);
    g_origB6CF30 = (fnDeliver)(base + RVA_sub_B6CF30);
    g_cenLast = GetTickCount();
    PatchRel32(base, RVA_CALL_D89DE0, 0xE8, base + RVA_sub_D89DE0,
               (void*)&Hook_D89DE0, "buildactions call@B6DDA0");
    PatchRel32(base, RVA_CALL_D89480, 0xE8, base + RVA_sub_D89480,
               (void*)&Hook_D89480, "fetchactions call@B6DE83");
    PatchRel32(base, RVA_CALL_B6CF30, 0xE8, base + RVA_sub_B6CF30,
               (void*)&Hook_B6CF30, "deliver call@B6DE8C (UI merge)");
    // THE RAW-EVENT FIX: make sub_B6D020's player-1-hardcoded pad filter per-listener.
    if (g_rawFix) {
        g_origB6D020 = (fnRawPass)(base + RVA_sub_B6D020);
        g_origGetP1  = (fnGetPlayer1)(base + RVA_sub_6B7310);
        bool a = PatchRel32(base, RVA_CALL_B6D020, 0xE8, base + RVA_sub_B6D020,
                            (void*)&Hook_B6D020, "rawfix: rawpass call@B6DE25");
        bool b = PatchRel32(base, RVA_CALL_6B7310, 0xE8, base + RVA_sub_6B7310,
                            (void*)&Hook_GetPlayer1, "rawfix: GetPlayer1 call@B6D076");
        if (!a || !b) Log("[rawfix] INCOMPLETE (a=%d b=%d) -- raw path left stock", a ? 1 : 0, b ? 1 : 0);
    } else {
        Log("[rawfix] disabled by norawfix.txt");
    }
    // THE ABILITY-GATE FIX: log every mode-handler ability queue, and lift the type-2 device
    // suppression for hero 2 at the sub_7F3210 gate (see the block comment above the hooks).
    g_fnLockCheck     = (fnLockCheck)(base + RVA_sub_9134F0);
    g_origAbilityGate = (fnAbilityGate)(base + RVA_sub_7F3210);
    g_origModeQueue   = (fnAbilityQueue)(base + RVA_sub_7F3A50_q);
    PatchRel32(base, RVA_CALL_7F3210, 0xE8, base + RVA_sub_7F3210,
               (void*)&Hook_AbilityGate, "abilityfix: gate call@7F3A6F");
    PatchRel32(base, RVA_CALL_7F3A50_MODE, 0xE8, base + RVA_sub_7F3A50_q,
               (void*)&Hook_ModeQueue, "abilityfix: modequeue call@1121F54");
    g_origAA1510 = (fnAA1510)(base + RVA_sub_AA1510);
    // Every script-rule mask query in the game, not just CanPerform's. Confirmed in-game: with only
    // the CanPerform site hooked, player 2 could unsheathe a weapon for the first time.
    RepointAllCallsTo(base, RVA_sub_AA1510, (void*)&Hook_AA1510, "rulefix: sub_AA1510 (all sites)");
    // The local-vs-remote decision behind the missing prompt (see the block at Hook_IsLocal).
    // Repointed at every site because the same predicate gates presentation, abilities and
    // inventory -- not just the emotion icon.
    g_origIsLocal = (fnIsLocal)(base + RVA_sub_825100);
    RepointAllCallsTo(base, RVA_sub_825100, (void*)&Hook_IsLocal,
                      "islocal: sub_825100 local-vs-network (all sites)");
    g_origCanUnsheathe = (fnCanPerform)(base + RVA_sub_13CC4B0);
    PatchVtableSlot(base, RVA_VT_UNSHEATHE_CAN, base + RVA_sub_13CC4B0,
                    (void*)&Hook_CanUnsheathe, "canperform: CAbilityActionUnsheatheMeleeWeapon");
    g_origPrimaryHero = (fnPrimaryHero)(base + RVA_sub_6BCB80);
    RepointAllCallsTo(base, RVA_sub_6BCB80, (void*)&Hook_PrimaryHero,
                      "heroswap: sub_6BCB80 GetPrimaryHero (all sites)");
    g_origRequestIx = (fnRequestIx)(base + RVA_sub_CB44A0);
    RepointAllCallsTo(base, RVA_sub_CB44A0, (void*)&Hook_RequestIx,
                      "requestix: CECCharacterInteraction::RequestInteraction (all sites)");
    g_origD70CB0  = (fnD70CB0)(base + RVA_sub_D70CB0);
    PatchRel32(base, RVA_CALL_D70CB0, 0xE8, base + RVA_sub_D70CB0,
               (void*)&Hook_D70CB0, "ixstart: sub_D70CB0@13CC475");
    g_orig12EDBF0 = (fn12EDBF0)(base + RVA_sub_12EDBF0);
    PatchRel32(base, RVA_CALL_12EDBF0, 0xE8, base + RVA_sub_12EDBF0,
               (void*)&Hook_12EDBF0, "ixstart: sub_12EDBF0@13CC2DB");
    g_origResolve = (fnResolve)(base + RVA_sub_7F3660);
    PatchRel32(base, RVA_CALL_7F3660, 0xE8, base + RVA_sub_7F3660,
               (void*)&Hook_Resolve, "ability handler resolve@7F3B2C");
    g_origGuiAct = (fnGuiAct)(base + RVA_sub_B9BFD0);
    PatchVtableSlot(base, RVA_VT_CGUIINPUT_ACT, base + RVA_sub_B9BFD0,
                    (void*)&Hook_GuiAction, "guiact trace: CGUIInput::HandleAction");
    g_origPerform = (fnPerform)(base + RVA_sub_13CBCF0);
    PatchVtableSlot(base, RVA_VT_PERFORM_INTERACT, base + RVA_sub_13CBCF0,
                    (void*)&Hook_InteractPerform, "perform trace: CAbilityActionPlayerInteract");
    g_origEnterIx = (fnEnterIx)(base + RVA_sub_BBE840);
    PatchRel32(base, RVA_IXSITE_915, 0xE8, base + RVA_sub_BBE840,
               (void*)&Hook_Ix915, "enterix site@91545D");
    PatchRel32(base, RVA_IXSITE_TICK, 0xE8, base + RVA_sub_BBE840,
               (void*)&Hook_IxTick, "enterix site@F1BA3B");
    PatchRel32(base, RVA_IXSITE_PERF, 0xE8, base + RVA_sub_BBE840,
               (void*)&Hook_IxPerf, "enterix site@13CBF5F");
    g_origPostEvent = (fnPostEvent)(base + RVA_sub_9F8AB0);
    g_origLockRm    = (fnLockRm)(base + RVA_sub_9134D0);
    PatchRel32(base, RVA_EVSITE_A, 0xE8, base + RVA_sub_9F8AB0,
               (void*)&Hook_PostEvA, "event3 site@13CC0BC");
    PatchRel32(base, RVA_EVSITE_B, 0xE8, base + RVA_sub_9F8AB0,
               (void*)&Hook_PostEvB, "event3 site@13CC261");
    PatchRel32(base, RVA_FOLLOWSITE, 0xE8, base + RVA_sub_9134D0,
               (void*)&Hook_FollowToggle, "follow site@13CBF02");
    // Handler gate trace: OPT-IN ONLY. It targets CInputProcessInteract, which §11 proves is a
    // dead end (both heroes fail its g5 gate), and it is the code that crashed twice. Create
    // "gatetrace.txt" next to Fable3.exe to re-enable it for future work.
    if (g_gateTrace) {
        g_sub9134F0    = (fnPred1)(base + RVA_sub_9134F0);
        g_sub8C7C60    = (fnPred0)(base + RVA_sub_8C7C60);
        g_origInteract = (fnHandle)(base + RVA_sub_1355000);
        g_origCombat   = (fnHandle)(base + RVA_sub_1354000);
        PatchVtableSlot(base, RVA_VT_INTERACT, base + RVA_sub_1355000,
                        (void*)&Hook_Interact, "gate: CInputProcessInteract::Handle");
        PatchVtableSlot(base, RVA_VT_COMBAT, base + RVA_sub_1354000,
                        (void*)&Hook_Combat, "gate: CInputProcessCombat::Handle");
    }
}

// --------- Routing verification (READ-ONLY) ---------
// After player 2 exists, once per second sample each pad's left stick and each hero's LATCHED
// control direction (CECPlayerControl +0xC0/+0xC4, written by the game's own input pump). Logs only
// while a stick is pushed, so the correlation is obvious:
//   push ONLY pad 1  ->  we want hero2 latch to move and hero1 latch to stay ~0  (routing worked)
//   if pad 1 moves hero1 latch instead                                            (still aggregated)
// This writes NOTHING into the game (unlike the old DriveHero2FromPad1) so it can't corrupt state.
static int AbsI(int v) { return v < 0 ? -v : v; }

// Dump the game's sign-in slot table (read-only). sub_6A66C0() == global dword_1DC2550; the sign-in
// manager is *(that+40) (sub_6A6960); its slot array base is *(mgr+20), 32-byte slots. sub_771D60
// (the gate that shows the "not a gamer profile" warning and keeps the game in 1-player mode) reads
// slot: (b0||b1) && !b2. We need to know, for controller 1, whether the slot is fully populated with
// data but missing the flag (=> a safe flag-write fixes it) or entirely empty (=> we must run the
// game's own sign-in sync). data[] shows the bytes right after the 3 flag bytes.
static void LogSigninState() {
    DWORD owner = *(DWORD*)(g_base + RVA_signinOwner);
    if (owner <= 0x10000) { Log("[signin] owner(dword_1DC2550) null"); return; }
    // Coop-mode / local-player-count gate: sub_673CB0() == *(*(owner+16)+0xB8). In sub_753010 the
    // player-2 input branch runs iff this == 1. If it's not 1, the native second-player pump is off.
    DWORD coopObj = *(DWORD*)(owner + 16);
    if (coopObj > 0x10000)
        Log("[gate] sub_673CB0 (coop-mode/local-players) = %d   (player-2 pump runs iff ==1)",
            *(int*)(coopObj + 0xB8));
    DWORD mgr = *(DWORD*)(owner + 40);
    if (mgr <= 0x10000) { Log("[signin] mgr null"); return; }
    DWORD b = *(DWORD*)(mgr + 20), e = *(DWORD*)(mgr + 24);
    if (b <= 0x10000 || e < b) { Log("[signin] slotbase bad b=%08x e=%08x", b, e); return; }
    int n = (int)((e - b) / 32);
    Log("[signin] mgr=%08x slots=%d", mgr, n);
    for (int i = 0; i < n && i < 4; i++) {
        BYTE* s = (BYTE*)(b + 32 * i);
        int ok = ((s[1] || s[0]) && !s[2]);
        Log("[signin] idx%d b0=%u b1=%u guest(b2)=%u -> 771D60=%d  data=%02x %02x %02x %02x %02x %02x %02x %02x",
            i, s[0], s[1], s[2], ok, s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11]);
    }
}
// --------- Hero capability diff (READ-ONLY) ---------
// The action stream reaching hero 2 is now identical to hero 1's, and hero 2 owns the same
// CInputProcessInteract / CInputProcessCombat listeners. So the rejection is inside those handlers,
// and both of them gate on bits of the entity's component bitmask before doing anything:
//
//   CInputProcessInteract::Handle (sub_1355000), action id 13:
//       E = **(this+12)
//       (*(BYTE *)(E+172) & 2)          -- entity alive/valid
//       (*(DWORD*)(E+44)  & 0x20)       -- has component X
//       comp37 = component(E, 37)       -- control scheme
//       comp37[+20] != 0                -- sub_9134F0's own gate
//       sub_BBE560(comp37, 3)           -- co-op/ability permission, via sub_BFDDA0(comp37[+48], 3)
//       (*(BYTE *)(E+55)  & 1)          -- has component Y
//   CInputProcessCombat::Handle (sub_1354000), action id 74:
//       (*(BYTE *)(E+172) & 2) && (*(DWORD*)(E+40) & 0x10000000)
//
// Every one of those is a plain read off the entity, so we can evaluate them for BOTH heroes from
// this watchdog thread without touching the game. Whichever word differs is the answer.
static void LogHeroCaps(DWORD h1, DWORD h2) {
    // Live coop-active readout. sub_683770(coop) == (coop[60] && coop[62]) || coop[148], and that is
    // exactly what sub_6A87A0() returns. sub_7D7000 -- the menu/pause opener -- refuses any player
    // key != 1 while this is FALSE:
    //     if (!sub_6A87A0() && a1 != 1 && !flag) return 1;   // refuse to open
    // so this must be TRUE at the moment the button is pressed, not merely at join time.
    // Computed by direct reads (no calls into the engine) so the watchdog thread stays harmless.
    {
        DWORD coop = GetCoopObj();
        if (coop && PtrOk(coop + 148, 4)) {
            int active = ((*(BYTE*)(coop + 60) && *(BYTE*)(coop + 62)) || *(DWORD*)(coop + 148)) ? 1 : 0;
            // Correct names (script table sub_6A6A70): 6A87A0 = IsInLiveGame, 6A8760 = IsClient.
            // mode = coop+0xB8; IsInCouchGame() is mode==1. Tracked every tick so we can see if
            // anything later RESETS it -- a one-shot read at join time would not show that.
            Log("[coopnow] 60=%u 61=%u 62=%u 148=%u -> IsInLiveGame(6A87A0)=%d  IsClient(6A8760)=%d  mode[B8]=%d IsInCouchGame=%d",
                *(BYTE*)(coop + 60), *(BYTE*)(coop + 61), *(BYTE*)(coop + 62),
                *(DWORD*)(coop + 148), active,
                (*(BYTE*)(coop + 60) && *(BYTE*)(coop + 62)) ? 1 : 0,
                PtrOk(coop + 0xB8, 4) ? *(int*)(coop + 0xB8) : -999,
                (PtrOk(coop + 0xB8, 4) && *(int*)(coop + 0xB8) == 1) ? 1 : 0);
        }
    }
    // Each hero's CECCharacterInteraction component (vtable 0x1B2533C), found by scanning component
    // ids rather than assuming one, plus its live state: +148 = "request in flight", +152 = request
    // handle (-1 = none), +160 = tracking pointer to the entity it is focused on.
    // The owner's report that player 2 gets NO interaction prompt at the outfit stand suggests the
    // presentation side never targets hero 2 either, so this shows whether hero 2's component is
    // even tracking anything.
    {
        DWORD wantVt = (DWORD)(uintptr_t)(g_base + 0x0172533C);   // CECCharacterInteraction vtable
        for (int i = 0; i < 2; i++) {
            DWORD e = i ? h2 : h1, found = 0;
            if (e > 0x10000) {
                for (int t = 0; t < 400; t++) {
                    DWORD c = GetComponentOf(e, t);
                    if (c > 0x10000 && PtrOk(c, 4) && *(DWORD*)c == wantVt) { found = c;
                        DWORD tgt = 0;
                        __try {
                            DWORD tp = *(DWORD*)(found + 160);
                            if (tp > 0x10000 && PtrOk(tp, 4)) tgt = *(DWORD*)tp;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {}
                        Log("[ixcomp] hero%d comp=%08x typeId=%d +148=%d +152=%d focus=%08x",
                            i + 1, found, t, *(BYTE*)(found + 148), *(int*)(found + 152), tgt);
                        break;
                    }
                }
            }
            if (!found) Log("[ixcomp] hero%d: NO CECCharacterInteraction component", i + 1);
        }
    }
    const DWORD hs[2] = { h1, h2 };
    for (int i = 0; i < 2; i++) {
        DWORD e = hs[i];
        if (e <= 0x10000) { Log("[caps] hero%d: entity null", i + 1); continue; }
        DWORD w40 = *(DWORD*)(e + 40), w44 = *(DWORD*)(e + 44), w48 = *(DWORD*)(e + 48);
        DWORD w52 = *(DWORD*)(e + 52); // flags dword: bit2 gates CAbilityActionPlayerInteract::Perform
        DWORD w56 = *(DWORD*)(e + 56), w60 = *(DWORD*)(e + 60), w64 = *(DWORD*)(e + 64);
        BYTE  b55 = *(BYTE*)(e + 55), b172 = *(BYTE*)(e + 172);
        Log("[caps] hero%d e=%08x  +40=%08x +44=%08x +48=%08x +52=%08x(bit2=%d) +55=%02x +56=%08x +60=%08x +64=%08x +172=%02x",
            i + 1, e, w40, w44, w48, w52, (w52 & 4) ? 1 : 0, b55, w56, w60, w64, b172);
        DWORD c37 = GetComponentOf(e, TYPE_CONTROLSCHEME);
        DWORD c28 = GetComponentOf(e, 28);
        // sub_9134F0 is `mov ecx,[ecx+14h] ; jmp sub_BBE560` -- the g5 gate runs on the control
        // scheme's DEVICE (comp37+0x14), and sub_BBE560 keys its registry lookup on device+0x30
        // (device+0x34 is the controller id). If device+0x30 differs, that is the lookup key that
        // has no entry for player 2.
        DWORD dev = (c37 && PtrOk(c37 + 0x14, 4)) ? *(DWORD*)(c37 + 0x14) : 0;
        if (PtrOk(dev + 0x30, 8))
            Log("[caps] hero%d device=%08x  +0x30=%08x (registry key)  +0x34=%d (controller)",
                i + 1, dev, *(DWORD*)(dev + 0x30), *(int*)(dev + 0x34));
        else
            Log("[caps] hero%d device=%08x  (unreadable/null -- g5 fails immediately)", i + 1, dev);
        Log("[caps] hero%d gates: alive(172&2)=%d interact(44&0x20)=%d hasY(55&1)=%d "
            "combat(40&0x10000000)=%d comp37=%08x comp37+20=%08x comp37+48=%08x comp28=%08x",
            i + 1, (b172 & 2) ? 1 : 0, (w44 & 0x20) ? 1 : 0, (b55 & 1) ? 1 : 0,
            (w40 & 0x10000000) ? 1 : 0, c37,
            c37 ? *(DWORD*)(c37 + 20) : 0, c37 ? *(DWORD*)(c37 + 48) : 0, c28);
    }
}

static LONG g_devidLogged = 0;
static unsigned g_capTick = 0;
static DWORD WINAPI RouteWatch(LPVOID) {
    for (;;) {
        Sleep(1000);
        if (!g_xiGetState) continue;
        DWORD h1 = GetHeroEntity(0xEC), h2 = GetHeroEntity(0xF4);
        if (!h2) continue;                                   // player 2 not spawned yet
        // One-shot: dump each hero's game-assigned controller id the moment player 2 exists. This is
        // the routing ground-truth -- we expect hero1 dev=0 and want hero2 dev=1; anything else
        // (esp. hero2 dev=-1/0) explains why pad 1 still drives hero 1.
        if (InterlockedExchange(&g_devidLogged, 1) == 0) {
            Log("[devid] hero1=%08x dev=%d   hero2=%08x dev=%d   (want 0 and 1)",
                h1, HeroDeviceId(h1), h2, HeroDeviceId(h2));
            LogSigninState(); // dump sign-in slots to see if controller 1 is a recognized local user
            LogHeroCaps(h1, h2);
        }
        // Re-dump the capability words every 5s: some of these bits are set later by the ability
        // system, so a one-shot at spawn time could read them before hero 2 is fully constructed.
        if ((++g_capTick % 5) == 0) LogHeroCaps(h1, h2);
        DWORD c1 = h1 ? GetComponentOf(h1, TYPE_CECPLAYERCONTROL) : 0;
        DWORD c2 = h2 ? GetComponentOf(h2, TYPE_CECPLAYERCONTROL) : 0;
        int p0x = 0, p0y = 0, p1x = 0, p1y = 0;
        XI_STATE s;
        memset(&s, 0, sizeof(s)); if (g_xiGetState(0, &s) == ERROR_SUCCESS) { p0x = s.Gamepad.sLX; p0y = s.Gamepad.sLY; }
        memset(&s, 0, sizeof(s)); if (g_xiGetState(1, &s) == ERROR_SUCCESS) { p1x = s.Gamepad.sLX; p1y = s.Gamepad.sLY; }
        if (AbsI(p0x) < 9000 && AbsI(p0y) < 9000 && AbsI(p1x) < 9000 && AbsI(p1y) < 9000) continue;
        int l1x = c1 ? (int)(*(volatile float*)(c1 + 0xC0) * 1000.0f) : 0;
        int l1y = c1 ? (int)(*(volatile float*)(c1 + 0xC4) * 1000.0f) : 0;
        int l2x = c2 ? (int)(*(volatile float*)(c2 + 0xC0) * 1000.0f) : 0;
        int l2y = c2 ? (int)(*(volatile float*)(c2 + 0xC4) * 1000.0f) : 0;
        Log("[route] pad0(%d,%d) pad1(%d,%d) | hero1 dev=%d latch(%d,%d) | hero2 dev=%d latch(%d,%d)",
            p0x / 328, p0y / 328, p1x / 328, p1y / 328,
            HeroDeviceId(h1), l1x, l1y, HeroDeviceId(h2), l2x, l2y);
    }
}

// Vectored exception handler: logs the first fatal crash (EIP + in-module stack) so we can see
// exactly where the join dies. Crashes don't trigger the watchdog, so this is our only visibility.
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Guard-page probe hit: this faulting EIP is the code that touched the input event queue.
    if (code == STATUS_GUARD_PAGE_VIOLATION && g_base) {
        LONG h = InterlockedIncrement(&g_probeHits);
        if (h <= PROBE_MAX_HITS) {
            DWORD base = (DWORD)(uintptr_t)g_base;
            DWORD eip  = ep->ContextRecord->Eip;
            DWORD addr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
            bool inMod = (eip >= base && eip < base + 0x1C00000);
            Log("[probe] #%d %s EIP=%08x rva=%08x addr=%08x tid=%u",
                h, ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ ",
                eip, inMod ? (eip - base + 0x400000) : 0, addr, GetCurrentThreadId());
        }
        return EXCEPTION_CONTINUE_EXECUTION; // guard already cleared; re-run the instruction
    }
    bool fatal = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                  code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                  code == 0xC0000409 /*__fastfail*/ || code == EXCEPTION_STACK_OVERFLOW);
    static LONG logged = 0;
    if (fatal && g_base && InterlockedExchange(&logged, 1) == 0) {
        DWORD base = (DWORD)(uintptr_t)g_base;
        CONTEXT* c = ep->ContextRecord;
        DWORD eip = c->Eip;
        Log("[crash] code=%08x tid=%u EIP=%08x (rva=%08x) ESP=%08x", code, GetCurrentThreadId(),
            eip, (eip >= base && eip < base + 0x1C00000) ? eip - base + 0x400000 : eip, c->Esp);
        DWORD lo = base + 0x1000, hi = base + 0x1C00000; int found = 0;
        __try {
            for (DWORD p = c->Esp; p < c->Esp + 0x400 && found < 32; p += 4) {
                DWORD v = *(DWORD*)p;
                if (v >= lo && v < hi) { Log("[crash]   [%08x]=%08x rva=%08x", p, v, v - base + 0x400000); found++; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_CONTINUE_SEARCH; // let the game handle/crash normally
}

static volatile LONG g_inited = 0;

static DWORD Init() {
    if (InterlockedCompareExchange(&g_inited, 1, 0) != 0)
        return ERROR_SUCCESS; // run once (XLLNModulePostInit and/or DllMain fallback)

    char exe[MAX_PATH] = {0}; GetModuleFileNameA(NULL, exe, MAX_PATH);
    Log("[init] === couchcoop module init ===  host=%s", exe);
    Log("[init] sign-in: pass-through to XLiveLessNess (player 2 = XLLN user index 1)");
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(NULL)); // Fable3.exe image base
    if (!base) { Log("[init] no module base"); return (DWORD)E_FAIL; }
    g_base = base;
    Log("[init] image base=%08x", (unsigned)(uintptr_t)base);
    AddVectoredExceptionHandler(1, CrashHandler); // capture the first fatal crash location

    HMODULE xi = LoadLibraryA("xinput1_3.dll");
    if (xi) g_xiGetState = reinterpret_cast<fnXInputGetState>(GetProcAddress(xi, "XInputGetState"));
    Log("[init] xinput1_3 XInputGetState=%s  pad1_connected=%d pad2_connected=%d pad3_connected=%d",
        g_xiGetState ? "ok" : "MISSING", PadConnected(1), PadConnected(2), PadConnected(3));

    ApplyInputPatch(base); // guarded; safe no-op if the original bytes don't match
    // Sign-in gate patches REMOVED: forcing the sign-in check true leaves player 2 half-initialized
    // -> crash in the bind. Correct approach is to have XLiveLessNess actually sign player 2 in so the
    // game's native check passes on its own. (ApplyBytes helper kept for future use.)
    (void)&ApplyBytes; (void)ORIG_7718B0; (void)NEW_7718B0; (void)ORIG_BINDGATE; (void)NEW_BINDGATE;

    HookIATSlot(base, RVA_IAT_GetSigninState, (void*)&Hook_XUserGetSigninState, (void**)&g_origState);
    HookIATSlot(base, RVA_IAT_GetSigninInfo,  (void*)&Hook_XUserGetSigninInfo,  (void**)&g_origInfo);
    HookIATSlot(base, RVA_IAT_GetName,        (void*)&Hook_XUserGetName,        (void**)&g_origName);
    HookIATSlot(base, RVA_IAT_CheckPrivilege, (void*)&Hook_XUserCheckPrivilege, (void**)&g_origPriv);
    HookIATSlot(base, RVA_IAT_XInputGetState, (void*)&Hook_GameXInputGetState,  (void**)&g_origGameXI);
    HookIATSlot(base, RVA_IAT_ShowSigninUI,   (void*)&Hook_XShowSigninUI,       (void**)&g_origShow);
    // Run player-2 spawn from the game's own drop-in dispatch (input thread), the way it worked
    // before. NO XLLNLogin (it collides with the manual guest sign-in) and NO D3D Present join
    // (blocks the render thread / breaks the sign-in dialog). Sign-in stays 100% XLLN's job.
    HookIATSlot(base, RVA_cb_DropIn, (void*)&Hook_DropIn, (void**)&g_origDropIn);
    Log("[init] drop-in callback hooked  orig=%08x", (unsigned)(uintptr_t)g_origDropIn);
    InstallQueueHooks(base); // shared-input-queue observation (poll driver + player-2 pump)
    Log("[init] IAT hooks installed  state_orig=%08x info_orig=%08x xinput_orig=%08x  DONE",
        (unsigned)(uintptr_t)g_origState, (unsigned)(uintptr_t)g_origInfo, (unsigned)(uintptr_t)g_origGameXI);
    CloseHandle(CreateThread(nullptr, 0, Watch, base, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, JoinWatchdog, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, RouteWatch, nullptr, 0, nullptr)); // read-only pad->hero routing log
    return ERROR_SUCCESS;
}

// XLiveLessNess module entrypoint. XLLN loads DLLs from ./xlln/modules/ and calls the
// export at ordinal 41101 AFTER the host's imports are bound (see xlln-modules.cpp). This
// is the correct, race-free place to patch + hook. Exported at ordinal 41101 via the .def.
extern "C" __declspec(dllexport) DWORD WINAPI XLLNModulePostInit() {
    return Init();
}

static DWORD WINAPI InitThread(LPVOID) { Init(); return 0; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Fallback for standalone injection (non-XLLN). Off the loader lock; the one-shot
        // guard makes it a no-op if XLLNModulePostInit already ran.
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
