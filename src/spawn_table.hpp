// =============================================================================
// HiveShock Mod for Dusklight v2.0.0
// spawn_table.hpp — Actor spawn registry (header-only)
//
// Maps effect actions (e.g. "bokoblin", "helmsaurus") to engine actor process
// IDs, archives, offsets, parameters, and arguments.
//
// Copyright (c) 2026 HiveShock Contributors — MIT License
// =============================================================================
#pragma once

#include "global.h"                  // u16, s16, etc.
#include "f_pc/f_pc_name.h"          // fpcNm_*_e actor process names

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hiveshock {

struct SpawnEntry {
    s16         procName;          ///< Engine process/profile name (from fpcNm_*_e).
    const char* arcName;           ///< Resource archive (e.g. "E_ocb", "E_MM", "Cow"). May be nullptr.
    const char* displayName;       ///< Human-readable name shown in logs.
    float       spawnOffsetY;      ///< Vertical offset from player Y when spawning.
    u32         defaultParams;     ///< Parameter flags passed to fopAcM_create.
    s8          argument;          ///< Argument passed to fopAcM_create (e.g. 1 for giant Donketsu Helmasaur).
};

inline const std::unordered_map<std::string, SpawnEntry>& getSpawnTable() {
    static const std::unordered_map<std::string, SpawnEntry> table = {
        // ---- Items / Projectiles --------------------------------------------
        {"spawn_bomb",      {fpcNm_NBOMB_e,        nullptr,    "Bomb",                   20.0f,  0,       0}},
        {"spawn_bombchu",   {fpcNm_NBOMB_e,        nullptr,    "Bombchu",                 0.0f,  1,       0}},

        // ---- Collectibles / Props -------------------------------------------
        {"spawn_rupee",     {fpcNm_ITEM_e,         nullptr,    "Rupee",                  30.0f,  0,       0}},
        {"give_rupee",      {fpcNm_ITEM_e,         nullptr,    "Rupee",                  30.0f,  0,       0}},
        {"spawn_heart",     {fpcNm_ITEM_e,         nullptr,    "Heart",                  30.0f,  1,       0}},
        // spawn_fairy: FIXED — this originally pointed at fpcNm_NPC_FAIRY_e
        // (the Great Fairy NPC, daNpc_Fairy_c), a mislabeling from before this
        // mod ever verified actor identities against source. The real "just a
        // fairy" actor is daObjYOUSEI_c (d_a_obj_yousei.h) — same actor/params
        // as recovery_fairy below (its own heal-on-touch is native, see that
        // entry's comment). See great_fairy below for the actual Great Fairy.
        {"spawn_fairy",     {fpcNm_Obj_Yousei_e,   "Yousei",   "Fairy",                  80.0f,  0xFFFFFFFF, -1}},
        {"spawn_pot",       {fpcNm_Obj_KiPot_e,    "Obj_pot",  "Pot",                     0.0f,  0,       0}},
        // great_fairy: daNpc_Fairy_c (d_a_npc_fairy.h), procName
        // fpcNm_NPC_FAIRY_e, real archive "Fairy". getType()=low byte
        // (0xFF/0x00 both -> the default "type 0" variant); getSwitchBitNo()
        // =top byte, read via fopAcM_isSwitch with no OOB risk for any byte
        // value, 0xFF here by convention. handleGreatFairy() does NOT rely on
        // her own scripted healing (see its comment) — this entry only
        // controls how she looks when spawned.
        {"great_fairy",     {fpcNm_NPC_FAIRY_e,    "Fairy",    "Great Fairy",             0.0f,  0xFF000000,  0}},
        // recovery_fairy: daObjYOUSEI_c (d_a_obj_yousei.h) — the small fairy
        // that flies to Link and heals him on contact via execItemGet(),
        // same native item-get path the game uses everywhere. Unlike
        // great_fairy above, this one genuinely heals in its own code — no
        // manual override needed. procName/params/argument copied verbatim
        // from the game's own spawn call for dItemNo_RECOVERY_FAILY_e
        // (f_op_actor_mng.cpp:1697): fopAcM_create(fpcNm_Obj_Yousei_e,
        // 0xFFFFFFFF, pos, room, angle, scale, -1) — not inferred, the exact
        // params the real game uses whenever a recovery fairy is spawned.
        {"recovery_fairy",  {fpcNm_Obj_Yousei_e,   "Yousei",   "Fairy (Healing)",        80.0f,  0xFFFFFFFF, -1}},

        // ---- Domestic / Harmless Animals -----------------------------------
        {"cucco",           {fpcNm_NI_e,           "Ni",       "Cucco",                  10.0f,  0,       0}},
        {"goat",            {fpcNm_COW_e,          "Cow",      "Ordon Goat",              0.0f,  0,       0}},

        // ---- Standard & Twilight Enemies ------------------------------------
        // `defaultParams` below were reverse-engineered against a previous,
        // confirmed-working mod (tiktok_bridge, dusklight_1.4.1) and its
        // comment: "Defeat/path switch nibbles MUST be 0xFF for crowd spawns,
        // or room switch 0 aborts/glitches create." Cross-checked against
        // daE_OC_c::create() (d_a_e_oc.cpp:2718): bits 16-23 of `parameters`
        // are read as a save-data "defeat switch" number, and bits 24-31 feed
        // a path/variant byte — 0xFF in both is the "none assigned" sentinel.
        // Our old values left those bytes as 0x00, so every spawned actor
        // believed it was tied to defeat-switch/path #0 of whatever room it
        // landed in — a room that, for HiveShock spawns, never authored that
        // switch/path. bokoblin's old value (0x0100) is the exact one
        // implicated in the reproducible bokoblin_army crash.
        {"keese",           {fpcNm_E_BA_e,         "E_ba",     "Keese",                  80.0f,  0xFFFF0F01,  0}},
        {"fire_keese",      {fpcNm_E_BA_e,         "E_fb",     "Fire Keese",             80.0f,  0xFFFF1F01,  0}},
        {"ice_keese",       {fpcNm_E_BA_e,         "E_ib",     "Ice Keese",              80.0f,  0xFFFF2F01,  0}},
        {"guay",            {fpcNm_E_GE_e,         "E_GE",     "Guay",                  100.0f,  0x0000FF01,  0}},
        {"bokoblin",        {fpcNm_E_OC_e,         "E_ocb",    "Bokoblin",                0.0f,  0xFFFF0100,  0}},
        // bulblin: top byte is a defeat switch that daE_RD SETS on death when
        // != 0xFF (d_a_e_rd.cpp:2794) — the old 0x0000 turned on room switch
        // #0 every time one died. 0xF000 = arg2 "none" (normal melee), weapon
        // nibble 0 (club), bits16-23 0xFF = scene_no 0, low byte 0 = normal.
        {"bulblin",         {fpcNm_E_RD_e,         "E_RD",     "Bulblin",                 0.0f,  0xFFFFF000,  0}},
        {"lizalfos",        {fpcNm_E_DN_e,         "E_dn",     "Lizalfos",                0.0f,  0xFF000000,  0}},
        {"chuchu",          {fpcNm_E_SM2_e,        "E_sm2",    "Chuchu",                  0.0f,  0xFF00001A,  0}},
        {"tektite",         {fpcNm_E_TT_e,         "E_TT",     "Tektite",                 0.0f,  0xFF0100FF,  0}},
        {"skulltula",       {fpcNm_E_ST_e,         "E_st",     "Skulltula",              30.0f,  0xFF000002,  0}},
        {"helmasaur",       {fpcNm_E_MM_e,         "E_MM",     "Helmasaur",               0.0f,  0x0000FFC8,  0}},
        {"helmsaurus",      {fpcNm_E_MM_e,         "E_MM",     "Helmsaurus (Capibara)",   0.0f,  0x0000FFC8,  1}},
        // wolfos: bits24-27 == 2 is how the game's own Wolfos pack master
        // spawns an attacker (daE_WW_c::createWolf, d_a_e_ww.cpp:631): it sets
        // field_0x758, so after its appear animation it jumps straight into
        // ATTACK/CHASE. The old value (1) left it idling in ACTION_WAIT
        // forever, waiting for a master that doesn't exist. Rest mirrors
        // createWolf's own layout: 0xF0FF0000|0xF000 filler, move range 30
        // (0x1E << 4), type 0 (normal, 100 HP).
        {"wolfos",          {fpcNm_E_WW_e,         "E_WW",     "White Wolfos",            0.0f,  0xF2FFF1E0,  0}},
        // gibdo: low byte = defeat switch (0xFF none), bits8-15 = start awake,
        // bits16-23 = mSwbit2, a "wake/fight only once this switch is on" gate
        // (d_a_e_gi.cpp:446) — 0xFF disables it. The old 0x00 there made it
        // wait for room switch #0, which is never set, so it never attacked.
        {"gibdo",           {fpcNm_E_GI_e,         "E_GI",     "Gibdo",                   0.0f,  0xFFFF01FF,  0}},
        // poe / espectro: daE_HP_c ("general-purpose Poe", E_HP) - the small, non-humanoid
        // ghost with the lantern. Deliberately NOT E_PO (the larger humanoid Poe). The
        // command keeps the name "poe" for existing profiles; "espectro" is an alias.
        // params: low byte arg0 (0 = normal; 2 = hovering variant), bits8-15 defeat
        // switch, bits16-23 activation range (0xFF = default 2000), bits24-31 an
        // "enabled" switch - 0xFF everywhere means "none / defaults". mNight comes
        // from the creation angle's z (0 here), so it is always active, day or night.
        {"poe",             {fpcNm_E_HP_e,         "E_HP",     "Poe (Espectro)",         40.0f,  0xFFFFFF00,  0}},
        {"espectro",        {fpcNm_E_HP_e,         "E_HP",     "Poe (Espectro)",         40.0f,  0xFFFFFF00,  0}},
        {"keese_twilight",  {fpcNm_E_YK_e,         "E_YK",     "Twilight Keese",         80.0f,  0xFF000000,  0}},
        {"shadow_beast",    {fpcNm_E_S1_e,         "E_S2",     "Shadow Beast",            0.0f,  0xFF00FFF0,  0}},
        {"twilight_insect", {fpcNm_E_YM_e,         "E_TM",     "Twilight Insect",         0.0f,  0xFF000000,  0}},
        {"twilight_vermin", {fpcNm_E_YG_e,         "E_YG",     "Twilight Vermin",         0.0f,  0xFFFF0000,  0}},
        {"poison_mite",     {fpcNm_E_BUG_e,        "E_bug",    "Poison Mite",             0.0f,  0xFFFF0F03,  0}},
        {"bee",             {fpcNm_E_BEE_e,        "E_bee",    "Bee",                    60.0f,  0x0000,      0}}, // no reference value found (old mod's "bee_swarm" is a different actor, E_nest) — unverified

        // ---- Newly added (2026) — params derived directly from each actor's
        // own create() in src/d/actor/*.cpp, not copied from a reference mod
        // (none of these were in tiktok_bridge). See chat history for the
        // per-actor reasoning.
        //
        // darknut: mType(bits8-15)=1 selects the "B_tnp2" variant and checks
        // a defeat switch (mSwitchNo, low byte) with no 0xFF-sentinel guard —
        // 0xFF there is just a harmless, almost-certainly-unset switch #255,
        // not a magic value. mType=0 was avoided: it requires
        // dComIfGs_isStageMiddleBoss() to be true or create() aborts, which
        // would silently no-op the spawn in an ordinary field room.
        {"darknut",         {fpcNm_B_TN_e,         "B_tn",     "Darknut",                 0.0f,  0xFFFF01FF,  0}},
        // puppet_zelda: only the top byte (bits24-31) is read, as a defeat
        // switch guarded by the standard 0xFF sentinel. Her native defeat
        // path is a boss cutscene that ends in dStage_changeScene() — the
        // mod suppresses it and kills her like a normal enemy instead, see
        // EffectDispatcher::tamePuppetZelda().
        {"puppet_zelda",    {fpcNm_E_HZELDA_e,     "Hzelda",   "Puppet Zelda",            0.0f,  0xFF000000,  0}},
        // freezard: low byte 0/1 = normal battle Freezard (10/11 = inert
        // decorative/caged variants seen in Snowpeak's Spiral Room — avoided
        // here since they never enable combat). bits8-15 (swBit0) IS guarded
        // by a 0xFF sentinel, so 0xFF there really does mean "no switch".
        {"freezard",        {fpcNm_E_FB_e,         "E_FL",     "Freezard",                0.0f,  0xFFFFFF00,  0}},
        // leever: unlike every other actor here, this one is normally spawned
        // as its own coordinator — low byte 0 = "parent" mode, which reads a
        // child count from bits8-15 (here 3) and spawns that many real,
        // fully-initialized child Leevers via fopAcM_createChild() itself;
        // the parent process stays invisible/harmless. Its defeat-switch
        // (unlike every other actor here) is read from the creation angle's
        // X component, not from params — our generic spawnActor() always
        // passes angle.x=0, which reads as switch #0 (not a 0xFF sentinel in
        // this actor). Low, not zero, risk: only misbehaves if room switch
        // #0 happens to already be set for something unrelated.
        {"leever",          {fpcNm_E_RB_e,         "E_rb",     "Leever",                  0.0f,  0x00000300,  0}},

        // ---- Legacy / Alias entries -----------------------------------------
        {"spawn_keese",     {fpcNm_E_BA_e,         "E_ba",     "Keese",                 150.0f,  0xFFFF0F01,  0}},
        {"spawn_bokoblin",  {fpcNm_E_OC_e,         "E_ocb",    "Bokoblin",                0.0f,  0xFFFF0100,  0}},
        {"spawn_bulblin",   {fpcNm_E_RD_e,         "E_RD",     "Bulblin",                 0.0f,  0xFFFFF000,  0}}, // see bulblin above
        {"spawn_lizalfos",  {fpcNm_E_DN_e,         "E_dn",     "Lizalfos",                0.0f,  0xFF000000,  0}},
        {"spawn_skulltula", {fpcNm_E_ST_e,         "E_st",     "Skulltula",              50.0f,  0xFF000002,  0}},
    };
    return table;
}

inline const SpawnEntry* findSpawnEntry(const std::string& commandId) {
    const auto& table = getSpawnTable();
    auto it = table.find(commandId);
    return (it != table.end()) ? &it->second : nullptr;
}

inline const std::vector<std::string>& getRandomEnemyIds() {
    static const std::vector<std::string> enemyIds = {
        "bokoblin", "keese", "fire_keese", "ice_keese", "guay",
        "lizalfos", "chuchu", "tektite", "skulltula", "helmasaur",
        "helmsaurus", "wolfos", "poe", "gibdo", "keese_twilight",
        "shadow_beast", "twilight_insect", "twilight_vermin", "poison_mite"
    };
    return enemyIds;
}

} // namespace hiveshock
