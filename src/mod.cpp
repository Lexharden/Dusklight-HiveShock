// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// mod.cpp — Main entry point & lifecycle
//
// This is the root translation unit for HiveShock.  It:
//   1. Declares mod metadata via DEFINE_MOD().
//   2. Imports the LogService for isolated HiveShock logging.
//   3. Initialises the NetBridge TCP server on a background thread.
//   4. Installs a pre-hook on fapGm_Execute to drain the command queue
//      and dispatch effects exclusively on the main thread.
//   5. Provides a clean shutdown path that tears down the server and
//      removes hooks before the mod is unloaded.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================

// --- Mod SDK boilerplate (must come first) ---
#include <mods/api.h>
#include <mods/service.hpp>
#include <mods/svc/log.h>
#include <mods/svc/log.hpp>
#include <mods/svc/hook.h>
#include <mods/svc/hook.hpp>

// --- HiveShock modules ---
#include "logging.hpp"
#include "net_bridge.hpp"
#include "effects.hpp"

// --- Engine headers (for the hooked function signature) ---
#include "f_ap/f_ap_game.h"           // fapGm_Execute
#include "d/d_com_inf_game.h"         // dComIfGp_getPlayer
#include "d/d_meter2.h"               // dMeter2_c::_draw — donor-nametag hook target

// --- Crash handler (MUST come after engine headers to avoid Windows.h macro collisions) ---
#include "crash_handler.hpp"

// --- Standard library ---
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

// =============================================================================
// Mod metadata — DEFINE_MOD() emits the required mod_ctx / mod_meta symbols.
// =============================================================================
DEFINE_MOD();

// =============================================================================
// Service imports
// =============================================================================
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

// =============================================================================
// Hook declaration — we intercept the main game-loop tick function so we can
// drain the command queue and dispatch effects on the main thread.
// =============================================================================
DEFINE_HOOK(&fapGm_Execute, HiveShockTick);

// =============================================================================
// Second hook — donor-nametag drawing. Ported from tiktok_bridge
// (dusklight_1.4.1): the 2D drawlist is reset every frame, and dMeter2_c's
// draw happens right after that reset, which is the only point where an
// entry queued into it actually survives to that frame's Painter pass.
// Queuing nametags from the regular per-frame HiveShockTick hook above is
// too early — see EffectDispatcher::submitNametags().
// =============================================================================
DEFINE_HOOK(&dMeter2_c::_draw, MeterDraw);

// =============================================================================
// Module-level state
// =============================================================================
namespace {

/// TCP server that receives external commands on port 43000.
std::unique_ptr<hiveshock::NetBridge>       g_netBridge;

/// Translates commands into in-game effects (spawn, damage, etc.).
std::unique_ptr<hiveshock::EffectDispatcher> g_effects;

/// Scratch buffer reused each frame to avoid per-frame allocations.
std::vector<hiveshock::CommandEvent>         g_frameEvents;

/// Commands EffectDispatcher::dispatch() reported as deferred (scene not
/// ready — e.g. sent mid-door-transition), waiting to be retried on a later
/// frame. Without this, such a command would just be dropped, even though
/// the condition that blocked it clears itself a moment later on its own
/// (confirmed via hiveshock_forensic.log — the gating always self-recovers,
/// commands just never got a second chance to run once it did).
std::deque<hiveshock::CommandEvent>          g_deferredEvents;

/// Hard cap on g_deferredEvents — if a player spams commands during a long
/// loading transition, drop the oldest rather than let this grow unbounded.
constexpr size_t kMaxDeferredEvents = 32;

/// Give up on a deferred command after this long — most transitions clear
/// in well under a second; anything still deferred after 15s more likely
/// means the player quit to the menu than that the transition is ongoing.
constexpr std::chrono::seconds kDeferredEventTimeout{15};

/// Version string shown in logs.
constexpr const char* kModVersion = "1.2.0";

/// TCP listen port.
constexpr uint16_t kListenPort = 43000;

/// Set once in mod_initialize(); every subsequent call into
/// processFrameEvents() is checked against this to guarantee command
/// dispatch never runs off the main thread.
std::thread::id g_mainThreadId{};

/// True once mods::hook::add_pre<HiveShockTick> succeeds. When true,
/// mod_update() skips its own call to processFrameEvents() so the queue is
/// drained exactly once per frame (via the hook) instead of twice.
std::atomic<bool> g_hookInstalled{false};

/// Reentrancy guard: processFrameEvents() must never run concurrently with
/// itself. In practice the hook and mod_update() are both main-thread call
/// sites, but this costs nothing and turns a future regression (e.g. a host
/// SDK change that calls mod_update from a worker thread) into a clean
/// early-return instead of concurrent access to g_effects/m_spawnedEnemies.
std::atomic<bool> g_dispatchBusy{false};

} // anonymous namespace

// =============================================================================
// Hook callback — executes AFTER the original fapGm_Execute.
//
// HookAction return:
//   HOOK_CONTINUE      → continue to call the original function.
//   HOOK_SKIP_ORIGINAL → skip the original (we never want that here).
//
// We use a *pre-hook* that always returns HOOK_CONTINUE so the original game
// loop runs normally; our code executes right before it.  This ensures that
// any actors we spawn are processed by the engine in the same frame's
// fopAcM tick.
// =============================================================================
#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // avoid Windows.h's min()/max() macros shadowing std::min/std::max — see
                 // the matching comment in crash_handler.hpp
#endif
#include <windows.h>

static bool invokeDispatchSEH(hiveshock::EffectDispatcher* dispatcher,
                             const hiveshock::CommandEvent* evt,
                             hiveshock::EffectResult* outResult) {
    __try {
        *outResult = dispatcher->dispatch(*evt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

// =============================================================================
// dispatchOneEvent — SEH/exception-guarded dispatch of a single command.
// Shared by both the fresh-event loop and the deferred-retry loop below so
// the crash-safety wrapping can't drift between the two call sites.
// =============================================================================
static hiveshock::EffectResult dispatchOneEvent(const hiveshock::CommandEvent& evt) {
    hiveshock::EffectResult result{false, "Execution failed"};
#if defined(_MSC_VER)
    if (!invokeDispatchSEH(g_effects.get(), &evt, &result)) {
        HIVESHOCK_TRACE("CRITICAL SEH CAUGHT: cmd='%s' from '%s'",
                       evt.commandId.c_str(), evt.senderName.c_str());
        DUSK_LOG_ERROR("[HiveShock] CRITICAL: Caught Access Violation executing '%s' — Game crash prevented!",
                       evt.commandId.c_str());
        result = {false, "Crash prevented by safety shield"};
    }
#else
    try {
        result = g_effects->dispatch(evt);
    } catch (const std::exception& ex) {
        DUSK_LOG_ERROR("[HiveShock] Exception executing '%s': %s", evt.commandId.c_str(), ex.what());
        result = {false, ex.what()};
    } catch (...) {
        DUSK_LOG_ERROR("[HiveShock] Unknown exception executing '%s'", evt.commandId.c_str());
        result = {false, "Unknown exception"};
    }
#endif
    return result;
}

// =============================================================================
// processFrameEvents — Drains pending network commands and dispatches them.
// Safe to call from either onFapGmExecutePre or mod_update.
// =============================================================================
static void processFrameEvents() {
    // --- Thread-safety guard #1: strictly main-thread only -----------------
    // The command queue itself is mutex-protected (ThreadSafeQueue), but the
    // *effects* it feeds into (m_spawnedEnemies, engine actor calls) are not
    // — they must only ever run on the thread the game engine itself ticks
    // on. Refuse to dispatch if that invariant is ever violated instead of
    // silently racing with the render/physics thread.
    if (std::this_thread::get_id() != g_mainThreadId) {
        DUSK_LOG_ERROR("[HiveShock] CRITICAL: processFrameEvents() called off the main thread — dispatch skipped this call.");
        HIVESHOCK_TRACE("processFrameEvents: thread-safety violation detected (not main thread), aborting drain");
        return;
    }

    // --- Thread-safety guard #2: reentrancy -------------------------------
    // Guarantees dispatch never runs twice concurrently even if the hook and
    // mod_update() were ever both live for the same frame (see g_hookInstalled).
    bool expectedBusy = false;
    if (!g_dispatchBusy.compare_exchange_strong(expectedBusy, true, std::memory_order_acquire)) {
        HIVESHOCK_TRACE("processFrameEvents: re-entrant call skipped (dispatch already in progress)");
        return;
    }
    struct BusyGuard {
        ~BusyGuard() { g_dispatchBusy.store(false, std::memory_order_release); }
    } busyGuard;

    if (!g_netBridge || !g_netBridge->isRunning()) {
        return;
    }

    if (g_effects) {
        g_effects->updateTimers();
    }

    // Only process commands while the player is in an active game session.
    if (dComIfGp_getPlayer(0) == nullptr) {
        return;
    }

    // Drain all pending commands from the network thread's queue.
    g_frameEvents.clear();
    g_netBridge->pollEvents(g_frameEvents);

    // --- Retry anything deferred by a previous frame first -----------------
    // (scene wasn't ready when it was first tried — e.g. sent mid-door-
    // transition; see EffectResult::deferred). Retried ahead of fresh events
    // so relative ordering is preserved: whatever arrived first still gets
    // applied first once things unblock. Runs even if g_frameEvents is empty
    // this frame — that's the whole point, a transition ending shouldn't
    // require a fresh command to arrive before deferred ones get another
    // shot.
    if (!g_deferredEvents.empty()) {
        const auto now = std::chrono::steady_clock::now();
        std::deque<hiveshock::CommandEvent> retrying;
        retrying.swap(g_deferredEvents);

        HIVESHOCK_CHECKPOINT("processFrameEvents::begin_retry_deferred");
        while (!retrying.empty()) {
            hiveshock::CommandEvent evt = std::move(retrying.front());
            retrying.pop_front();

            if (now - evt.timestamp > kDeferredEventTimeout) {
                DUSK_LOG_WARN("[HiveShock] Dropping deferred command '%s' from '%s' — still not ready after %llds",
                              evt.commandId.c_str(), evt.senderName.c_str(),
                              static_cast<long long>(kDeferredEventTimeout.count()));
                continue;
            }

            hiveshock::EffectResult result = dispatchOneEvent(evt);
            if (result.deferred) {
                g_deferredEvents.push_back(std::move(evt)); // still not ready — try again next frame
                continue;
            }

            if (result.success) {
                DUSK_LOG_INFO("[HiveShock]   -> OK (retried): %s", result.message);
            } else {
                DUSK_LOG_WARN("[HiveShock]   -> FAILED (retried): %s", result.message);
            }
        }
        HIVESHOCK_CHECKPOINT("processFrameEvents::end_retry_deferred");
    }

    if (g_frameEvents.empty()) {
        return;
    }

    HIVESHOCK_CHECKPOINT("processFrameEvents::begin_dispatch");

    // Dispatch each fresh event through the effect system.
    for (auto& evt : g_frameEvents) {
        DUSK_LOG_INFO("[HiveShock] Processing command '%s' from '%s' (amount=%u)",
                      evt.commandId.c_str(),
                      evt.senderName.c_str(),
                      evt.amount);

        HIVESHOCK_TRACE("processFrameEvents: about to dispatch cmd='%s'", evt.commandId.c_str());

        hiveshock::EffectResult result = dispatchOneEvent(evt);

        HIVESHOCK_TRACE("processFrameEvents: dispatch returned success=%d deferred=%d",
                        result.success ? 1 : 0, result.deferred ? 1 : 0);

        if (result.deferred) {
            if (g_deferredEvents.size() >= kMaxDeferredEvents) {
                DUSK_LOG_WARN("[HiveShock] Deferred queue full (%zu) — dropping oldest to make room",
                              g_deferredEvents.size());
                g_deferredEvents.pop_front();
            }
            DUSK_LOG_INFO("[HiveShock]   -> DEFERRED (will retry): %s", result.message);
            g_deferredEvents.push_back(std::move(evt));
        } else if (result.success) {
            DUSK_LOG_INFO("[HiveShock]   -> OK: %s", result.message);
        } else {
            DUSK_LOG_WARN("[HiveShock]   -> FAILED: %s", result.message);
        }
    }

    HIVESHOCK_CHECKPOINT("processFrameEvents::end_dispatch");
}

static HookAction onFapGmExecutePre(
    [[maybe_unused]] ModContext* ctx,
    [[maybe_unused]] void*      args,
    [[maybe_unused]] void*      retval,
    [[maybe_unused]] void*      userdata)
{
    processFrameEvents();
    return HOOK_CONTINUE;
}

// =============================================================================
// Post-hook on dMeter2_c::_draw — see the MeterDraw hook comment above for
// why nametags are queued from here instead of onFapGmExecutePre.
// =============================================================================
static void onMeterDrawPost(
    [[maybe_unused]] ModContext* ctx,
    [[maybe_unused]] void*      args,
    [[maybe_unused]] void*      retval,
    [[maybe_unused]] void*      userdata)
{
    if (g_effects) {
        g_effects->submitNametags();
    }
}

// =============================================================================
// mod_initialize — Called once after the host loads the mod.
// =============================================================================
extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* out_error) {
    // ---- Banner ----
    DUSK_LOG_INFO("==============================================");
    DUSK_LOG_INFO("  HiveShock v%s — Interactive Stream Mod", kModVersion);
    DUSK_LOG_INFO("  Dusklight Engine v2.0.0");
    DUSK_LOG_INFO("==============================================");

    // ---- Record the main thread's identity for the dispatch guard ----
    // mod_initialize() is documented by the host SDK to run on the main
    // thread, so this is the one reliable place to capture it.
    g_mainThreadId = std::this_thread::get_id();

    // ---- Initialize crash handler & forensic logger ----
    hiveshock::CrashHandler::instance().init();
    hiveshock::ForensicLog::instance(); // Force init of the forensic log file
    DUSK_LOG_INFO("[HiveShock] Crash handler installed, forensic logger active.");
    HIVESHOCK_CHECKPOINT("mod_initialize::crash_handler_ready");

    // ---- Create effect dispatcher ----
    g_effects = std::make_unique<hiveshock::EffectDispatcher>();

    // ---- Start TCP server ----
    g_netBridge = std::make_unique<hiveshock::NetBridge>(kListenPort);
    if (!g_netBridge->start()) {
        const char* msg = "Failed to start TCP server on port 43000";
        DUSK_LOG_ERROR("[HiveShock] %s", msg);
        return mods::set_error(out_error, MOD_ERROR, msg);
    }

    // NetBridge::start() already logged the concrete bind/LAN addresses.
    DUSK_LOG_INFO("[HiveShock] Connect from HiveShock or external app to send commands.");
    DUSK_LOG_INFO("[HiveShock] Protocol: JSON lines or COMMAND_ID|SENDER|AMOUNT|PAYLOAD");

    // ---- Install main-loop hook ----
    // processFrameEvents() must run exactly once per frame, strictly on the
    // main thread. When the hook installs, it becomes the sole source of
    // that call; mod_update() then intentionally no-ops on this path (see
    // below) so the queue is never drained twice in the same frame.
    ModResult hookResult = mods::hook::add_pre<HiveShockTick>(onFapGmExecutePre);
    if (hookResult == MOD_OK) {
        g_hookInstalled.store(true, std::memory_order_release);
        DUSK_LOG_INFO("[HiveShock] Hook installed on fapGm_Execute — command dispatch driven by the hook.");
    } else {
        DUSK_LOG_WARN("[HiveShock] Hook on fapGm_Execute not installed; falling back to mod_update() for dispatch.");
    }

    // ---- Install donor-nametag hook (non-fatal if it fails) ----
    // Without this hook, everything else still works — spawns just won't
    // show a name above their head.
    ModResult meterHookResult = mods::hook::add_post<MeterDraw>(onMeterDrawPost);
    if (meterHookResult == MOD_OK) {
        DUSK_LOG_INFO("[HiveShock] Hook installed on dMeter2_c::_draw — donor nametags enabled.");
    } else {
        DUSK_LOG_WARN("[HiveShock] Hook on dMeter2_c::_draw not installed; donor nametags disabled.");
    }

    DUSK_LOG_INFO("[HiveShock] Initialisation complete.");
    return MOD_OK;
}

// =============================================================================
// mod_update — Called every game tick on the main thread.
// =============================================================================
MOD_EXPORT ModResult mod_update([[maybe_unused]] ModError* out_error) {
    // Only act as a fallback dispatcher when the fapGm_Execute hook failed to
    // install. When the hook is live, it is the sole call site for
    // processFrameEvents() — calling it here too would double-drain the
    // queue every frame (harmless on its own since the queue empties on the
    // first call, but it's needless work and muddies the "strictly hooked"
    // guarantee this refactor is meant to establish).
    if (!g_hookInstalled.load(std::memory_order_acquire)) {
        processFrameEvents();
    }
    return MOD_OK;
}

// =============================================================================
// mod_shutdown — Called before the mod is unloaded.
//
// Tear-down order (reverse of initialisation):
//   1. Stop the TCP server and join the listener thread.
//   2. Destroy the effect dispatcher.
//   3. Clear any residual events.
//   (Hooks are automatically removed by the host when the mod is unloaded.)
// =============================================================================
MOD_EXPORT ModResult mod_shutdown([[maybe_unused]] ModError* out_error) {
    DUSK_LOG_INFO("[HiveShock] Shutting down...");

    // Explicitly uninstalled (mirrors tiktok_bridge) even though the host
    // also removes hooks automatically on unload — cheap and symmetric with
    // installation above.
    mods::hook::uninstall<MeterDraw>();

    // Stop the network bridge first — this joins the listener thread.
    if (g_netBridge) {
        g_netBridge->stop();
        g_netBridge.reset();
    }

    g_effects.reset();
    g_frameEvents.clear();
    g_frameEvents.shrink_to_fit();
    g_deferredEvents.clear();

    DUSK_LOG_INFO("[HiveShock] Shutdown complete. Goodbye!");
    return MOD_OK;
}

} // extern "C"
