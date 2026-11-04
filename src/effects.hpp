// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// effects.hpp — Effect dispatch system (header)
//
// Translates inbound CommandEvents into in-game actions: spawning entities,
// modifying player stats, transforming Link, and managing game state.
// All public methods run exclusively on the main game thread.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#pragma once

#include "net_bridge.hpp"  // CommandEvent

#include "global.h"        // s16, u16, etc.
#include "SSystem/SComponent/c_xyz.h" // cXyz
#include "f_pc/f_pc_manager.h" // fpc_ProcID
#include "d/d_drawlist.h"   // dDlst_base_c — donor-nametag drawlist (also forward-declares JUTFont)

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <cstring>

class fopAc_ac_c;

namespace hiveshock {

struct SpawnEntry;

// =============================================================================
// NameTagDrawlist — draws each tracked spawn's donor name above its head.
//
// Ported from tiktok_bridge (dusklight_1.4.1). draw() is implemented
// out-of-line in effects.cpp (it needs the full JUTFont.h, which we don't
// want to drag into every TU that includes this header — d_drawlist.h above
// only forward-declares JUTFont, which is enough here).
//
// Populated by EffectDispatcher::submitNametags(), which must be called from
// a post-hook on dMeter2_c::_draw (see mod.cpp), NOT from the regular
// per-frame command-dispatch hook — the 2D drawlist is reset every frame,
// and meter-draw runs right after that reset, which is the only point where
// entries queued here actually survive to the frame's Painter pass.
// =============================================================================
class NameTagDrawlist : public dDlst_base_c {
public:
    static constexpr int kMaxEntries = 128; // matches EffectDispatcher::kMaxTrackedActors

    struct Entry {
        char  text[24]{};
        float x = 0.0f;
        float y = 0.0f;
    };

    Entry entries[kMaxEntries]{};
    int   count = 0;

    void draw() override;
};

struct EffectResult {
    bool success{false};
    char message[256]{};

    /// Set by EffectDispatcher::dispatch() itself (not by handlers) when the
    /// command was rejected purely because the scene wasn't ready / the
    /// player couldn't act yet — e.g. sent mid-door-transition. Distinct from
    /// a genuine failure (unknown command, actor cap, etc.): the caller
    /// (mod.cpp) should requeue and retry a deferred command on a later
    /// frame instead of dropping it. Without this, a command that happens to
    /// land during a transition is silently lost forever, even though the
    /// transition clears itself a moment later.
    bool deferred{false};

    EffectResult() = default;
    EffectResult(bool s, const char* msg) : success(s) {
        if (msg != nullptr) {
            std::strncpy(message, msg, sizeof(message) - 1);
            message[sizeof(message) - 1] = '\0';
        }
    }
    EffectResult(bool s, const std::string& msg) : EffectResult(s, msg.c_str()) {}
};

class EffectDispatcher {
public:
    EffectDispatcher();

    /// Dispatch a single command event on the main thread.
    EffectResult dispatch(const CommandEvent& event);

    /// Spawns an actor near the player. `senderName` (the viewer/donor who
    /// triggered it) is tracked alongside the actor so its nametag can be
    /// drawn above its head — see submitNametags().
    EffectResult spawnActor(const SpawnEntry& entry, uint32_t customParams = 0, float customOffsetY = 0.0f,
                             const std::string& senderName = {});

    /// Per-frame bookkeeping for active timed effects.
    void updateTimers();

    /// Draws the donor-name tag above every currently tracked, alive spawn.
    /// Call this from a post-hook on dMeter2_c::_draw (see mod.cpp) — see the
    /// NameTagDrawlist comment for why it can't run from the usual per-frame
    /// command-dispatch hook.
    void submitNametags();

private:
    // --- Handlers: Spawns & Armies ---
    EffectResult handleSpawnEntity(const CommandEvent& event);
    EffectResult handleRandomEnemy(const CommandEvent& event);
    EffectResult handleArmy(const CommandEvent& event);
    EffectResult handleBeeSwarm(const CommandEvent& event);
    EffectResult handleClearEnemies(const CommandEvent& event);
    EffectResult handleStatus(const CommandEvent& event);
    EffectResult handleGreatFairy(const CommandEvent& event);

    // --- Handlers: Player Stats & Combat ---
    EffectResult handleHeal(const CommandEvent& event);
    EffectResult handleDamage(const CommandEvent& event);
    EffectResult handleKill(const CommandEvent& event);
    EffectResult handleRupees(const CommandEvent& event);
    EffectResult handleStealRupees(const CommandEvent& event);
    EffectResult handleFillRupees(const CommandEvent& event);
    EffectResult handleEmptyOil(const CommandEvent& event);
    EffectResult handleFillOil(const CommandEvent& event);
    EffectResult handleKnockback(const CommandEvent& event);
    EffectResult handleExplode(const CommandEvent& event);

    // --- Handlers: Transformation & Equipment ---
    EffectResult handleWolfToggle(const CommandEvent& event);
    EffectResult handleForceWolf(const CommandEvent& event);
    EffectResult handleForceHuman(const CommandEvent& event);
    EffectResult handleIronBootsToggle(const CommandEvent& event);
    EffectResult handleIronBootsOn(const CommandEvent& event);
    EffectResult handleIronBootsOff(const CommandEvent& event);
    EffectResult handleSummonEpona(const CommandEvent& event);
    EffectResult handleDismountHorse(const CommandEvent& event);

    // --- Handlers: Game State & Save ---
    EffectResult handleReset(const CommandEvent& event);
    EffectResult handleDeleteSave(const CommandEvent& event);

    // --- Utility helpers ---
    bool validateSpawnConditions() const;
    cXyz getSpawnPosition(float offsetY = 0.0f) const;

    /// Sets a freshly spawned actor's initial action state to immediately
    /// chase/attack Link instead of its default idle/wait behaviour, per
    /// actor type. Ported from tiktok_bridge's ForceAggro(). No-op (actor
    /// just faces Link) for any procName not covered — safer than guessing.
    /// Must only be called once the actor has finished its create() phase —
    /// see tickTrackedSpawns().
    void forceAggro(fopAc_ac_c* actor) const;

    /// Per-frame pass over m_spawnedEnemies: applies forceAggro() once each
    /// actor finishes creating, keeps Puppet Zelda tame, and prunes actors
    /// that died/despawned. Runs from updateTimers(), i.e. before the
    /// engine's own actor execute for the frame.
    void tickTrackedSpawns();

    /// Deletes a tracked spawn. A Gibdo mid-scream is first told to finish it
    /// (mCryTimer = 1) and deleted a few frames later: its file-static m_cry_gi
    /// is only cleared by its own execute, and a dangling one stops every later
    /// Gibdo from attacking. (Replaces the old engine-side fix in _delete().)
    void safeDelete(fpc_ProcID id);
    struct DyingGibdo { fpc_ProcID id; u8 framesLeft; };
    std::vector<DyingGibdo> m_dyingGibdos;
    void tickDyingGibdos();

    /// Logs free heap sizes (diagnostics for silent create() failures).
    void logHeapState(const char* tag) const;

    struct TrackedSpawn;
    /// Keeps Puppet Zelda from running her Palace-of-Twilight boss script:
    /// undoes create()'s one-shot save/message side effects, keeps her
    /// health from ever reaching 0 (which is what starts the defeat
    /// cutscene + dStage_changeScene), and kills her like a normal enemy on
    /// the third hit. Returns true if she was killed this frame.
    bool tamePuppetZelda(TrackedSpawn& tracked, fopAc_ac_c* actor);

    // --- Extended scene-safety gating ---
    // Ported from tiktok_bridge (dusklight_1.4.1), a previous mod that spawned
    // actors reliably over long sessions. validateSpawnConditions() above only
    // checked "is there a player and a stage" — this adds the checks that mod
    // actually needed to avoid corrupting engine state: no spawns during pause,
    // cutscenes, scene transitions, game-over, or in the ~0.8s right after a
    // scene becomes current (systems like the room/layer manager are still
    // settling then). isSceneReady() tracks that settle time via
    // m_stableFrames, ticked once per frame from updateTimers().
    bool isPlaySceneAlive() const;
    bool isHardTransition() const;
    bool isLinkProcUnsafe() const;
    bool isPlayerUnableToFight() const;
    bool isSceneReady() const;
    void tickSceneSafety();

    /// Writes a short description of whichever specific condition is
    /// currently making isPlayerUnableToFight()/isHardTransition() block
    /// effects into `out` (empty string if nothing is blocking). Exists only
    /// for tickSceneSafety()'s stuck-state diagnostic logging — duplicates
    /// the same checks as isHardTransition()/isLinkProcUnsafe() read-only,
    /// no behavior change, just visibility into WHY when it's stuck.
    void describeBlockReason(char* out, size_t outSize) const;

    static constexpr uint32_t kMinStableFrames = 48; // ~0.8s at 60fps before spawns unlock
    uint32_t m_stableFrames{0};
    uint32_t m_frameCounter{0}; // ticked once per updateTimers(); diagnostics only
    char     m_lastLoggedBlockReason[80]{}; // change-detection only, for the trace log
    bool     m_wasInHardTransition{false};

    // --- Pending multi-spawn queue (armies/swarms) ---
    // Every fopAcM_create() we've confirmed via hiveshock_forensic.log
    // succeeds on its own; crashes only show up shortly after a burst of
    // several *identical* actors created within the same frame (e.g.
    // bokoblin_army/goat_army), never after a lone spawn. Draining this
    // queue at one actor per frame (from updateTimers()) keeps each
    // fopAcM_create() call, and the new actor's first few create()-phase
    // ticks, isolated to its own frame instead of piling five of them into
    // one — this is the cheapest mod-side mitigation for that pattern and
    // doesn't touch engine/port code.
    struct PendingSpawn {
        const SpawnEntry* entry;
        std::string       label;      // e.g. "Bokoblin army"
        std::string       senderName;
    };
    std::deque<PendingSpawn> m_pendingSpawns;
    void queueSpawns(const SpawnEntry& entry, uint32_t count, const std::string& label,
                      const std::string& senderName);

    // --- Tracked state ---
    static constexpr size_t kMaxTrackedActors = 128; // shared cap: spawn tracking + nametag drawlist size

    struct TrackedSpawn {
        fpc_ProcID  id;
        std::string donor; // truncated/sanitized viewer name, or empty for non-donor spawns
        s16         procName{0};
        bool        aggroApplied{false};
        u32         creatingFrames{0}; // frames spent in fpcM_IsCreating; stuck creates get culled
        u32         spawnFrame{0};     // m_frameCounter when fopAcM_create() returned (diagnostics)
        bool        existedOnce{false}; // true once create() finished and the actor was found; false at removal = create() failed

        // Puppet Zelda only — see tamePuppetZelda().
        bool        zeldaInit{false};
        bool        zeldaDunSwitch2WasSet{false}; // dungeon switch 2 state before her create() set it
        u8          zeldaHits{0};
    };
    static std::string truncateDonorName(const std::string& user);

    std::vector<TrackedSpawn> m_spawnedEnemies;
    char                      m_trackedStage[8]{}; // stage/room the tracked spawns belong to
    int                       m_trackedRoom{-1};
    void                      clearTrackedSpawns();
    NameTagDrawlist          m_nameTagDraw;
    bool                      m_puppetZeldaBgmActive{false}; // her create() started boss BGM; restore scene BGM once none are left

    // --- Dispatch table ---
    using HandlerFn = EffectResult (EffectDispatcher::*)(const CommandEvent&);
    static const std::unordered_map<std::string, HandlerFn>& getDispatchTable();
};

} // namespace hiveshock
