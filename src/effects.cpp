// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// effects.cpp — Effect dispatch system (implementation)
//
// Every handler runs exclusively on the main thread and performs
// strict pointer/state validation before touching game memory.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#include "effects.hpp"
#include "spawn_table.hpp"
#include "logging.hpp"

// --- Dusklight engine headers ---
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_alink.h"      // real daAlink_c — mProcID / checkEventRun / etc. for scene-safety gating
#include "f_pc/f_pc_layer.h"        // fpcLy_CurrentLayer / fpcLy_SetCurrentLayer — for PlayLayerGuard
#include "f_pc/f_pc_node.h"         // process_node_class
#include "f_pc/f_pc_name.h"         // fpcNm_PLAY_SCENE_e
#include "m_Do/m_Do_Reset.h"        // mDoRst::onReset / onReturnToMenu / isReset
#include "m_Do/m_Do_graphic.h"      // mDoGph_gInf_c::getFader
#include "m_Do/m_Do_MemCard.h"      // g_mDoMemCd_control, mDoMemCd_save, SAVEFILE_SIZE
#include "m_Do/m_Do_MemCardRWmng.h" // mDoMemCdRWm_SetCheckSumGameData
#include "m_Do/m_Do_lib.h"          // mDoLib_pos2camera / mDoLib_project — donor nametag projection
#include "JSystem/JKernel/JKRExpHeap.h" // heap free-size diagnostics
#include "m_Do/m_Do_ext.h"          // mDoExt_getGameHeap / getZeldaHeap
#include "JSystem/JUtility/JUTFont.h" // full JUTFont (d_drawlist.h only forward-declares it)

// --- Per-enemy "force aggro" headers (immediate-attack-on-spawn) ---
// Field names/offsets below are verbatim from tiktok_bridge (dusklight_1.4.1)
// and confirmed byte-for-byte identical in this engine version before use
// (e_ba_class::mAction @0x672, e_dn_class::action @0x6CE, e_st_class::mAction
// @0x67E, e_mm_class::action @0x66E, e_sm2_class::action @0x682,
// e_yk_class::mAction @0x66E, e_yg_class::mAction @0x67E, e_s1_class::mAction
// @0x696, ni_class::mAction @0x5FA — all checked against these headers).
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_e_ba.h"
#include "d/actor/d_a_e_ge.h"
#include "d/actor/d_a_e_dn.h"
#include "d/actor/d_a_e_ww.h"
#include "d/actor/d_a_e_tt.h"
#include "d/actor/d_a_e_st.h"
#include "d/actor/d_a_e_mm.h"
#include "d/actor/d_a_e_sm2.h"
#include "d/actor/d_a_e_gi.h"
#include "d/actor/d_a_e_hp.h"
#include "d/actor/d_a_e_yk.h"
#include "d/actor/d_a_e_ym.h"
#include "d/actor/d_a_e_yg.h"
#include "d/actor/d_a_e_s1.h"
#include "d/actor/d_a_ni.h"
#include "d/actor/d_a_cow.h"
#include "d/actor/d_a_e_hzelda.h"  // Puppet Zelda — see tamePuppetZelda()
#include "m_Do/m_Do_audio.h"       // mDoAud_sceneBgmStart

// --- Crash handler (MUST come after engine headers to avoid Windows.h macro collisions) ---
#include "crash_handler.hpp"

// --- Standard library ---
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace hiveshock {

// =============================================================================
// Static dispatch table
// =============================================================================
const std::unordered_map<std::string, EffectDispatcher::HandlerFn>&
EffectDispatcher::getDispatchTable() {
    static const std::unordered_map<std::string, HandlerFn> table = {
        // --- Spawns & Collectibles ---
        {"spawn_bomb",        &EffectDispatcher::handleExplode},
        {"spawn_bombchu",     &EffectDispatcher::handleExplode},
        {"spawn_rupee",       &EffectDispatcher::handleSpawnEntity},
        {"give_rupee",        &EffectDispatcher::handleSpawnEntity},
        {"spawn_heart",       &EffectDispatcher::handleSpawnEntity},
        {"spawn_fairy",       &EffectDispatcher::handleSpawnEntity},
        {"spawn_pot",         &EffectDispatcher::handleSpawnEntity},
        {"great_fairy",       &EffectDispatcher::handleGreatFairy},
        {"recovery_fairy",    &EffectDispatcher::handleSpawnEntity},

        // --- Enemies & Creatures ---
        {"cucco",             &EffectDispatcher::handleSpawnEntity},
        {"goat",              &EffectDispatcher::handleSpawnEntity},
        {"keese",             &EffectDispatcher::handleSpawnEntity},
        {"fire_keese",        &EffectDispatcher::handleSpawnEntity},
        {"ice_keese",         &EffectDispatcher::handleSpawnEntity},
        {"guay",              &EffectDispatcher::handleSpawnEntity},
        {"bokoblin",          &EffectDispatcher::handleSpawnEntity},
        {"bulblin",           &EffectDispatcher::handleSpawnEntity},
        {"lizalfos",          &EffectDispatcher::handleSpawnEntity},
        {"chuchu",            &EffectDispatcher::handleSpawnEntity},
        {"tektite",           &EffectDispatcher::handleSpawnEntity},
        {"skulltula",         &EffectDispatcher::handleSpawnEntity},
        {"helmasaur",         &EffectDispatcher::handleSpawnEntity},
        {"helmsaurus",        &EffectDispatcher::handleSpawnEntity},
        {"wolfos",            &EffectDispatcher::handleSpawnEntity},
        {"poe",               &EffectDispatcher::handleSpawnEntity},
        {"espectro",          &EffectDispatcher::handleSpawnEntity},
        {"gibdo",             &EffectDispatcher::handleSpawnEntity},
        {"keese_twilight",    &EffectDispatcher::handleSpawnEntity},
        {"shadow_beast",      &EffectDispatcher::handleSpawnEntity},
        {"twilight_insect",   &EffectDispatcher::handleSpawnEntity},
        {"twilight_vermin",   &EffectDispatcher::handleSpawnEntity},
        {"poison_mite",       &EffectDispatcher::handleSpawnEntity},
        {"bee",               &EffectDispatcher::handleSpawnEntity},
        {"darknut",           &EffectDispatcher::handleSpawnEntity},
        {"puppet_zelda",      &EffectDispatcher::handleSpawnEntity},
        {"freezard",          &EffectDispatcher::handleSpawnEntity},
        {"leever",            &EffectDispatcher::handleSpawnEntity},

        // --- Legacy aliases ---
        {"spawn_keese",       &EffectDispatcher::handleSpawnEntity},
        {"spawn_bokoblin",    &EffectDispatcher::handleSpawnEntity},
        {"spawn_bulblin",     &EffectDispatcher::handleSpawnEntity},
        {"spawn_lizalfos",    &EffectDispatcher::handleSpawnEntity},
        {"spawn_skulltula",   &EffectDispatcher::handleSpawnEntity},

        // --- Multi-spawns & Random ---
        {"random_enemy",      &EffectDispatcher::handleRandomEnemy},
        {"cucco_army",        &EffectDispatcher::handleArmy},
        {"bokoblin_army",     &EffectDispatcher::handleArmy},
        {"goat_army",         &EffectDispatcher::handleArmy},
        {"bee_swarm",         &EffectDispatcher::handleBeeSwarm},
        {"clear_enemies",     &EffectDispatcher::handleClearEnemies},
        {"hs_status",         &EffectDispatcher::handleStatus},

        // --- Player Stats & Combat ---
        {"heal",              &EffectDispatcher::handleHeal},
        {"damage",            &EffectDispatcher::handleDamage},
        {"kill",              &EffectDispatcher::handleKill},
        {"instakill",         &EffectDispatcher::handleKill},
        {"rupees",            &EffectDispatcher::handleRupees},
        {"give_rupees",       &EffectDispatcher::handleRupees},
        {"steal_rupees",      &EffectDispatcher::handleStealRupees},
        {"fill_rupees",       &EffectDispatcher::handleFillRupees},
        {"magic",             &EffectDispatcher::handleFillOil},
        {"empty_oil",         &EffectDispatcher::handleEmptyOil},
        {"fill_oil",          &EffectDispatcher::handleFillOil},
        {"knockback",         &EffectDispatcher::handleKnockback},
        {"explode",           &EffectDispatcher::handleExplode},
        {"bomb_explode",      &EffectDispatcher::handleExplode},

        // --- Transformation & Equipment ---
        {"wolf_toggle",       &EffectDispatcher::handleWolfToggle},
        {"force_wolf",        &EffectDispatcher::handleForceWolf},
        {"force_human",       &EffectDispatcher::handleForceHuman},
        {"iron_boots_toggle", &EffectDispatcher::handleIronBootsToggle},
        {"iron_boots_on",     &EffectDispatcher::handleIronBootsOn},
        {"iron_boots_off",    &EffectDispatcher::handleIronBootsOff},
        {"summon_epona",      &EffectDispatcher::handleSummonEpona},
        {"dismount_horse",    &EffectDispatcher::handleDismountHorse},

        // --- Game State & Save ---
        {"reset",             &EffectDispatcher::handleReset},
        {"delete_save",       &EffectDispatcher::handleDeleteSave},
        {"wipe_save",         &EffectDispatcher::handleDeleteSave},
        {"nuke_save",         &EffectDispatcher::handleDeleteSave},
    };
    return table;
}

// =============================================================================
// NameTagDrawlist::draw — ported from tiktok_bridge (dusklight_1.4.1). Drop
// shadow + gold text, matching that mod's on-screen donor tags.
// =============================================================================
void NameTagDrawlist::draw() {
    JUTFont* font = mDoExt_getMesgFont();
    if (font == nullptr || count <= 0) {
        return;
    }

    constexpr float kNameTagCharScale = 0.62f;
    const float charW = static_cast<float>(font->getWidth()) * kNameTagCharScale;
    const float charH = static_cast<float>(font->getHeight()) * kNameTagCharScale;
    font->setGX();

    for (int i = 0; i < count; ++i) {
        const Entry& entry = entries[i];
        const u32 len = static_cast<u32>(std::strlen(entry.text));
        if (len == 0) {
            continue;
        }
        const float textW = charW * static_cast<float>(len);
        const float x = entry.x - (textW * 0.5f);
        const float y = entry.y - charH;

        // Drop shadow first, then the actual gold text on top.
        font->setCharColor(JUtility::TColor(0, 0, 0, 210));
        font->drawString_size_scale(x + 1.0f, y + 1.0f, charW, charH, entry.text, len, true);
        font->setCharColor(JUtility::TColor(255, 230, 90, 255));
        font->drawString_size_scale(x, y, charW, charH, entry.text, len, true);
    }
}

// =============================================================================
// Constructor
// =============================================================================
EffectDispatcher::EffectDispatcher() = default;

// =============================================================================
// dispatch
// =============================================================================
EffectResult EffectDispatcher::dispatch(const CommandEvent& event) {
    HIVESHOCK_RECORD_CMD(event.commandId.c_str(), event.senderName.c_str());
    HIVESHOCK_TRACE("dispatch: cmd='%s', sender='%s', amount=%u",
                    event.commandId.c_str(), event.senderName.c_str(), event.amount);

    const auto& table = getDispatchTable();
    auto it = table.find(event.commandId);

    if (it == table.end()) {
        std::string msg = "Unknown command: " + event.commandId;
        DUSK_LOG_WARN("[HiveShock] %s", msg.c_str());
        return {false, std::move(msg)};
    }

    // Centralized scene-readiness gate. Every handler already re-checks this
    // itself via validateSpawnConditions() (left as-is, harmless defense in
    // depth) — but checking it HERE first lets us tell "genuinely rejected"
    // apart from "just landed at a bad moment, e.g. mid-door-transition" and
    // signal the caller (mod.cpp's processFrameEvents) to retry on a later
    // frame instead of silently dropping it. Without this, a command sent
    // during any transition is lost forever even though the transition
    // clears itself a moment later — confirmed via hiveshock_forensic.log:
    // the gating itself always self-recovers, the command just never got a
    // second chance to run once it did.
    //
    // Exempted: delete_save/wipe_save/nuke_save, same as tiktok_bridge
    // exempted them — a forced reset should still go through even from a
    // stuck state, not queue up waiting for "ready".
    const bool wipeSave = event.commandId == "delete_save" || event.commandId == "wipe_save" ||
                          event.commandId == "nuke_save";
    if (!wipeSave && (!isSceneReady() || isPlayerUnableToFight())) {
        HIVESHOCK_TRACE("dispatch: deferring '%s' — scene not ready / player unable to act",
                        event.commandId.c_str());
        EffectResult deferredResult{false, "Deferred: scene not ready"};
        deferredResult.deferred = true;
        return deferredResult;
    }

    HIVESHOCK_CHECKPOINT("dispatch::calling_handler");
    auto result = (this->*(it->second))(event);
    HIVESHOCK_CHECKPOINT("dispatch::handler_returned");
    return result;
}

// =============================================================================
// updateTimers
// =============================================================================
void EffectDispatcher::updateTimers() {
    ++m_frameCounter;
    tickSceneSafety();

    // Drain at most one queued army/swarm spawn per frame — see the comment
    // on m_pendingSpawns in effects.hpp for why this is throttled instead of
    // looped inline in the handler. spawnActor() already logs each spawn's
    // own success/failure, so nothing further to report here per-item.
    if (!m_pendingSpawns.empty()) {
        PendingSpawn next = std::move(m_pendingSpawns.front());
        m_pendingSpawns.pop_front();
        HIVESHOCK_TRACE("updateTimers: draining queued spawn for '%s' (%zu left), requested by %s",
                        next.label.c_str(), m_pendingSpawns.size(), next.senderName.c_str());
        spawnActor(*next.entry, 0, 0.0f, next.senderName);
    }

    tickDyingGibdos();
    tickTrackedSpawns();
}

// =============================================================================
// tickTrackedSpawns — see effects.hpp. fopAcM_create() only *queues* the
// actor: until its create() phase finishes (fpcM_IsCreating), SearchByID
// returns null, and create() itself resets the action state anyway. So the
// aggro override can't be applied from spawnActor() — it used to be, and was
// silently a no-op for every actor. It's applied here instead, on the first
// frame the actor actually exists.
// =============================================================================
void EffectDispatcher::tickTrackedSpawns() {
    auto* player = dComIfGp_getPlayer(0);
    if (player == nullptr || isHardTransition()) {
        return;
    }

    // Tracked spawns belong to the stage/room they were summoned in: drop them
    // when Link moves to another one, so they don't pile up against the cap.
    const char* stage = dComIfGp_getStartStageName();
    const int   room  = fopAcM_GetRoomNo(player);
    if (stage != nullptr && (std::strncmp(stage, m_trackedStage, sizeof(m_trackedStage) - 1) != 0 ||
                             room != m_trackedRoom)) {
        if (!m_spawnedEnemies.empty() || !m_pendingSpawns.empty()) {
            DUSK_LOG_INFO("[HiveShock] Stage/room changed (%s/%d -> %s/%d) — clearing %zu tracked spawns, %zu queued",
                          m_trackedStage, m_trackedRoom, stage, room, m_spawnedEnemies.size(),
                          m_pendingSpawns.size());
            clearTrackedSpawns();
            m_pendingSpawns.clear(); // queued army members belonged to the previous place
        }
        std::strncpy(m_trackedStage, stage, sizeof(m_trackedStage) - 1);
        m_trackedStage[sizeof(m_trackedStage) - 1] = '\0';
        m_trackedRoom = room;
    }

    if (m_spawnedEnemies.empty() && !m_puppetZeldaBgmActive) {
        return;
    }

    constexpr u32 kMaxCreatingFrames = 600; // ~10 s at 60 fps
    size_t zeldasAlive = 0;
    for (auto it = m_spawnedEnemies.begin(); it != m_spawnedEnemies.end();) {
        if (fpcM_IsCreating(it->id)) {
            if (++it->creatingFrames > kMaxCreatingFrames) {
                DUSK_LOG_WARN("[HiveShock] Spawn 0x%08X stuck in create() — dropping it", it->id);
                fopAcM_delete(it->id);
                it = m_spawnedEnemies.erase(it);
                continue;
            }
            if (it->procName == fpcNm_E_HZELDA_e) {
                zeldasAlive++;
            }
            ++it;
            continue;
        }

        fopAc_ac_c* actor = fopAcM_SearchByID(it->id);
        if (actor == nullptr) {
            // Distinguish "create() failed" (never existed) from a normal death/despawn.
            if (it->existedOnce) {
                DUSK_LOG_INFO("[HiveShock] Spawn 0x%08X (proc 0x%04X) gone after %u frames (died/despawned) at %s/%d",
                              it->id, it->procName, m_frameCounter - it->spawnFrame, m_trackedStage, m_trackedRoom);
            } else {
                DUSK_LOG_WARN("[HiveShock] Spawn 0x%08X (proc 0x%04X) NEVER EXISTED — create() failed after %u frames (%u creating) at %s/%d",
                              it->id, it->procName, m_frameCounter - it->spawnFrame, it->creatingFrames,
                              m_trackedStage, m_trackedRoom);
                logHeapState("create() failure");
            }
            it = m_spawnedEnemies.erase(it);
            continue;
        }
        it->existedOnce = true;

        if (!it->aggroApplied) {
            it->aggroApplied = true;
            forceAggro(actor);
        }

        if (it->procName == fpcNm_E_HZELDA_e) {
            if (tamePuppetZelda(*it, actor)) {
                it = m_spawnedEnemies.erase(it);
                continue;
            }
            zeldasAlive++;
        }
        ++it;
    }

    // Her create() starts the Ganon battle theme (e_hzelda_wait, mode -1) and
    // her native defeat path is the only thing that would ever stop it. Once
    // no tracked Puppet Zelda is left (killed, cleared, or despawned), put the
    // stage's own BGM back.
    if (zeldasAlive > 0) {
        m_puppetZeldaBgmActive = true;
    } else if (m_puppetZeldaBgmActive) {
        m_puppetZeldaBgmActive = false;
        dComIfGs_offOneZoneSwitch(12, -1); // her triangle attack toggles this
        // No bgmStop first: sceneBgmStart() is a no-op if the stage theme is
        // already playing (e.g. after a room change), and when it isn't, the
        // bgmStart() it ends in already stops the Ganon theme itself.
        mDoAud_sceneBgmStart();
        DUSK_LOG_INFO("[HiveShock] Puppet Zelda gone — restored scene BGM");
    }
}

void EffectDispatcher::safeDelete(fpc_ProcID id) {
    if (!fpcM_IsCreating(id)) {
        fopAc_ac_c* actor = fopAcM_SearchByID(id);
        if (actor != nullptr && fopAcM_GetName(actor) == fpcNm_E_GI_e) {
            auto* gibdo = static_cast<daE_GI_c*>(actor);
            if (gibdo->mCryTimer != 0) {
                gibdo->mCryTimer = 1; // its next execute clears m_cry_gi
                m_dyingGibdos.push_back({id, 3});
                return;
            }
        }
    }
    fopAcM_delete(id);
}

void EffectDispatcher::tickDyingGibdos() {
    for (auto it = m_dyingGibdos.begin(); it != m_dyingGibdos.end();) {
        if (it->framesLeft > 0) {
            --it->framesLeft;
            ++it;
            continue;
        }
        if (fpcM_IsCreating(it->id) || fopAcM_SearchByID(it->id) != nullptr) {
            fopAcM_delete(it->id);
        }
        it = m_dyingGibdos.erase(it);
    }
}

void EffectDispatcher::logHeapState(const char* tag) const {
    JKRExpHeap* game = mDoExt_getGameHeap();
    JKRExpHeap* zelda = mDoExt_getZeldaHeap();
    DUSK_LOG_INFO("[HiveShock] Heap (%s): game free=%d total-free=%d | zelda free=%d total-free=%d",
                  tag,
                  game ? static_cast<int>(game->getFreeSize()) : -1,
                  game ? static_cast<int>(game->getTotalFreeSize()) : -1,
                  zelda ? static_cast<int>(zelda->getFreeSize()) : -1,
                  zelda ? static_cast<int>(zelda->getTotalFreeSize()) : -1);
}

EffectResult EffectDispatcher::handleStatus(const CommandEvent& event) {
    const char* stage = dComIfGp_getStartStageName();
    DUSK_LOG_INFO("[HiveShock] STATUS stage=%s room=%d tracked=%zu pending=%zu stableFrames=%u frame=%u (by %s)",
                  stage ? stage : "(null)", m_trackedRoom, m_spawnedEnemies.size(), m_pendingSpawns.size(),
                  m_stableFrames, m_frameCounter, event.senderName.c_str());
    for (const auto& t : m_spawnedEnemies) {
        DUSK_LOG_INFO("[HiveShock]   tracked 0x%08X proc=0x%04X creating=%d existed=%d age=%u", t.id, t.procName,
                      fpcM_IsCreating(t.id) ? 1 : 0, t.existedOnce ? 1 : 0, m_frameCounter - t.spawnFrame);
    }
    logHeapState("status");
    return {true, "Status logged"};
}

void EffectDispatcher::clearTrackedSpawns() {
    for (const auto& tracked : m_spawnedEnemies) {
        if (fpcM_IsCreating(tracked.id) || fopAcM_SearchByID(tracked.id) != nullptr) {
            safeDelete(tracked.id);
        }
    }
    m_spawnedEnemies.clear();
}

// =============================================================================
// tamePuppetZelda — see effects.hpp. Every problem here comes from her being
// written as a one-off scripted boss (d_a_e_hzelda.cpp):
//   * create() sets dungeon save switch 2 and a 30-frame timer that pops a
//     Midna message (0x490) the first time she's ever spawned;
//   * mMissedBallCount starts at 1, and after 5 un-returned balls turns on
//     zone switch 3 (Midna's "hit it back" tutorial);
//   * when a returned ball drops health to 0 she sets mDemoMode=1, which
//     orders an event, grabs the camera, teleports her to (0,500,-2000),
//     fades to black and calls dStage_changeScene() — the "weird scenes".
// Health is only ever reduced in one place (the ball-return hit, -1 per hit)
// and this runs before her execute each frame, so resetting health to 3 here
// guarantees that path can never reach 0.
// =============================================================================
bool EffectDispatcher::tamePuppetZelda(TrackedSpawn& tracked, fopAc_ac_c* actor) {
    constexpr s16 kZeldaHealth = 3;
    constexpr u8  kZeldaHitsToKill = 3;

    auto* zelda = static_cast<e_hzelda_class*>(actor);

    if (!tracked.zeldaInit) {
        tracked.zeldaInit = true;
        zelda->mMsgSetTimer = 0;
        zelda->mMissedBallCount = 0;
        if (!tracked.zeldaDunSwitch2WasSet) {
            dComIfGs_offSaveDunSwitch(2);
        }
    }

    // Safety net: should be unreachable given the health clamp below, but if
    // the defeat demo ever did start, cancel it before demo_camera() acts on
    // it (it only runs from her own execute, which comes after this).
    if (zelda->mDemoMode != 0) {
        zelda->mDemoMode = 0;
        tracked.zeldaHits = kZeldaHitsToKill;
    }

    if (actor->health < kZeldaHealth) {
        const int lost = kZeldaHealth - std::max<int>(actor->health, 0);
        tracked.zeldaHits = static_cast<u8>(std::min<int>(tracked.zeldaHits + lost, kZeldaHitsToKill));
        actor->health = kZeldaHealth;
    }

    if (tracked.zeldaHits < kZeldaHitsToKill) {
        return false;
    }

    fopAcM_createDisappear(actor, &actor->current.pos, 10, 0, 0);
    fopAcM_delete(actor);
    DUSK_LOG_INFO("[HiveShock] Puppet Zelda defeated — removed as a normal enemy (no boss cutscene)");
    return true;
}

void EffectDispatcher::queueSpawns(const SpawnEntry& entry, uint32_t count, const std::string& label,
                                    const std::string& senderName) {
    for (uint32_t i = 0; i < count; ++i) {
        m_pendingSpawns.push_back({&entry, label, senderName});
    }
    DUSK_LOG_INFO("[HiveShock] Queued %u x %s (spawning 1 per frame) — requested by %s",
                  count, label.c_str(), senderName.c_str());
}

// =============================================================================
// truncateDonorName — sanitize a viewer name for on-screen display: strip to
// printable ASCII and cap the length so a hostile/weird username can't
// overflow the fixed-size NameTagDrawlist::Entry::text buffer. Ported from
// tiktok_bridge's TruncateDonorName().
// =============================================================================
std::string EffectDispatcher::truncateDonorName(const std::string& user) {
    constexpr size_t kMaxNameTagChars = 18; // Entry::text is char[24]; leaves room for the terminator
    std::string out;
    out.reserve(std::min<size_t>(user.size(), kMaxNameTagChars));
    for (unsigned char c : user) {
        if (out.size() >= kMaxNameTagChars) {
            break;
        }
        if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// =============================================================================
// submitNametags — projects each tracked, alive spawn's world position to
// screen space and queues a donor-name entry to draw there. Ported from
// tiktok_bridge's SubmitNametags(). Must run from the dMeter2_c::_draw
// post-hook — see the NameTagDrawlist comment in effects.hpp for why.
// =============================================================================
void EffectDispatcher::submitNametags() {
    m_nameTagDraw.count = 0;

    if (isHardTransition() || !isPlaySceneAlive() || m_spawnedEnemies.empty() ||
        dComIfGd_getView() == nullptr || dComIfGp_getPlayer(0) == nullptr) {
        return;
    }

    const float screenW = mDoGph_gInf_c::getWidthF();
    const float screenH = mDoGph_gInf_c::getHeightF();

    for (const TrackedSpawn& tracked : m_spawnedEnemies) {
        if (tracked.donor.empty() || m_nameTagDraw.count >= NameTagDrawlist::kMaxEntries) {
            continue;
        }
        if (fpcM_IsCreating(tracked.id)) {
            continue; // still mid-create()-phase; no valid transform yet
        }
        fopAc_ac_c* actor = fopAcM_SearchByID(tracked.id);
        if (actor == nullptr) {
            continue; // died / despawned since spawning
        }

        // Prefer attention/eye height; fall back to a fixed offset above feet.
        cXyz world = actor->current.pos;
        if (actor->attention_info.position.y > world.y + 1.0f) {
            world = actor->attention_info.position;
        } else if (actor->eyePos.y > world.y + 1.0f) {
            world = actor->eyePos;
        } else {
            world.y += 80.0f;
        }
        world.y += 25.0f;

        cXyz cam{};
        mDoLib_pos2camera(&world, &cam);
        if (cam.z >= 0.0f) {
            continue; // behind the camera
        }

        cXyz screen{};
        mDoLib_project(&world, &screen);
        if (screen.x < -80.0f || screen.x > screenW + 80.0f || screen.y < -80.0f ||
            screen.y > screenH + 80.0f) {
            continue; // well off-screen
        }

        NameTagDrawlist::Entry& entry = m_nameTagDraw.entries[m_nameTagDraw.count++];
        entry.x = screen.x;
        entry.y = screen.y;
        std::snprintf(entry.text, sizeof(entry.text), "%s", tracked.donor.c_str());
    }

    if (m_nameTagDraw.count > 0) {
        dComIfGd_set2DOpaTop(&m_nameTagDraw);
    }
}

// =============================================================================
// validateSpawnConditions
// =============================================================================
bool EffectDispatcher::validateSpawnConditions() const {
    HIVESHOCK_CHECKPOINT("validateSpawnConditions::enter");

    auto* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        HIVESHOCK_TRACE("validateSpawnConditions: player is NULL");
        return false;
    }

    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr || stage[0] == '\0') {
        HIVESHOCK_TRACE("validateSpawnConditions: stage is NULL or empty");
        return false;
    }

    // Do not allow spawns/effects in title or file-select screen
    if (std::strcmp(stage, "name") == 0) {
        HIVESHOCK_TRACE("validateSpawnConditions: on title screen");
        return false;
    }

    // Verify the player pointer is actually dereferenceable by reading a known field
    // This catches stale/dangling player pointers during scene transitions
    volatile auto testPos = player->current.pos.x;
    (void)testPos;

    // Extended scene-safety gating (ported from tiktok_bridge, dusklight_1.4.1 —
    // see the isPlaySceneAlive/isHardTransition/... block below). Blocks any
    // engine-state mutation during pause, cutscenes, scene transitions,
    // game-over, or the brief window right after a scene becomes current
    // while its layer/room systems are still settling.
    if (!isSceneReady() || isPlayerUnableToFight()) {
        HIVESHOCK_TRACE("validateSpawnConditions: scene not ready / player unable to act");
        return false;
    }

    HIVESHOCK_CHECKPOINT("validateSpawnConditions::pass");
    return true;
}

// =============================================================================
// Extended scene-safety gating
//
// validateSpawnConditions() above only checked "is there a player and a
// stage" — that is not enough. Ported from tiktok_bridge (dusklight_1.4.1), a
// previous mod that created actors reliably over long sessions: it never
// touched engine state during pause, cutscenes, scene transitions, or the
// ~0.8s right after a scene becomes current, while the layer/room manager is
// still settling. m_stableFrames tracks that settle time, ticked once per
// frame from updateTimers() via tickSceneSafety().
// =============================================================================
bool EffectDispatcher::isPlaySceneAlive() const {
    return fpcM_SearchByName(fpcNm_PLAY_SCENE_e) != nullptr;
}

bool EffectDispatcher::isHardTransition() const {
    if (!isPlaySceneAlive()) {
        return true;
    }
    if (dComIfGp_isEnableNextStage()) {
        return true;
    }
    if (mDoRst::isReset()) {
        return true;
    }
    if (dComIfGp_getGameoverStatus() != 0) {
        return true;
    }
    JUTFader* fader = mDoGph_gInf_c::getFader();
    // Status 2 = mid wipe / blocked for scene change.
    if (fader != nullptr && fader->getStatus() == 2) {
        return true;
    }
    return false;
}

bool EffectDispatcher::isLinkProcUnsafe() const {
    auto* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr) {
        return true;
    }
    if (link->getClothesChangeWaitTimer() != 0) {
        return true;
    }
    if (link->checkPlayerDemoMode() || link->checkSpecialDemoMode()) {
        return true;
    }
    if (link->checkEventRun()) {
        return true;
    }
    switch (link->mProcID) {
    case daAlink_c::PROC_DEAD:
    case daAlink_c::PROC_FOG_DEAD:
    case daAlink_c::PROC_WARP:
    case daAlink_c::PROC_DUNGEON_WARP:
    case daAlink_c::PROC_DUNGEON_WARP_READY:
    case daAlink_c::PROC_DUNGEON_WARP_SCN_START:
    case daAlink_c::PROC_METAMORPHOSE:
    case daAlink_c::PROC_METAMORPHOSE_ONLY:
    case daAlink_c::PROC_DOOR_OPEN:
    case daAlink_c::PROC_GET_ITEM:
    case daAlink_c::PROC_TOOL_DEMO:
    case daAlink_c::PROC_DEMO_PUSH_PULL_WAIT:
    case daAlink_c::PROC_DEMO_PUSH_MOVE:
    case daAlink_c::PROC_SUBJECTIVITY:
    case daAlink_c::PROC_PEEP_SUBJECTIVITY:
    case daAlink_c::PROC_BOSS_ATN_WAIT:
    case daAlink_c::PROC_HORSE_GETOFF:
    case daAlink_c::PROC_HORSE_COMEBACK:
    case daAlink_c::PROC_WOLF_HOWL_DEMO:
        return true;
    default:
        break;
    }
    return false;
}

bool EffectDispatcher::isPlayerUnableToFight() const {
    if (isHardTransition()) {
        return true;
    }
    if (dComIfGp_isPauseFlag()) {
        return true;
    }
    if (dComIfGp_event_runCheck()) {
        return true;
    }
    return isLinkProcUnsafe();
}

// =============================================================================
// describeBlockReason — read-only mirror of isHardTransition()/
// isPlayerUnableToFight()'s exact checks, in the same order, but naming
// WHICH one is true instead of just returning a bool. Added to diagnose a
// report that effects sent during a door/entrance transition stop firing
// permanently afterward — on paper every check here re-reads live engine
// state every call, so nothing in our own code should be able to latch
// "stuck"; if something does get stuck, this will show whether it's
// isHardTransition()'s 5 checks, isLinkProcUnsafe()'s player-state checks
// (mProcID printed numerically — the door-open case is
// daAlink_c::PROC_DOOR_OPEN), or the standalone pause/event-run flags.
// =============================================================================
void EffectDispatcher::describeBlockReason(char* out, size_t outSize) const {
    if (out == nullptr || outSize == 0) {
        return;
    }
    out[0] = '\0';

    if (!isPlaySceneAlive()) {
        std::snprintf(out, outSize, "no play scene (fpcM_SearchByName(PLAY_SCENE) == null)");
        return;
    }
    if (dComIfGp_isEnableNextStage()) {
        std::snprintf(out, outSize, "dComIfGp_isEnableNextStage() == true");
        return;
    }
    if (mDoRst::isReset()) {
        std::snprintf(out, outSize, "mDoRst::isReset() == true");
        return;
    }
    if (dComIfGp_getGameoverStatus() != 0) {
        std::snprintf(out, outSize, "gameover status = %u", dComIfGp_getGameoverStatus());
        return;
    }
    JUTFader* fader = mDoGph_gInf_c::getFader();
    if (fader != nullptr && fader->getStatus() == 2) {
        std::snprintf(out, outSize, "fader status == 2");
        return;
    }
    if (dComIfGp_isPauseFlag()) {
        std::snprintf(out, outSize, "dComIfGp_isPauseFlag() == true");
        return;
    }
    if (dComIfGp_event_runCheck()) {
        std::snprintf(out, outSize, "dComIfGp_event_runCheck() == true");
        return;
    }

    auto* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr) {
        std::snprintf(out, outSize, "player null");
        return;
    }
    if (link->getClothesChangeWaitTimer() != 0) {
        std::snprintf(out, outSize, "clothes change wait timer = %u", link->getClothesChangeWaitTimer());
        return;
    }
    if (link->checkPlayerDemoMode() || link->checkSpecialDemoMode()) {
        std::snprintf(out, outSize, "player demo mode (demo=%d, special=%d)",
                      link->checkPlayerDemoMode() ? 1 : 0, link->checkSpecialDemoMode() ? 1 : 0);
        return;
    }
    if (link->checkEventRun()) {
        std::snprintf(out, outSize, "link->checkEventRun() == true");
        return;
    }
    switch (link->mProcID) {
    case daAlink_c::PROC_DEAD:
    case daAlink_c::PROC_FOG_DEAD:
    case daAlink_c::PROC_WARP:
    case daAlink_c::PROC_DUNGEON_WARP:
    case daAlink_c::PROC_DUNGEON_WARP_READY:
    case daAlink_c::PROC_DUNGEON_WARP_SCN_START:
    case daAlink_c::PROC_METAMORPHOSE:
    case daAlink_c::PROC_METAMORPHOSE_ONLY:
    case daAlink_c::PROC_DOOR_OPEN:
    case daAlink_c::PROC_GET_ITEM:
    case daAlink_c::PROC_TOOL_DEMO:
    case daAlink_c::PROC_DEMO_PUSH_PULL_WAIT:
    case daAlink_c::PROC_DEMO_PUSH_MOVE:
    case daAlink_c::PROC_SUBJECTIVITY:
    case daAlink_c::PROC_PEEP_SUBJECTIVITY:
    case daAlink_c::PROC_BOSS_ATN_WAIT:
    case daAlink_c::PROC_HORSE_GETOFF:
    case daAlink_c::PROC_HORSE_COMEBACK:
    case daAlink_c::PROC_WOLF_HOWL_DEMO:
        std::snprintf(out, outSize, "link->mProcID = %d (unsafe proc)", static_cast<int>(link->mProcID));
        return;
    default:
        break;
    }

    // Nothing blocking — leave out[] empty.
}

bool EffectDispatcher::isSceneReady() const {
    if (!isPlaySceneAlive() || dComIfGp_getPlayer(0) == nullptr || isHardTransition()) {
        return false;
    }
    return m_stableFrames >= kMinStableFrames;
}

void EffectDispatcher::tickSceneSafety() {
    // Diagnostic: log the blocking reason only when it CHANGES (entering,
    // leaving, or switching to a different reason) — never every frame, so a
    // long-stuck state doesn't flood hiveshock_forensic.log. If effects stay
    // blocked long after a door/transition should have ended, the last
    // logged line here is exactly which live engine check never cleared.
    char reason[80];
    describeBlockReason(reason, sizeof(reason));
    if (std::strcmp(reason, m_lastLoggedBlockReason) != 0) {
        if (reason[0] != '\0') {
            HIVESHOCK_TRACE("tickSceneSafety: effects now BLOCKED — %s", reason);
        } else {
            HIVESHOCK_TRACE("tickSceneSafety: effects now UNBLOCKED (were: %s)", m_lastLoggedBlockReason);
        }
        std::snprintf(m_lastLoggedBlockReason, sizeof(m_lastLoggedBlockReason), "%s", reason);
    }

    if (isHardTransition()) {
        m_stableFrames = 0;
        m_wasInHardTransition = true;
        return;
    }
    if (m_wasInHardTransition) {
        m_wasInHardTransition = false;
        m_stableFrames = 0;
    }
    if (m_stableFrames < kMinStableFrames) {
        ++m_stableFrames;
        if (m_stableFrames == kMinStableFrames) {
            DUSK_LOG_INFO("[HiveShock] Scene stable — spawns/effects unlocked");
        }
    }
}

// =============================================================================
// Spawn placement tuning & defensive helpers (2026 anti-clip / anti-crash pass)
// =============================================================================
namespace detail {

constexpr float kSpawnForwardOffset     = 300.0f; // Distance in front of Link (X/Z plane)
constexpr float kSpawnJitterRadius      = 60.0f;  // Small radial jitter so multi-spawns don't stack in one column
constexpr float kGroundCheckLift        = 200.0f; // Probe start height above Link when ground-snapping
constexpr float kMaxGroundDeltaFromLink = 400.0f; // Reject ground found this far from Link's own Y (holes/other floors)

// ---------------------------------------------------------------------------
// snapToGround — ported from tiktok_bridge's SnapSpawnPosition().
//
// Enemies used to spawn 450 units above Link and fall in; per request, they
// now spawn already standing on the ground so they can attack immediately.
// Casts a ground probe down from just above Link's own height at the target
// XZ; if solid ground turns up within kMaxGroundDeltaFromLink of Link's Y,
// use it (+ the entry's own spawnOffsetY, e.g. for flyers). Otherwise fall
// back to Link's own height — never leaves an actor with an undefined Y.
// ---------------------------------------------------------------------------
inline bool snapToGround(cXyz& pos, float linkY, float yOffset) {
    cXyz probe{};
    probe.x = pos.x;
    probe.y = linkY + kGroundCheckLift;
    probe.z = pos.z;

    if (fopAcM_gc_c::gndCheck(&probe)) {
        const float groundY = fopAcM_gc_c::getGroundY();
        if (groundY > -1.0e9f && std::fabs(groundY - linkY) <= kMaxGroundDeltaFromLink) {
            pos.y = groundY + yOffset;
            return true;
        }
    }

    pos.y = linkY + yOffset;
    return false;
}

// ---------------------------------------------------------------------------
// PlayLayerGuard — ported from tiktok_bridge (dusklight_1.4.1).
//
// fopAcM_create() registers the new process into whatever layer is currently
// "current" (fpcLy_CurrentLayer()) at the moment it's called. HiveShock's
// hook runs from a fapGm_Execute pre-hook, which is not guaranteed to leave
// the play scene's layer as current — if some other layer is active at that
// instant, the new actor gets linked into the wrong layer's process list.
// That doesn't fail at creation (fopAcM_create still returns a valid ID); it
// surfaces later, when that unrelated layer's own execute/draw pass walks a
// process it never expected to own — exactly the "succeeds now, crashes a
// moment later, no SEH exception raised" pattern seen in
// hiveshock_forensic.log across every reproduction so far. This guard forces
// the current layer to the play scene's own layer for the duration of the
// create call, then restores whatever it was before.
// ---------------------------------------------------------------------------
struct PlayLayerGuard {
    layer_class* saved    = nullptr;
    bool         restored = false;

    PlayLayerGuard() {
        saved = fpcLy_CurrentLayer();
        base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
        if (playScene != nullptr) {
            fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
        }
    }

    ~PlayLayerGuard() { restore(); }

    void restore() {
        if (!restored && saved != nullptr) {
            fpcLy_SetCurrentLayer(saved);
            restored = true;
        }
    }

    PlayLayerGuard(const PlayLayerGuard&)            = delete;
    PlayLayerGuard& operator=(const PlayLayerGuard&) = delete;
};

#if defined(_MSC_VER)
// ---------------------------------------------------------------------------
// SEH trampoline for the actual engine call. Kept free of any C++ objects
// requiring stack unwinding (std::string, std::vector, RAII guards) inside
// the __try block itself — MSVC forbids mixing __try with unwindable objects
// in the *same* function (C2712), so the risky call is isolated in its own
// tiny noexcept function, mirroring the pattern already used in mod.cpp.
// ---------------------------------------------------------------------------
static bool seh_createActor(s16 procName, u32 params, const cXyz* pos, int room,
                             const csXyz* angle, s8 argument, fpc_ProcID* outId) noexcept {
    __try {
        *outId = fopAcM_create(procName, params, pos, room, angle, nullptr, argument);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

} // namespace detail

// =============================================================================
// getSpawnPosition
// =============================================================================
// Places the spawn point in front of Link (using his facing angle) and
// ground-snapped so actors are already standing and can attack immediately
// instead of falling in from above.
cXyz EffectDispatcher::getSpawnPosition(float offsetY) const {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> jitterAngleDist(0.0f, 6.28318530718f);
    std::uniform_real_distribution<float> jitterRadiusDist(0.0f, detail::kSpawnJitterRadius);

    auto* player = dComIfGp_getPlayer(0);
    cXyz pos{};
    if (player == nullptr) {
        // Callers always check validateSpawnConditions() first, but stay
        // defensive in case this is ever called directly in the future.
        return pos;
    }

    pos = player->current.pos;

    // --- Forward projection using Link's facing angle (shape_angle.y) ------
    // shape_angle.y is a BAM-style s16 angle — the same convention this file
    // already uses in the other direction inside spawnActor() (angle *
    // (32768/pi) there), so we invert that here: angle * (pi/32768).
    // If entities end up spawning BEHIND Link instead of in front on your
    // build, your engine's angle winding is mirrored — flip the sign of
    // forwardX/forwardZ below.
    const float facingRad = static_cast<float>(player->shape_angle.y) * (3.14159265f / 32768.0f);
    const float forwardX  = std::sin(facingRad) * detail::kSpawnForwardOffset;
    const float forwardZ  = std::cos(facingRad) * detail::kSpawnForwardOffset;

    // Small circular jitter around that forward point so multi-spawns
    // (armies/swarms) don't stack in the exact same column of air.
    const float jAngle  = jitterAngleDist(rng);
    const float jRadius = jitterRadiusDist(rng);

    pos.x += forwardX + jRadius * std::cos(jAngle);
    pos.z += forwardZ + jRadius * std::sin(jAngle);

    const bool grounded = detail::snapToGround(pos, player->current.pos.y, offsetY);
    HIVESHOCK_TRACE("getSpawnPosition: %s (y=%.1f)", grounded ? "snapped to ground" : "no ground found, used Link's Y", pos.y);

    // Defensive: never hand the engine a non-finite position (a malformed
    // "amount" from the network could theoretically feed huge offsetY values
    // through customOffsetY upstream).
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        HIVESHOCK_TRACE("getSpawnPosition: computed NON-FINITE position — falling back to player position");
        pos = player->current.pos;
    }

    return pos;
}

// =============================================================================
// spawnActor
// =============================================================================
EffectResult EffectDispatcher::spawnActor(const SpawnEntry& entry, uint32_t customParams, float customOffsetY,
                                           const std::string& senderName) {
    HIVESHOCK_CHECKPOINT("spawnActor::enter");
    HIVESHOCK_TRACE("spawnActor: procName=0x%04X, arc=%s, display=%s",
                    entry.procName, entry.arcName ? entry.arcName : "(null)", entry.displayName);

    if (!validateSpawnConditions()) {
        return {false, "Cannot spawn: game not in active session"};
    }

    // --- Resource note: archives stream in on demand, not pre-checked here ---
    // Every actor spawned through fopAcM_create runs its own multi-phase
    // create() (fpcBs_SubCreate: cPhs_INIT_e -> cPhs_LOADING_e -> cPhs_NEXT_e/
    // cPhs_COMPLEATE_e), and that create() calls dComIfG_resLoad() itself for
    // its archive (verified against daE_OC_c::create() in d_a_e_oc.cpp,
    // which loads "E_ocb" exactly this way). That's the same path used for
    // every naturally-placed enemy in the game, so requiring the archive to
    // already be resident before calling fopAcM_create here was wrong — it
    // blocked spawns of anything not already loaded by the current room,
    // which is the common case for HiveShock. The engine streams it in and
    // ticks the actor until ready; nothing here needs to wait on it.
    //
    // The one case that DOES need a synchronous pre-check is a direct model
    // swap with no create()-phase machinery behind it — e.g. changeWolf()
    // (d_a_alink_wolf.inc) dereferences "Wmdl" resources immediately with no
    // loading state machine, so handleWolfToggle/handleForceWolf still gate
    // on dComIfG_getObjectResInfo("Wmdl") before calling it.
    if (entry.arcName != nullptr && entry.arcName[0] != '\0' &&
        dComIfG_getObjectResInfo(entry.arcName) == nullptr) {
        HIVESHOCK_TRACE("spawnActor: resource '%s' not yet resident — engine will stream it in via create()",
                        entry.arcName);
    }

    // --- Actor count limit: prevent heap exhaustion ---
    // Prune dead entries first, then recycle the oldest instead of refusing —
    // a full list would otherwise block every later spawn.
    if (m_spawnedEnemies.size() >= kMaxTrackedActors) {
        for (auto it = m_spawnedEnemies.begin(); it != m_spawnedEnemies.end();) {
            if (!fpcM_IsCreating(it->id) && fopAcM_SearchByID(it->id) == nullptr) {
                it = m_spawnedEnemies.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (m_spawnedEnemies.size() >= kMaxTrackedActors) {
        DUSK_LOG_WARN("[HiveShock] Actor limit reached (%zu/%zu) — recycling oldest spawn",
                      m_spawnedEnemies.size(), kMaxTrackedActors);
        safeDelete(m_spawnedEnemies.front().id);
        m_spawnedEnemies.erase(m_spawnedEnemies.begin());
    }

    HIVESHOCK_CHECKPOINT("spawnActor::getSpawnPosition");
    cXyz spawnPos = getSpawnPosition(entry.spawnOffsetY + customOffsetY);

    // Fetch the player early: we need it both for the room number below and
    // for the facing-angle calculation further down.
    auto* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        // validateSpawnConditions() already checked this, but the pointer can
        // theoretically go stale between calls (e.g. a scene transition
        // landing mid-frame) — re-check right before we touch it again.
        HIVESHOCK_TRACE("spawnActor: player went NULL after validation — aborting spawn");
        return {false, "Player pointer went null before spawn"};
    }

    // Room MUST be the player's current room (fopAcM_GetRoomNo reads
    // i_actor->current.roomNo), not dComIfGp_getStartStageRoomNo() — that
    // returns the room the stage was *entered* at and goes stale the moment
    // Link crosses into a different room of the same stage. Every per-room
    // engine lookup the spawned actor's own create()/execute() ticks make
    // afterward (solid heap, BG collision, tevstr, room camera/zone data) is
    // indexed by the actor's own room number, so a stale room here doesn't
    // fail loudly at spawn time — fopAcM_create() itself succeeds — it
    // crashes a frame or two later inside the actor's own room-indexed
    // lookups, which matches the "spawnActor::success then crash" pattern
    // seen in hiveshock_forensic.log.
    int room = fopAcM_GetRoomNo(player);

    s16 angleY = 0;
    {
        float dx = player->current.pos.x - spawnPos.x;
        float dz = player->current.pos.z - spawnPos.z;
        angleY = static_cast<s16>(std::atan2(dx, dz) * (32768.0f / 3.14159265f));
    }
    csXyz angle(0, angleY, 0);

    u32 params = (customParams != 0) ? customParams : entry.defaultParams;

    // Puppet Zelda's create() unconditionally turns on dungeon save switch 2
    // the first time; remember its prior state so tamePuppetZelda() can undo it.
    const bool dunSwitch2WasSet = (entry.procName == fpcNm_E_HZELDA_e) && dComIfGs_isSaveDunSwitch(2);

    HIVESHOCK_TRACE("spawnActor: calling fopAcM_create(procName=0x%04X, params=0x%08X, room=%d, arg=%d)",
                    entry.procName, params, room, entry.argument);

    // -------------------------------------------------------------------
    // Guarded spawn: SEH + immediate log flush around the risky engine call.
    // (No stage-flag bypass here: this engine has no runtime "no enemy"
    // flag for fopAcM_create to check — Kakariko/Ordon are enemy-free
    // because their room data never places enemy actors there, not because
    // of a gate that can be toggled. Confirmed by reading fpcBs_Create,
    // dStage_stagInfo_c's real fields, and E_OC's create() path: none of
    // them reference any such flag.)
    // -------------------------------------------------------------------
    constexpr fpc_ProcID kInvalidProc = static_cast<fpc_ProcID>(0xFFFFFFFF);
    fpc_ProcID actorId    = kInvalidProc;
    bool       spawnSurvived = true;
    {
        detail::PlayLayerGuard layerGuard; // force current layer to the play scene before creating
        HIVESHOCK_CHECKPOINT("spawnActor::pre_fopAcM_create");
        hiveshock::ForensicLog::instance().flush(); // guarantee this line hits disk before the risky call

#if defined(_MSC_VER)
        spawnSurvived = detail::seh_createActor(entry.procName, params, &spawnPos, room,
                                                &angle, entry.argument, &actorId);
#else
        try {
            actorId = fopAcM_create(entry.procName, params, &spawnPos, room, &angle, nullptr, entry.argument);
        } catch (const std::exception& ex) {
            spawnSurvived = false;
            DUSK_LOG_ERROR("[HiveShock] Exception in fopAcM_create for %s: %s", entry.displayName, ex.what());
        } catch (...) {
            spawnSurvived = false;
            DUSK_LOG_ERROR("[HiveShock] Unknown exception in fopAcM_create for %s", entry.displayName);
        }
#endif

        HIVESHOCK_CHECKPOINT("spawnActor::post_fopAcM_create");
        hiveshock::ForensicLog::instance().flush();
    }

    if (!spawnSurvived) {
        DUSK_LOG_ERROR("[HiveShock] CRITICAL: native exception spawning %s — crash prevented!",
                       entry.displayName);
        HIVESHOCK_TRACE("spawnActor: SEH CAUGHT during fopAcM_create for %s", entry.displayName);
        return {false, "Crash prevented while spawning (native exception caught)"};
    }

    if (actorId == kInvalidProc) {
        std::string msg = "fopAcM_create returned invalid for " + std::string(entry.displayName);
        DUSK_LOG_WARN("[HiveShock] %s", msg.c_str());
        HIVESHOCK_TRACE("spawnActor: FAILED — invalid proc ID returned");
        return {false, std::move(msg)};
    }

    TrackedSpawn tracked{actorId, truncateDonorName(senderName)};
    tracked.procName = entry.procName;
    tracked.spawnFrame = m_frameCounter;
    tracked.zeldaDunSwitch2WasSet = dunSwitch2WasSet;
    m_spawnedEnemies.push_back(std::move(tracked));
    // forceAggro() runs later from tickTrackedSpawns(), once create() is done.

    DUSK_LOG_INFO("[HiveShock] Spawned %s (procName 0x%04X, procId 0x%08X, arg=%d) facing Link — stage=%s room=%d pos=(%.0f,%.0f,%.0f) params=0x%08X",
                  entry.displayName, entry.procName, actorId, entry.argument,
                  dComIfGp_getStartStageName() ? dComIfGp_getStartStageName() : "?", room,
                  spawnPos.x, spawnPos.y, spawnPos.z, params);
    logHeapState("after fopAcM_create");
    HIVESHOCK_CHECKPOINT("spawnActor::success");

    return {true, std::string("Spawned ") + entry.displayName};
}

// =============================================================================
// forceAggro — ported from tiktok_bridge's ForceAggro(). Sets each actor
// type's own internal action-state fields so it immediately chases/attacks
// Link instead of starting in its default idle/wait state. Field names,
// offsets and enum values are copied verbatim from that proven mod and were
// re-checked against this engine's current headers before porting (see the
// include block at the top of this file) — every one matched exactly.
// =============================================================================
void EffectDispatcher::forceAggro(fopAc_ac_c* actor) const {
    if (actor == nullptr) {
        return;
    }

    // Face Link immediately regardless of type — spawnActor() already aims
    // the creation angle at Link, but some actors' own create() overwrites
    // it, so re-assert it here too.
    auto* player = dComIfGp_getPlayer(0);
    if (player != nullptr) {
        const s16 yaw = fopAcM_searchPlayerAngleY(actor);
        actor->current.angle.y = yaw;
        actor->shape_angle.y   = yaw;
    }

    const s16 name = fopAcM_GetName(actor);
    switch (name) {
    case fpcNm_E_OC_e:
        // FIND -> chase/attack Link
        static_cast<daE_OC_c*>(actor)->setActionMode(4, 1);
        break;
    case fpcNm_E_BA_e: {
        auto* keese = reinterpret_cast<e_ba_class*>(actor);
        keese->mAction = e_ba_class::ACT_FIGHT_FLY;
        keese->mMode = 0;
        break;
    }
    case fpcNm_E_GE_e:
        static_cast<daE_GE_c*>(actor)->setActionMode(2); // ACTION_ATTACK
        break;
    case fpcNm_E_DN_e: {
        auto* liz = reinterpret_cast<e_dn_class*>(actor);
        liz->action = 3; // ACTION_FIGHT_RUN
        liz->mode = 0;
        break;
    }
    case fpcNm_E_WW_e:
        // Nothing to force: the spawn params (see spawn_table.hpp) make it
        // attack on its own right after its appear animation. Forcing CHASE
        // here would skip executeWait()'s mode 10, which is what clears the
        // status flag its appear animation sets.
        break;
    case fpcNm_E_TT_e:
        static_cast<daE_TT_c*>(actor)->setActionMode(1, 0); // ACTION_CHASE
        break;
    case fpcNm_E_ST_e: {
        auto* st = reinterpret_cast<e_st_class*>(actor);
        st->mAction = 0x33; // ACTION_G_FIGHT
        st->mActionPhase = 0;
        break;
    }
    case fpcNm_E_MM_e: {
        auto* mm = reinterpret_cast<e_mm_class*>(actor);
        mm->action = 1; // ACTION_DASH
        mm->mode = 0;
        break;
    }
    case fpcNm_E_SM2_e: {
        auto* sm = reinterpret_cast<e_sm2_class*>(actor);
        sm->action = ACTION_ATTACK;
        sm->mode = 0;
        break;
    }
    case fpcNm_E_GI_e:
        static_cast<daE_GI_c*>(actor)->setActionMode(2, 0); // ACTION_CHASE_e
        break;
    case fpcNm_E_YK_e: {
        auto* yk = static_cast<e_yk_class*>(actor);
        yk->mAction = ACT_FIGHT_FLY;
        yk->mActionPhase = 0;
        break;
    }
    case fpcNm_E_YM_e:
        static_cast<daE_YM_c*>(actor)->setActionMode(daE_YM_c::ACT_ATTACK);
        break;
    case fpcNm_E_YG_e: {
        auto* yg = reinterpret_cast<e_yg_class*>(actor);
        yg->mAction = 1; // ACTION_ATTACK
        yg->mActionMode = 0;
        break;
    }
    case fpcNm_E_S1_e: {
        auto* s1 = static_cast<e_s1_class*>(actor);
        s1->mAction = 2; // ACT_FIGHT_RUN
        s1->mMode = 0;
        break;
    }
    case fpcNm_E_HP_e:
        // Skip the idle "laugh" wait: go straight to chasing Link (executeMove ->
        // attack once close). Its own wait would do this when Link is in range.
        static_cast<daE_HP_c*>(actor)->setActionMode(1, 0);
        break;
    case fpcNm_NI_e: {
        // TP cuccos don't fight; AWAY makes them react and swarm the player.
        auto* cucco = static_cast<ni_class*>(actor);
        cucco->mAction = ACTION_AWAY_e;
        cucco->mMode = 0;
        break;
    }
    case fpcNm_COW_e: {
        auto* goat = static_cast<daCow_c*>(actor);
        goat->setProcess(&daCow_c::action_run, false);
        break;
    }
    default:
        // No known aggro data for this type — it still faces Link (above)
        // and its own natural player-search may pick him up.
        break;
    }
}

// =============================================================================
// handleSpawnEntity
// =============================================================================
EffectResult EffectDispatcher::handleSpawnEntity(const CommandEvent& event) {
    const SpawnEntry* entry = findSpawnEntry(event.commandId);
    if (entry == nullptr) {
        return {false, "No spawn entry for command: " + event.commandId};
    }
    return spawnActor(*entry, 0, 0.0f, event.senderName);
}

// =============================================================================
// handleRandomEnemy
// =============================================================================
EffectResult EffectDispatcher::handleRandomEnemy(const CommandEvent& event) {
    const auto& enemyIds = getRandomEnemyIds();
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, enemyIds.size() - 1);
    const std::string& pickedId = enemyIds[dist(rng)];

    const SpawnEntry* entry = findSpawnEntry(pickedId);
    if (entry == nullptr) {
        return {false, "Random enemy failed: ID not in table: " + pickedId};
    }

    auto res = spawnActor(*entry, 0, 0.0f, event.senderName);
    if (res.success) {
        DUSK_LOG_INFO("[HiveShock] Random enemy spawned: %s — triggered by %s",
                      entry->displayName, event.senderName.c_str());
    }
    return res;
}

// =============================================================================
// handleArmy
// =============================================================================
EffectResult EffectDispatcher::handleArmy(const CommandEvent& event) {
    const std::string& cmd = event.commandId;
    std::string targetEnemy = "bokoblin";
    uint32_t count = event.amount > 0 ? event.amount : 5;

    if (cmd == "cucco_army") {
        targetEnemy = "cucco";
        count = event.amount > 0 ? event.amount : 5;
    } else if (cmd == "goat_army") {
        targetEnemy = "goat";
        count = event.amount > 0 ? event.amount : 4;
    } else if (cmd == "bokoblin_army") {
        targetEnemy = "bokoblin";
        count = event.amount > 0 ? event.amount : 5;
    }

    const SpawnEntry* entry = findSpawnEntry(targetEnemy);
    if (entry == nullptr) {
        return {false, "Unknown army target: " + targetEnemy};
    }

    // Queued at one spawn per frame instead of a tight loop here — see the
    // m_pendingSpawns comment in effects.hpp.
    queueSpawns(*entry, count, std::string(entry->displayName) + " army", event.senderName);
    return {true, "Queued " + std::string(entry->displayName) + " army (" + std::to_string(count) + ")"};
}

// =============================================================================
// handleBeeSwarm
// =============================================================================
EffectResult EffectDispatcher::handleBeeSwarm(const CommandEvent& event) {
    uint32_t count = event.amount > 0 ? event.amount : 12;
    const SpawnEntry* entry = findSpawnEntry("bee");
    if (entry == nullptr) {
        return {false, "Bee spawn entry missing from table"};
    }

    queueSpawns(*entry, count, "Bee swarm", event.senderName);
    return {true, "Queued bee swarm (" + std::to_string(count) + ")"};
}

// =============================================================================
// handleGreatFairy — spawns the Great Fairy NPC AND applies a full heal
// directly, rather than relying on her to heal Link herself.
//
// Read her full create()/execute() (d_a_npc_fairy.cpp): there is no
// dComIfGs_setLife/setItemLifeCount call anywhere in her own C++ logic. Her
// healing is entirely driven by the event-flow script referenced by
// getFlowNodeNo() (mFlowNodeNo, read from home.angle.x, tied to her authored
// placement + room's dialogue data) — a fountain cutscene we have no way to
// trigger for a field spawn with no such data. So we guarantee the actual
// "cura normalmente" behaviour ourselves, the same way handleExplode spawns
// a decorative bomb AND applies real damage/launch itself rather than
// trusting the bomb's own collision logic to hit Link.
// =============================================================================
EffectResult EffectDispatcher::handleGreatFairy(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot summon Great Fairy: not in game"};

    const SpawnEntry* entry = findSpawnEntry("great_fairy");
    if (entry == nullptr) {
        return {false, "Great Fairy spawn entry missing from table"};
    }

    EffectResult spawnResult = spawnActor(*entry, 0, 0.0f, event.senderName);
    if (!spawnResult.success) {
        DUSK_LOG_WARN("[HiveShock] Great Fairy actor spawn failed (%s) — healing anyway",
                      spawnResult.message);
    }

    u16 maxHP = dComIfGs_getMaxLife();
    dComIfGs_setLife(maxHP);
    DUSK_LOG_INFO("[HiveShock] Great Fairy healed player to full (%u) — triggered by %s",
                  maxHP, event.senderName.c_str());

    return {true, spawnResult.success ? "Great Fairy appeared and healed you fully!"
                                       : "Healed to full (Great Fairy didn't appear)"};
}

// =============================================================================
// handleClearEnemies
// =============================================================================
EffectResult EffectDispatcher::handleClearEnemies(const CommandEvent& event) {
    size_t count = 0;
    for (const auto& tracked : m_spawnedEnemies) {
        if (fopAcM_SearchByID(tracked.id) != nullptr) {
            safeDelete(tracked.id);
            count++;
        }
    }
    m_spawnedEnemies.clear();
    DUSK_LOG_INFO("[HiveShock] Cleared %zu spawned enemies — triggered by %s",
                  count, event.senderName.c_str());
    return {true, "Cleared " + std::to_string(count) + " enemies"};
}

// =============================================================================
// handleHeal
// =============================================================================
EffectResult EffectDispatcher::handleHeal(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot heal: not in game"};
    u16 current = dComIfGs_getLife();
    u16 maxHP   = dComIfGs_getMaxLife();
    u16 toHeal  = event.amount > 0 ? static_cast<u16>(event.amount) : 16;
    u16 newHP   = static_cast<u16>(std::min<int>(current + toHeal, maxHP));
    dComIfGs_setLife(newHP);
    DUSK_LOG_INFO("[HiveShock] Healed player by %u (was %u, now %u) — triggered by %s",
                  toHeal, current, newHP, event.senderName.c_str());
    return {true, "Healed player for " + std::to_string(toHeal)};
}

// =============================================================================
// handleDamage
// =============================================================================
EffectResult EffectDispatcher::handleDamage(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot damage: not in game"};
    u16 current = dComIfGs_getLife();
    u16 dmg     = event.amount > 0 ? static_cast<u16>(event.amount) : 8;
    u16 newHP   = (current > dmg + 1) ? static_cast<u16>(current - dmg) : static_cast<u16>(1);
    dComIfGs_setLife(newHP);
    DUSK_LOG_INFO("[HiveShock] Damaged player by %u (was %u, now %u) — triggered by %s",
                  dmg, current, newHP, event.senderName.c_str());
    return {true, "Damaged player for " + std::to_string(dmg)};
}

// =============================================================================
// handleKill
// =============================================================================
EffectResult EffectDispatcher::handleKill(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot kill: not in game"};
    dComIfGs_setLife(0);
    DUSK_LOG_INFO("[HiveShock] Instakill player — triggered by %s", event.senderName.c_str());
    return {true, "Player killed"};
}

// =============================================================================
// handleRupees
// =============================================================================
EffectResult EffectDispatcher::handleRupees(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot modify rupees: not in game"};
    u16 current = dComIfGs_getRupee();
    u16 maxR    = dComIfGs_getRupeeMax();
    u16 toAdd   = event.amount > 0 ? static_cast<u16>(event.amount) : 20;
    u16 newVal  = static_cast<u16>(std::min<int>(current + toAdd, maxR));
    dComIfGs_setRupee(newVal);
    DUSK_LOG_INFO("[HiveShock] Added %u rupees (was %u, now %u) — triggered by %s",
                  toAdd, current, newVal, event.senderName.c_str());
    return {true, "Added " + std::to_string(toAdd) + " rupees"};
}

// =============================================================================
// handleStealRupees
// =============================================================================
EffectResult EffectDispatcher::handleStealRupees(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot steal rupees: not in game"};
    u16 old = dComIfGs_getRupee();
    dComIfGs_setRupee(0);
    DUSK_LOG_INFO("[HiveShock] Stole all %u rupees — triggered by %s", old, event.senderName.c_str());
    return {true, "Stole all rupees"};
}

// =============================================================================
// handleFillRupees
// =============================================================================
EffectResult EffectDispatcher::handleFillRupees(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot fill rupees: not in game"};
    u16 maxR = dComIfGs_getRupeeMax();
    dComIfGs_setRupee(maxR);
    DUSK_LOG_INFO("[HiveShock] Filled wallet to %u rupees — triggered by %s", maxR, event.senderName.c_str());
    return {true, "Filled wallet to max rupees"};
}

// =============================================================================
// handleEmptyOil
// =============================================================================
EffectResult EffectDispatcher::handleEmptyOil(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot empty oil: not in game"};
    dComIfGs_setOil(0);
    DUSK_LOG_INFO("[HiveShock] Lantern oil emptied — triggered by %s", event.senderName.c_str());
    return {true, "Lantern oil emptied"};
}

// =============================================================================
// handleFillOil
// =============================================================================
EffectResult EffectDispatcher::handleFillOil(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot fill oil: not in game"};
    u16 maxOil = dComIfGs_getMaxOil();
    dComIfGs_setOil(maxOil);
    DUSK_LOG_INFO("[HiveShock] Lantern oil filled to %u — triggered by %s", maxOil, event.senderName.c_str());
    return {true, "Lantern oil filled"};
}

// =============================================================================
// handleKnockback
// =============================================================================
EffectResult EffectDispatcher::handleKnockback(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot knockback: not in game"};
    auto* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr) return {false, "Player null"};

    // Writing player->speed.y/speedF directly (the old code) never visibly
    // applied: Link's own per-frame movement update overwrites those fields
    // from his current proc's physics before they get a chance to render.
    // setThrowDamage() is the real API the engine uses to shove Link around
    // (also used by real hazards) — it drives the actual launch/knockback
    // state machine instead of fighting it. Ported from tiktok_bridge.
    float strength = event.amount > 0 ? static_cast<float>(event.amount) : 8.0f;
    strength = std::clamp(strength, 2.0f, 50.0f);
    constexpr int kKnockbackDamage = 8; // setThrowDamage always deals some damage; matches tiktok_bridge's default

    const s16 yaw = static_cast<s16>(link->shape_angle.y + 0x8000); // away from Link's facing
    link->setThrowDamage(yaw, strength * 4.5f, strength * 4.0f, kKnockbackDamage, 1, 0);

    DUSK_LOG_INFO("[HiveShock] Knockback (strength=%.1f) — triggered by %s", strength, event.senderName.c_str());
    return {true, "Knockback applied"};
}

// =============================================================================
// handleExplode — Explodes bomb directly on Link with launch physics & damage
// =============================================================================
EffectResult EffectDispatcher::handleExplode(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot explode: not in game"};
    auto* player = dComIfGp_getPlayer(0);
    if (player == nullptr) return {false, "Player null"};

    cXyz pos = player->current.pos;
    int room = fopAcM_GetRoomNo(player); // current room, not the stage's start room — see spawnActor()

    // Spawn real bomb actor directly at Link's feet, guarded the same way
    // spawnActor() guards fopAcM_create (this call was previously unguarded).
    fpc_ProcID bombId = static_cast<fpc_ProcID>(0xFFFFFFFF);
    {
        detail::PlayLayerGuard layerGuard;
#if defined(_MSC_VER)
        detail::seh_createActor(fpcNm_NBOMB_e, 0, &pos, room, nullptr, 0, &bombId);
#else
        try {
            bombId = fopAcM_create(fpcNm_NBOMB_e, 0, &pos, room, nullptr, nullptr, 0);
        } catch (...) {
            DUSK_LOG_ERROR("[HiveShock] Exception spawning bomb in handleExplode — crash prevented");
        }
#endif
    }
    HIVESHOCK_TRACE("handleExplode: bomb procId=0x%08X (room=%d)", bombId, room);

    // Launch + damage Link via the real API (see handleKnockback for why the
    // old direct speed.y/speedF writes never visibly applied).
    // strength (launch force) and damage are independent wire fields — see
    // CommandEvent::damage for why "damage" used to silently collide with
    // "strength" here.
    auto* link = static_cast<daAlink_c*>(player);
    float strength = event.amount > 0 ? static_cast<float>(event.amount) : 18.0f;
    strength = std::clamp(strength, 4.0f, 50.0f);
    const s16 yaw = static_cast<s16>(link->shape_angle.y + 0x8000);
    u16 dmg = event.damage > 0 ? static_cast<u16>(event.damage) : 8; // 2 full hearts default
    link->setThrowDamage(yaw, strength * 5.0f, strength * 4.5f, dmg, 1, 0);

    DUSK_LOG_INFO("[HiveShock] Bomb exploded on Link (strength=%.1f, dmg=%u) — triggered by %s",
                  strength, dmg, event.senderName.c_str());
    return {true, "Bomb exploded Link!"};
}

// =============================================================================
// handleWolfToggle
// =============================================================================
EffectResult EffectDispatcher::handleWolfToggle(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot toggle wolf: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot toggle wolf during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (player->checkHorseRide()) return {false, "Cannot transform while riding horse"};

    if (player->checkWolf()) {
        link->changeLink(0);
        DUSK_LOG_INFO("[HiveShock] Transformed to Human Link — triggered by %s", event.senderName.c_str());
        return {true, "Transformed to Human Link"};
    } else {
        if (dComIfG_getObjectResInfo("Wmdl") == nullptr) {
            DUSK_LOG_WARN("[HiveShock] Wolf Link model (Wmdl) not loaded in this stage — transformation blocked to prevent crash");
            return {false, "Wolf Link resources not loaded in current area"};
        }
        link->changeWolf();
        DUSK_LOG_INFO("[HiveShock] Transformed to Wolf Link — triggered by %s", event.senderName.c_str());
        return {true, "Transformed to Wolf Link"};
    }
}

// =============================================================================
// handleForceWolf
// =============================================================================
EffectResult EffectDispatcher::handleForceWolf(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot force wolf: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot force wolf during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (player->checkHorseRide()) return {false, "Cannot transform while riding horse"};

    if (player->checkWolf()) {
        return {true, "Already in Wolf form"};
    }

    if (dComIfG_getObjectResInfo("Wmdl") == nullptr) {
        DUSK_LOG_WARN("[HiveShock] Wolf Link model (Wmdl) not loaded in this stage — transformation blocked to prevent crash");
        return {false, "Wolf Link resources not loaded in current area"};
    }

    link->changeWolf();
    DUSK_LOG_INFO("[HiveShock] Forced Wolf form — triggered by %s", event.senderName.c_str());
    return {true, "Forced Wolf Link form"};
}

// =============================================================================
// handleForceHuman
// =============================================================================
EffectResult EffectDispatcher::handleForceHuman(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot force human: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot force human during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (!player->checkWolf()) {
        return {true, "Already in Human form"};
    }

    link->changeLink(0);
    DUSK_LOG_INFO("[HiveShock] Forced Human form — triggered by %s", event.senderName.c_str());
    return {true, "Forced Human Link form"};
}

// =============================================================================
// handleIronBootsToggle
// =============================================================================
EffectResult EffectDispatcher::handleIronBootsToggle(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot toggle boots: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot toggle boots during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (player->checkWolf()) {
        return {false, "Cannot equip boots in Wolf form"};
    }

    bool on = player->checkEquipHeavyBoots() != 0;
    link->setHeavyBoots(!on);
    DUSK_LOG_INFO("[HiveShock] Iron boots toggled (now %s) — triggered by %s",
                  !on ? "ON" : "OFF", event.senderName.c_str());
    return {true, std::string("Iron boots toggled ") + (!on ? "ON" : "OFF")};
}

// =============================================================================
// handleIronBootsOn
// =============================================================================
EffectResult EffectDispatcher::handleIronBootsOn(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot equip boots: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot equip boots during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (player->checkWolf()) {
        return {false, "Cannot equip boots in Wolf form"};
    }

    link->setHeavyBoots(1);
    DUSK_LOG_INFO("[HiveShock] Iron boots equipped — triggered by %s", event.senderName.c_str());
    return {true, "Iron boots equipped"};
}

// =============================================================================
// handleIronBootsOff
// =============================================================================
EffectResult EffectDispatcher::handleIronBootsOff(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot unequip boots: not in game"};
    if (dComIfGp_event_runCheck()) return {false, "Cannot unequip boots during cutscene"};

    auto* player = reinterpret_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
    auto* link   = reinterpret_cast<daAlink_c*>(player);
    if (link == nullptr) return {false, "Player null"};

    if (player->checkWolf()) {
        return {false, "Cannot equip boots in Wolf form"};
    }

    link->setHeavyBoots(0);
    DUSK_LOG_INFO("[HiveShock] Iron boots unequipped — triggered by %s", event.senderName.c_str());
    return {true, "Iron boots unequipped"};
}

// =============================================================================
// handleSummonEpona
// =============================================================================
EffectResult EffectDispatcher::handleSummonEpona(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot summon Epona: not in game"};
    fopAc_ac_c* horse = reinterpret_cast<fopAc_ac_c*>(dComIfGp_getHorseActor());
    auto* player = dComIfGp_getPlayer(0);
    if (horse != nullptr && player != nullptr) {
        horse->current.pos = player->current.pos;
        horse->current.pos.x += 120.0f;
        horse->old.pos = horse->current.pos;
        DUSK_LOG_INFO("[HiveShock] Epona teleported to player — triggered by %s", event.senderName.c_str());
        return {true, "Epona summoned to player"};
    } else {
        // daHorse_c::create() (d_a_horse.cpp) streams its own archive via
        // dComIfG_resLoad and cleanly returns cPhs_ERROR_e if a horse actor
        // already exists or the stage forbids it — same on-demand pattern as
        // every enemy, so no resource pre-check is needed here either.
        SpawnEntry horseEntry{fpcNm_HORSE_e, "Horse", "Epona", 0.0f, 0, 0};
        return spawnActor(horseEntry, 0, 0.0f, event.senderName);
    }
}

// =============================================================================
// handleDismountHorse
// =============================================================================
EffectResult EffectDispatcher::handleDismountHorse(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot dismount: not in game"};
    auto* link = static_cast<daAlink_c*>(dComIfGp_getPlayer(0));
    if (link == nullptr) return {false, "Player null"};

    if (!link->checkHorseRide()) {
        return {true, "Player is not riding a horse"};
    }

    // Proper dismount proc instead of poking speed.y/speedF directly (same
    // "gets overwritten by Link's own physics update" issue as knockback).
    link->procHorseGetOffInit(0);
    DUSK_LOG_INFO("[HiveShock] Dismounted player from horse — triggered by %s", event.senderName.c_str());
    return {true, "Dismounted from horse"};
}

// =============================================================================
// handleReset — Return to title/file select WITHOUT wiping the save
// =============================================================================
EffectResult EffectDispatcher::handleReset(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot reset: not in game"};

    // CORRECTION (crash report 2026-09-20 11:55): mDoRst::onReturnToMenu()
    // (the previous fix here) is NOT a lighter/standalone way to reach the
    // menu — read dComIfG_resetToOpening() (d_com_inf_game.cpp): it
    // explicitly ABORTS ("return 0") whenever isReturnToMenu() is true. That
    // flag exists for the in-game Home-button quick-menu (d_home_button.cpp),
    // whose handler sets it and then calls its OWN dHomeButton_c::onReset()
    // — a member function that assumes the Home-menu's HBM UI subsystem
    // (m_myObj, field_0x7c, HBMInit()) is already alive. We never open that
    // menu, so setting the flag alone just permanently blocks the normal
    // reset path with nothing left to drive the transition — hence the
    // black screen, and the null-pointer crash when some other Home-menu-
    // only code path assumed that subsystem existed.
    //
    // The correct, minimal trigger is mDoRst::onReset() alone (same one
    // handleDeleteSave() already uses successfully) — dComIfG_resetToOpening
    // proceeds normally as long as isReturnToMenu() stays false. The only
    // real difference from handleDeleteSave() is that we skip the save wipe.
    mDoRst::onReset();
    DUSK_LOG_INFO("[HiveShock] Reset to menu (save intact) — triggered by %s", event.senderName.c_str());
    return {true, "Returned to menu (save intact)"};
}

// =============================================================================
// handleDeleteSave — Wipe current save and return to menu
// =============================================================================
EffectResult EffectDispatcher::handleDeleteSave(const CommandEvent& event) {
    if (!validateSpawnConditions()) return {false, "Cannot delete save: not in game"};

    // Same black-screen problem as handleReset, plus the old code only wiped
    // the in-RAM save struct (save->init()) without persisting the wipe to
    // the memory card, so a reload would have brought the old save right
    // back. This sequence (wipe RAM -> write blank data to the card buffer ->
    // checksum -> save -> trigger reset) is ported from tiktok_bridge, which
    // reset cleanly; mDoRst::onReset() is the full app-level reset (title
    // screen), distinct from onReturnToMenu()'s "keep save" path above.
    const u8 slot = dComIfGs_getDataNum();
    dComIfGs_init();
    dComIfGs_setInitDataToCard(g_mDoMemCd_control.mData, static_cast<int>(slot));
    mDoMemCdRWm_SetCheckSumGameData(g_mDoMemCd_control.mData, slot);
    mDoMemCd_save(g_mDoMemCd_control.mData, SAVEFILE_SIZE, 0);
    mDoRst::onReset();

    DUSK_LOG_INFO("[HiveShock] Save deleted & reset to menu — triggered by %s", event.senderName.c_str());
    return {true, "Save deleted and returned to menu"};
}

} // namespace hiveshock
