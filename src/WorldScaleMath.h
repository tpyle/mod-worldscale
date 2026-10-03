/*
 * mod-worldscale - the arithmetic, separated from the world.
 *
 * Everything in this header is a pure function of numbers: no Player, no
 * Creature, no config singleton, no database. That is the point. The hooks in
 * mod_worldscale.cpp do the part that needs the world - deciding whether a
 * creature is scalable, reading its base stats, patching an update field - and
 * hand the numbers to these functions, which decide what the numbers should
 * be. The decisions are then testable on their own (tests/), which matters
 * because they are where the quiet mistakes live: a clamp applied to the wrong
 * side of a ratio, a level delta that escapes the level cap, an unscale that
 * does not undo its scale.
 */

#ifndef MOD_WORLDSCALE_MATH_H
#define MOD_WORLDSCALE_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WorldScaleMath
{
    // The core's uint8/int32/uint32 are plain typedefs of these
    // (src/common/Define.h), so the module passes its own types straight in.
    // Spelling them out here rather than including Define.h is what lets this
    // header - and therefore its tests - be compiled with nothing but a
    // standard library, which is why CI can run the tests without AzerothCore.
    using u8  = std::uint8_t;
    using u32 = std::uint32_t;
    using i32 = std::int32_t;

    // The two multipliers for one creature/observer pair.
    struct Multipliers
    {
        float creatureDamage = 1.0f;  // applied to damage dealt by the creature
        float playerDamage   = 1.0f;  // applied to damage dealt to the creature
    };

    // Which maps this module scales on.
    //
    // PvP instances are never touched: a battleground is already a level
    // bracket and an arena is meant to be symmetrical. Dungeons and raids are
    // switches because something else may own them - mod-autobalance scales
    // instances by group size and has its own level scaling, and two systems
    // both multiplying a creature's health and damage compound into nonsense.
    // Turn one off before turning the other on.
    inline bool ScalesOnThisMap(bool isBattlegroundOrArena, bool isRaid, bool isDungeon,
                                bool scaleDungeons, bool scaleRaids)
    {
        if (isBattlegroundOrArena)
            return false;

        // A raid is also a dungeon as far as Map is concerned, so it has to be
        // asked about first or the raid switch would never be reached.
        if (isRaid)
            return scaleRaids;

        if (isDungeon)
            return scaleDungeons;

        return true;   // the open world
    }

    // The level a creature should be pulled to for this observer: the player's
    // level plus the configured delta, kept inside [1, maxPlayerLevel].
    // The clamp is what stops a positive delta pushing a creature past the
    // level cap, where the creature base stat table has no rows.
    inline u8 TargetLevel(u8 playerLevel, i32 levelDelta, u32 maxPlayerLevel)
    {
        i32 const level = i32(playerLevel) + levelDelta;
        return u8(std::clamp<i32>(level, 1, i32(maxPlayerLevel)));
    }

    // Whether a creature at creatureLevel should be scaled at all for an
    // observer whose target level is targetLevel. Equal levels are never
    // scaled - not because it would be wrong, but because it would be a
    // multiplication by one on every damage event in the world.
    inline bool ShouldScale(u8 creatureLevel, u8 targetLevel, u8 playerLevel,
                            u8 minPlayerLevel, bool scaleUp, bool scaleDown)
    {
        if (playerLevel < minPlayerLevel)
            return false;
        if (creatureLevel == targetLevel)
            return false;
        if (creatureLevel < targetLevel)
            return scaleUp;
        return scaleDown;
    }

    // The multipliers, from the creature's own base stats at its real level
    // and at the level it is being presented as.
    //
    // creatureDamage scales what the creature deals: the ratio of the damage a
    // creature of the target level would deal to what this one deals.
    // playerDamage scales what the creature takes, and deliberately uses the
    // *health* ratio the other way round rather than a damage ratio: the
    // creature keeps its own health pool, so to make the fight last as long as
    // it would against a creature of the target level, incoming damage is
    // divided by how much more health such a creature would have.
    //
    // Returns false when a ratio cannot be formed, which leaves the caller's
    // multipliers untouched at 1.0 rather than inventing a number.
    inline bool ComputeMultipliers(float currentHealth, float targetHealth,
                                   float currentDamage, float targetDamage,
                                   float minMultiplier, float maxMultiplier,
                                   Multipliers& out)
    {
        if (currentHealth <= 0.0f || targetHealth <= 0.0f || currentDamage <= 0.0f || targetDamage <= 0.0f)
            return false;

        out.creatureDamage = std::clamp(targetDamage / currentDamage, minMultiplier, maxMultiplier);
        out.playerDamage   = std::clamp(currentHealth / targetHealth, minMultiplier, maxMultiplier);
        return true;
    }

    // The level to show this observer, or 0 for "send the real one". Only the
    // scale-up direction is relabelled: a creature above the player is
    // genuinely harder than it looks and keeps its own level.
    inline u8 PresentedLevel(u8 creatureLevel, u8 targetLevel, bool presentLevel, bool scaleUp)
    {
        if (!presentLevel || !scaleUp)
            return 0;
        if (creatureLevel >= targetLevel)
            return 0;
        return targetLevel;
    }

    // The skill a corpse of this level demands to be skinned, as the core
    // works it out in Spell::CheckCast for SPELL_EFFECT_SKINNING:
    //
    //     ReqValue = (skillValue < 100 ? (TargetLevel - 10) * 10 : TargetLevel * 5)
    //
    // Replicated rather than approximated, because it decides whether telling
    // the truth about a corpse's level would actually let the cast through.
    // Below level 10 the first branch goes negative, which reads as no
    // requirement at all.
    inline u32 SkinningRequirement(u8 level, i32 skillValue)
    {
        i32 const required = skillValue < 100 ? (i32(level) - 10) * 10 : i32(level) * 5;
        return required < 0 ? 0u : u32(required);
    }

    // Whether telling one observer a corpse's real level is what makes the
    // difference between being able to skin it and not.
    //
    // Three things have to hold: they have some of the skill the corpse asks
    // for, the level they are being shown puts it out of reach, and the real
    // level does not. The last is the point - if they could not skin it at its
    // true level either then the lie is not what is stopping them, and
    // renumbering the corpse in front of them buys nothing.
    inline bool TruthWouldAllowSkinning(u8 presentedLevel, u8 realLevel, i32 skillValue)
    {
        if (presentedLevel <= realLevel)
            return false;           // the scaling is not making it harder

        if (skillValue <= 0)
            return false;           // they cannot skin anything at all

        return SkinningRequirement(presentedLevel, skillValue) > u32(skillValue)
            && SkinningRequirement(realLevel, skillValue) <= u32(skillValue);
    }

    // Aggro range for a creature that is being presented at the player's
    // level. Such a creature would otherwise aggro from the range its real
    // level implies - the core widens aggro range by the level gap - so the
    // configured number of levels is taken back off. Never below zero; the
    // core applies its own floor afterwards.
    inline float AggroRange(float range, u32 levelsBelow)
    {
        return std::max(0.0f, range - float(levelsBelow));
    }

    // Undo a scaling that has already been applied to a damage figure, for the
    // benefit of a formula that must see the raw number (the rage formula
    // reads damage dealt and taken). Returns the damage unchanged when the
    // multiplier is not usable, so a bad multiplier cannot zero someone's
    // rage, and never returns 0 for a non-zero hit: a hit that landed should
    // be worth something however hard it was scaled down.
    inline u32 Unscale(u32 damage, float applied)
    {
        if (applied <= 0.0f)
            return damage;

        return u32(std::max(1.0f, float(damage) / applied));
    }

    // How much of the way from the unscaled reward to the scaled one to
    // actually pay. Used for both quest and kill experience, each with its own
    // fraction.
    //
    // Scaling all the way to the player's level is a lot: an old quest can be
    // worth ten times its written reward, and a grey creature goes from
    // nothing to a full kill's worth. That makes levelling through old content
    // faster than intended rather than merely worthwhile. A fraction of the
    // *difference* keeps the direction - never less than the content was worth
    // unscaled - while choosing how far to go.
    //
    // 1.0 is full scaling, 0.0 is stock behaviour. For quests the figure is
    // what the client is told as well as what is granted (the server sends the
    // computed reward in the quest details, offer-reward and turn-in paths,
    // all three of which run through the same hook), so the quest log and the
    // experience gained cannot disagree.
    inline u32 BlendXP(u32 original, u32 scaled, float fraction)
    {
        if (scaled <= original)
            return original;

        fraction = std::clamp(fraction, 0.0f, 1.0f);

        double const extra = double(scaled - original) * double(fraction);
        return original + u32(std::lround(extra));
    }

    // Quest::XPValue() recomputed as though the quest sat at questLevel,
    // mirroring the core's rounding steps exactly (QuestDef.cpp) so that the
    // two agree for the level the core would itself have used. baseXp is the
    // QuestXP.dbc row for questLevel, in the quest's own XP slot.
    inline u32 QuestXP(u32 baseXp, u8 playerLevel, i32 questLevel)
    {
        i32 const diffFactor = std::clamp(2 * (questLevel - i32(playerLevel)) + 20, 1, 10);

        u32 xp = u32(diffFactor) * baseXp / 10;

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
}

#endif // MOD_WORLDSCALE_MATH_H
