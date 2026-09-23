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
