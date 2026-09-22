/*
 * mod-worldscale - world-wide (open world) level scaling for AzerothCore 3.3.5a
 *
 * Instances are intentionally left alone: mod-autobalance owns dungeon/raid
 * scaling. This module scales the *open world* per player, so a level 20
 * character and a level 80 character can fight the same creature and both get
 * a level-appropriate fight.
 *
 * Creature stats are never mutated (that would be shared between all players
 * who can see the creature). Instead the damage exchanged between a player and
 * a creature is scaled in both directions:
 *
 *   creature -> player : creature hits like a creature of the player's level
 *   player -> creature : creature takes damage as if it had the health pool of
 *                        a creature of the player's level
 *
 * The multipliers come from the core's own creature_classlevelstats table, so
 * they follow the same stat curve the core uses when it spawns a creature.
 */

#include "Config.h"
#include "AllCreatureScript.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "UpdateFields.h"
#include "World.h"

#include <algorithm>
#include <cmath>

namespace
{
    struct WorldScaleConfig
    {
        bool  Enable            = true;
        bool  ScaleUp           = true;   // pull weaker (lower level) creatures up to the player
        bool  ScaleDown         = true;   // pull stronger (higher level) creatures down to the player
        int32 LevelDelta        = 0;      // target creature level = player level + this
        bool  AffectElites      = true;
        bool  AffectWorldBosses = false;
        bool  ScaleXP           = true;
        bool  PresentLevel      = true;   // show scaled-up creatures at the observer's level
        uint32 AggroLevelsBelow = 0;      // aggro as if this many levels below the player
        bool  ScaleQuests       = true;   // quest experience, and how quest levels present
        float MinMultiplier     = 0.05f;
        float MaxMultiplier     = 20.0f;
        uint8 MinPlayerLevel    = 1;
    };

    WorldScaleConfig cfg;

    struct Multipliers
    {
        float creatureDamage = 1.0f;  // applied to damage dealt by the creature
        float playerDamage   = 1.0f;  // applied to damage dealt to the creature
    };

    void LoadConfig()
    {
        cfg.Enable            = sConfigMgr->GetOption<bool>("WorldScale.Enable", true);
        cfg.ScaleUp           = sConfigMgr->GetOption<bool>("WorldScale.ScaleUp", true);
        cfg.ScaleDown         = sConfigMgr->GetOption<bool>("WorldScale.ScaleDown", true);
        cfg.LevelDelta        = sConfigMgr->GetOption<int32>("WorldScale.LevelDelta", 0);
        cfg.AffectElites      = sConfigMgr->GetOption<bool>("WorldScale.AffectElites", true);
        cfg.AffectWorldBosses = sConfigMgr->GetOption<bool>("WorldScale.AffectWorldBosses", false);
        cfg.ScaleXP           = sConfigMgr->GetOption<bool>("WorldScale.ScaleXP", true);
        cfg.PresentLevel      = sConfigMgr->GetOption<bool>("WorldScale.PresentLevel", true);
        cfg.AggroLevelsBelow  = sConfigMgr->GetOption<uint32>("WorldScale.Aggro.LevelsBelow", 0);
        cfg.ScaleQuests       = sConfigMgr->GetOption<bool>("WorldScale.ScaleQuests", true);
        cfg.MinMultiplier     = sConfigMgr->GetOption<float>("WorldScale.MinMultiplier", 0.05f);
        cfg.MaxMultiplier     = sConfigMgr->GetOption<float>("WorldScale.MaxMultiplier", 20.0f);
        cfg.MinPlayerLevel    = uint8(sConfigMgr->GetOption<uint32>("WorldScale.MinPlayerLevel", 1));

        cfg.LevelDelta = std::clamp<int32>(cfg.LevelDelta, -60, 10);
        if (cfg.MinMultiplier <= 0.0f)
            cfg.MinMultiplier = 0.05f;
        if (cfg.MaxMultiplier < cfg.MinMultiplier)
            cfg.MaxMultiplier = cfg.MinMultiplier;

        LOG_INFO("module", "mod-worldscale: {} (delta {}, up {}, down {}, xp {}, present level {})",
            cfg.Enable ? "enabled" : "disabled", cfg.LevelDelta,
            cfg.ScaleUp, cfg.ScaleDown, cfg.ScaleXP, cfg.PresentLevel);
    }

    uint8 TargetLevelFor(Player const* player)
    {
        int32 maxLevel = int32(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        int32 level    = int32(player->GetLevel()) + cfg.LevelDelta;
        return uint8(std::clamp<int32>(level, 1, maxLevel));
    }

    // What level a particular observer should *see* on a creature. 0 means
    // "send the real value". Only the scale-up direction is mirrored: a creature
    // above the player keeps its true, higher level so it still reads as
    // dangerous, which matters now that ScaleDown is off.
    uint8 PresentedLevelFor(Creature const* creature, Player const* observer);

    bool IsScalableCreature(Creature const* creature)
    {
        if (!creature)
            return false;

        Map* map = creature->GetMap();
        // Dungeons/raids belong to mod-autobalance, PvP instances are never touched.
        if (!map || map->IsDungeon() || map->IsBattlegroundOrArena())
            return false;

        // Anything a player owns fights on the player's side of the equation.
        if (creature->GetCharmerOrOwnerPlayerOrPlayerItself())
            return false;

        if (creature->IsTotem() || creature->IsTrigger() || creature->IsCritter())
            return false;

        if (creature->isWorldBoss())
            return cfg.AffectWorldBosses;

        if (!cfg.AffectElites)
        {
            uint32 const rank = creature->GetCreatureTemplate()->rank;
            if (rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE)
                return false;
        }

        return true;
    }

    // Quest::XPValue(), recomputed as though the quest sat at questLevel.
    // Mirrors the core (QuestDef.cpp) including its rounding steps, so the two
    // agree for the level the core would itself have used.
    uint32 QuestXPAtLevel(Quest const* quest, uint8 playerLevel, int32 questLevel)
    {
        QuestXPEntry const* entry = sQuestXPStore.LookupEntry(uint32(questLevel));
        if (!entry)
            return 0;

        int32 diffFactor = std::clamp(2 * (questLevel - int32(playerLevel)) + 20, 1, 10);
        uint32 xp = uint32(diffFactor) * entry->Exp[quest->GetXPId()] / 10;

        if (xp <= 100)
            xp = 5 * ((xp + 2) / 5);
        else if (xp <= 500)
            xp = 10 * ((xp + 5) / 10);
        else if (xp <= 1000)
            xp = 25 * ((xp + 12) / 25);
        else
            xp = 50 * ((xp + 25) / 50);

        return xp;
    }

    uint8 PresentedLevelFor(Creature const* creature, Player const* observer)
    {
        if (!cfg.Enable || !cfg.PresentLevel || !cfg.ScaleUp || !creature || !observer)
            return 0;

        if (!IsScalableCreature(creature))
            return 0;

        if (observer->GetLevel() < cfg.MinPlayerLevel)
            return 0;

        uint8 const targetLevel = TargetLevelFor(observer);

        // Only creatures being pulled *up* are relabelled. One above the player
        // is genuinely harder than it looks and keeps its real level.
        if (creature->GetLevel() >= targetLevel)
            return 0;

        return targetLevel;
    }

    bool GetMultipliers(Creature* creature, Player* player, Multipliers& out)
    {
        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        if (!cinfo)
            return false;

        if (player->GetLevel() < cfg.MinPlayerLevel)
            return false;

        uint8 const creatureLevel = creature->GetLevel();
        uint8 const targetLevel   = TargetLevelFor(player);

        if (creatureLevel == targetLevel)
            return false;
        if (creatureLevel < targetLevel && !cfg.ScaleUp)
            return false;
        if (creatureLevel > targetLevel && !cfg.ScaleDown)
            return false;

        uint32 const expansion = std::min<uint32>(cinfo->expansion, MAX_EXPANSIONS - 1);

        CreatureBaseStats const* current = sObjectMgr->GetCreatureBaseStats(creatureLevel, cinfo->unit_class);
        CreatureBaseStats const* target  = sObjectMgr->GetCreatureBaseStats(targetLevel, cinfo->unit_class);
        if (!current || !target)
            return false;

        float const currentHealth = float(current->BaseHealth[expansion]);
        float const targetHealth  = float(target->BaseHealth[expansion]);
        float const currentDamage = current->BaseDamage[expansion];
        float const targetDamage  = target->BaseDamage[expansion];

        if (currentHealth <= 0.0f || targetHealth <= 0.0f || currentDamage <= 0.0f || targetDamage <= 0.0f)
            return false;

        out.creatureDamage = std::clamp(targetDamage / currentDamage, cfg.MinMultiplier, cfg.MaxMultiplier);
        out.playerDamage   = std::clamp(currentHealth / targetHealth, cfg.MinMultiplier, cfg.MaxMultiplier);
        return true;
    }

    // Scales one damage event. Works for both the uint32 and int32 damage hooks.
    template <typename T>
    void ScaleDamage(Unit* target, Unit* attacker, T& damage)
    {
        if (!cfg.Enable || !target || !attacker || damage <= 0)
            return;

        Player* attackerPlayer = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        Player* targetPlayer   = target->GetCharmerOrOwnerPlayerOrPlayerItself();

        // Player vs player (and creature vs creature) is left untouched.
        if (!!attackerPlayer == !!targetPlayer)
            return;

        Multipliers multipliers;

        if (targetPlayer)
        {
            Creature* creature = attacker->ToCreature();
            if (!IsScalableCreature(creature) || !GetMultipliers(creature, targetPlayer, multipliers))
                return;

            damage = T(std::max(1.0f, float(damage) * multipliers.creatureDamage));
        }
        else
        {
            Creature* creature = target->ToCreature();
            if (!IsScalableCreature(creature) || !GetMultipliers(creature, attackerPlayer, multipliers))
                return;

            damage = T(std::max(1.0f, float(damage) * multipliers.playerDamage));
        }
    }

    // Undoes this module's own distortion of a rage figure.
    //
    // Rage income is a function of damage (Unit::RewardRage), so scaling the
    // damage of an exchange scales the rage with it. Cutting a player's damage
    // to 22% against a level 2 creature cut their rage to 22% as well, and the
    // formula's own clamp
    //
    //     addRage = std::min(addRage, rageFromDamageDealt * 2.0f);
    //
    // then removed the weapon speed term, which is the part that does not
    // depend on damage - so rage stopped growing with the length of the fight
    // and became purely proportional to the creature's (unchanged, tiny)
    // health pool. A level 12 warrior earned about 14 rage for an entire kill,
    // less than one Heroic Strike, over a fight lasting twenty seconds.
    //
    // The fix is symmetric, because both halves of the exchange were scaled:
    // rage for damage dealt is divided by the multiplier that reduced it, and
    // rage for damage taken is divided by the multiplier that inflated it.
    // Rage then behaves exactly as it would in an at-level fight, in both
    // directions, while the damage itself stays scaled.
    void UnscaleRage(Unit* unit, Unit* other, uint32& damage, bool attacker)
    {
        if (!cfg.Enable || !unit || !other || !damage)
            return;

        Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!player)
            return;

        Creature* creature = other->ToCreature();
        if (!IsScalableCreature(creature))
            return;

        Multipliers multipliers;
        if (!GetMultipliers(creature, player, multipliers))
            return;

        // attacker: the player dealt damage that was multiplied by
        // playerDamage. Otherwise the player took damage that was multiplied
        // by creatureDamage.
        float const applied = attacker ? multipliers.playerDamage : multipliers.creatureDamage;
        if (applied <= 0.0f)
            return;

        uint32 const before = damage;
        damage = uint32(std::max(1.0f, float(damage) / applied));

        // Debug only: "Logger.module" at level 5 shows the correction happening
        // per hit, which is the only way to watch it without a client.
        LOG_DEBUG("module", "mod-worldscale: rage for {} {} {} - damage {} -> {} (x1/{:.3f})",
            player->GetName(), attacker ? "dealing to" : "taking from",
            creature->GetName(), before, damage, applied);
    }
}

class WorldScale_WorldScript : public WorldScript
{
public:
    WorldScale_WorldScript() : WorldScript("WorldScale_WorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class WorldScale_UnitScript : public UnitScript
{
public:
    WorldScale_UnitScript() : UnitScript("WorldScale_UnitScript", true,
        {
            UNITHOOK_MODIFY_MELEE_DAMAGE,
            UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
            UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
            UNITHOOK_SHOULD_TRACK_VALUES_UPDATE_POS_BY_INDEX,
            UNITHOOK_ON_PATCH_VALUES_UPDATE,
            UNITHOOK_ON_UNIT_GET_LEVEL_FOR_TARGET,
            UNITHOOK_ON_UNIT_REWARD_RAGE
        }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        ScaleDamage(target, attacker, damage);
    }

    void OnUnitRewardRage(Unit* unit, Unit* other, uint32& damage, bool attacker) override
    {
        UnscaleRage(unit, other, damage, attacker);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        ScaleDamage(target, attacker, damage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        ScaleDamage(target, attacker, damage);
    }

    // A creature scaled up to the player's level still *displayed* its own low
    // level, so a mob hitting like level 60 showed a grey name - the display was
    // the only part of the illusion telling the player the wrong thing.
    //
    // UNIT_FIELD_LEVEL is an update field, so this is purely what gets written
    // into the packet: the creature's real level, its combat maths, its XP and
    // everything the server computes are untouched. Because the core builds
    // update blocks per observer and patches tracked fields in afterwards, each
    // player can be sent a different number for the same creature - which is
    // essential here, since 500 bots of every level share the same world and a
    // single global level could not be right for all of them.
    //
    // Asking the core to record where UNIT_FIELD_LEVEL landed in the cached
    // buffer. It only lands there when the field is actually being sent, which
    // in practice means the create-object block when the creature comes into
    // view; the client caches it from there.
    bool ShouldTrackValuesUpdatePosByIndex(Unit const* unit, uint8 /*updateType*/, uint16 index) override
    {
        return cfg.Enable && cfg.PresentLevel && index == UNIT_FIELD_LEVEL && unit && unit->IsCreature();
    }

    void OnPatchValuesUpdate(Unit const* unit, ByteBuffer& valuesUpdateBuf,
                             BuildValuesCachePosPointers& posPointers, Player* target) override
    {
        if (!cfg.Enable || !cfg.PresentLevel || !unit || !target)
            return;

        auto const pos = posPointers.other.find(UNIT_FIELD_LEVEL);
        if (pos == posPointers.other.end())
        {
            if (target->GetSession() && !target->GetSession()->IsBot() && unit->IsCreature())
                LOG_DEBUG("module", "mod-worldscale: present-level: {} sees {} but UNIT_FIELD_LEVEL is not tracked in this block",
                    target->GetName(), unit->GetName());
            return;
        }

        uint8 const presented = PresentedLevelFor(unit->ToCreature(), target);
        if (target->GetSession() && !target->GetSession()->IsBot())
            LOG_DEBUG("module", "mod-worldscale: present-level: {} (L{}) sees {} (L{}) -> {}",
                target->GetName(), target->GetLevel(), unit->GetName(), unit->GetLevel(), presented);
        if (!presented)
            return;

        valuesUpdateBuf.put<uint32>(pos->second, uint32(presented));
    }

    // The level this creature reports *to this observer*.
    // Creature::getLevelForTarget feeds aggro range and attack distance, the
    // melee skill gap behind miss/dodge/parry/block (through GetUnitMeleeSkill
    // and GetMaxSkillValueForLevel) and the glancing/crushing gates - so
    // reporting the scaled level makes all of those behave as though the
    // creature really were the player's level.
    //
    // Experience is deliberately unaffected: Acore::XP::Gain reads
    // unit->GetLevel(), the real level, so the kill XP scaling in this module
    // cannot double up with this.
    void OnUnitGetLevelForTarget(Unit const* unit, WorldObject const* target, uint8& level) override
    {
        if (!unit || !target)
            return;

        Player const* observer = target->ToPlayer();
        if (!observer)
            return;

        if (uint8 const scaled = PresentedLevelFor(unit->ToCreature(), observer))
            level = scaled;
    }
};

class WorldScale_CreatureScript : public AllCreatureScript
{
public:
    WorldScale_CreatureScript() : AllCreatureScript("WorldScale_CreatureScript") { }

    // Presenting a scaled-up creature at the player's level made it as alert
    // as an at-level one: 20 yards, where a creature ten levels down would
    // have noticed the player at 10 and one fifteen down at the 5-yard floor.
    // Fights at your level are the point; being lunged at from twenty yards
    // by everything in the zone is a side effect. This pulls the aggro
    // radius back by WorldScale.Aggro.LevelsBelow yards - the formula's own
    // unit, one yard per level - for scaled-up creatures only, and only for
    // aggro. Avoidance, hit chance and experience still see the presented
    // level. The core's 5-yard floor is applied after this.
    void OnCreatureGetAggroRange(Creature const* creature, Unit const* target, float& range) override
    {
        if (!cfg.AggroLevelsBelow || !creature || !target)
            return;

        Player const* observer = target->ToPlayer();
        if (!observer)
            return;

        if (!PresentedLevelFor(creature, observer))
            return;

        range -= float(cfg.AggroLevelsBelow);
    }
};

class WorldScale_PlayerScript : public PlayerScript
{
public:
    WorldScale_PlayerScript() : PlayerScript("WorldScale_PlayerScript",
        {
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
            PLAYERHOOK_ON_QUEST_COMPUTE_LEVEL
        }) { }

    // A creature that has been scaled up to the player's level should also pay
    // out experience for that level, otherwise low level zones stay pointless.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 /*xpSource*/) override
    {
        if (!cfg.Enable || !cfg.ScaleXP || !player || !victim)
            return;

        Creature* creature = victim->ToCreature();
        if (!IsScalableCreature(creature))
            return;

        uint8 const creatureLevel = creature->GetLevel();
        uint8 const targetLevel   = TargetLevelFor(player);
        if (creatureLevel >= targetLevel)
            return;

        ContentLevels const content = GetContentLevelsForMapAndZone(creature->GetMapId(), creature->GetZoneId());
        uint32 const baseCurrent = Acore::XP::BaseGain(player->GetLevel(), creatureLevel, content);
        uint32 const baseTarget  = Acore::XP::BaseGain(player->GetLevel(), targetLevel, content);

        if (!baseTarget)
            return;

        // Grey creatures never arrive here at all - see OnPlayerCreatureKill.
        if (!baseCurrent || !amount)
            return;

        // Keep whatever group/rate modifiers the core already applied.
        amount = uint32(float(amount) * (float(baseTarget) / float(baseCurrent)));
    }

    // A creature that is grey for the player pays out nothing, and the core
    // never even offers it for scaling: KillRewarder::_RewardXP computes the
    // experience first and only then calls the hook, inside "if (xp)" -
    //
    //     if (xp)
    //     {
    //         xp *= player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT);
    //         sScriptMgr->OnPlayerGiveXP(player, xp, _victim, XPSOURCE_KILL);
    //         player->GiveXP(xp, _victim, _groupRate);
    //     }
    //
    // so for a grey kill OnPlayerGiveXP is skipped entirely. This hook fires
    // unconditionally from Unit::Kill instead, and awards what the creature
    // would have been worth at the level it was scaled to. Player::GiveXP does
    // not itself call OnPlayerGiveXP, so there is no risk of re-entering the
    // scaling above.
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!cfg.Enable || !cfg.ScaleXP || !killer || !killed)
            return;

        if (!IsScalableCreature(killed))
            return;

        if (killer->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return;

        uint8 const creatureLevel = killed->GetLevel();
        uint8 const targetLevel   = TargetLevelFor(killer);
        if (creatureLevel >= targetLevel)
            return;

        ContentLevels const content = GetContentLevelsForMapAndZone(killed->GetMapId(), killed->GetZoneId());

        // Only the grey case belongs here. Anything the core still values goes
        // through OnPlayerGiveXP above, and handling it twice would double pay.
        if (Acore::XP::BaseGain(killer->GetLevel(), creatureLevel, content))
            return;

        uint32 const baseTarget = Acore::XP::BaseGain(killer->GetLevel(), targetLevel, content);
        if (!baseTarget)
            return;

        killer->GiveXP(uint32(float(baseTarget) * sWorld->getRate(RATE_XP_KILL)), killed);
    }

    // Quest experience is measured against the quest's own level, so a level 10
    // quest pays a level 60 character a rounding error. Recomputing it at the
    // player's level fixes that, but only ever upwards: a quest *above* the
    // player is worth more measured at its own level, and taking the better of
    // the two means this can never reduce a reward.
    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& xpValue) override
    {
        if (!cfg.Enable || !cfg.ScaleQuests || !player || !quest)
            return;

        int32 const questLevel = quest->GetQuestLevel();
        if (questLevel <= 0)
            return;   // already dynamic; the core measures it at the player's level

        uint8 const playerLevel = player->GetLevel();
        if (questLevel >= int32(playerLevel))
            return;

        uint32 const atPlayerLevel = QuestXPAtLevel(quest, playerLevel, int32(playerLevel));
        if (atPlayerLevel > xpValue)
            xpValue = atPlayerLevel;
    }

    // -1 is the client's "use my own level" sentinel, resolved when the quest is
    // drawn rather than when questcache.wdb is written, so it never goes stale.
    // Reporting it for a quest at or below the player stops low level quests
    // presenting as trivial; a quest above the player keeps its real level so it
    // still reads as ahead of them.
    void OnPlayerQuestComputeLevel(Player* player, Quest const* quest, int32& level) override
    {
        if (!cfg.Enable || !cfg.ScaleQuests || !player || !quest)
            return;

        int32 const questLevel = quest->GetQuestLevel();
        if (questLevel <= 0)
            return;

        if (questLevel <= int32(player->GetLevel()))
            level = -1;
    }
};

void AddWorldScaleScripts()
{
    new WorldScale_WorldScript();
    new WorldScale_UnitScript();
    new WorldScale_CreatureScript();
    new WorldScale_PlayerScript();
}
