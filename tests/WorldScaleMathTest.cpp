/*
 * Tests for the mod-worldscale arithmetic (src/WorldScaleMath.h).
 *
 * These exercise the decisions, not the plumbing: what level a creature is
 * pulled to, whether it is pulled at all, what the two damage multipliers
 * come out as, what level the client is told, and what the rage formula is
 * handed back. No Player, no Creature, no database - which is the whole
 * reason the arithmetic was separated from the hooks.
 */

#include "WorldScaleMath.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace WorldScaleMath;

namespace
{
    constexpr std::uint32_t MAX_LEVEL = 80;
    constexpr float  MIN_MULT  = 0.05f;
    constexpr float  MAX_MULT  = 20.0f;
}

// ------------------------------------------------------------ ScalesOnThisMap

TEST(WorldScaleMap, TheOpenWorldIsAlwaysScaled)
{
    EXPECT_TRUE(ScalesOnThisMap(/*bg*/ false, /*raid*/ false, /*dungeon*/ false,
                                /*scaleDungeons*/ false, /*scaleRaids*/ false));
}

TEST(WorldScaleMap, PvpInstancesAreNeverScaled)
{
    // A battleground is already a level bracket and an arena is meant to be
    // symmetrical; no switch may turn this on.
    EXPECT_FALSE(ScalesOnThisMap(true, false, false, true, true));
    EXPECT_FALSE(ScalesOnThisMap(true, false, true, true, true));
}

TEST(WorldScaleMap, DungeonsFollowTheirSwitch)
{
    EXPECT_FALSE(ScalesOnThisMap(false, false, /*dungeon*/ true, /*scaleDungeons*/ false, false));
    EXPECT_TRUE(ScalesOnThisMap(false, false, /*dungeon*/ true, /*scaleDungeons*/ true, false));
}

TEST(WorldScaleMap, RaidsFollowTheirOwn)
{
    // Map::IsDungeon() is also true for a raid, so a raid arrives here with
    // both flags set. Asking about the raid first is what keeps the raid
    // switch reachable at all - with the order reversed, a raid would be
    // scaled by the *dungeon* setting and WorldScale.Raids would do nothing.
    EXPECT_FALSE(ScalesOnThisMap(false, /*raid*/ true, /*dungeon*/ true,
                                 /*scaleDungeons*/ true, /*scaleRaids*/ false));
    EXPECT_TRUE(ScalesOnThisMap(false, /*raid*/ true, /*dungeon*/ true,
                                /*scaleDungeons*/ false, /*scaleRaids*/ true));
}

// ---------------------------------------------------------------- TargetLevel

TEST(WorldScaleTargetLevel, NoDeltaIsThePlayersOwnLevel)
{
    EXPECT_EQ(TargetLevel(1, 0, MAX_LEVEL), 1);
    EXPECT_EQ(TargetLevel(42, 0, MAX_LEVEL), 42);
    EXPECT_EQ(TargetLevel(80, 0, MAX_LEVEL), 80);
}

TEST(WorldScaleTargetLevel, DeltaMovesTheTarget)
{
    EXPECT_EQ(TargetLevel(40, 3, MAX_LEVEL), 43);
    EXPECT_EQ(TargetLevel(40, -5, MAX_LEVEL), 35);
}

TEST(WorldScaleTargetLevel, NeverEscapesTheLevelCap)
{
    // A positive delta at the cap would ask for creature base stats at a level
    // the table does not have.
    EXPECT_EQ(TargetLevel(80, 5, MAX_LEVEL), 80);
    EXPECT_EQ(TargetLevel(79, 5, MAX_LEVEL), 80);
}

TEST(WorldScaleTargetLevel, NeverGoesBelowOne)
{
    EXPECT_EQ(TargetLevel(1, -5, MAX_LEVEL), 1);
    EXPECT_EQ(TargetLevel(3, -10, MAX_LEVEL), 1);
}

// ---------------------------------------------------------------- ShouldScale

TEST(WorldScaleShouldScale, EqualLevelsAreLeftAlone)
{
    EXPECT_FALSE(ShouldScale(/*creature*/ 30, /*target*/ 30, /*player*/ 30, 1, true, true));
}

TEST(WorldScaleShouldScale, DirectionsAreIndependentSwitches)
{
    // A weaker creature is pulled up only when ScaleUp is on...
    EXPECT_TRUE(ShouldScale(10, 30, 30, 1, true, false));
    EXPECT_FALSE(ShouldScale(10, 30, 30, 1, false, true));

    // ...and a stronger one is pulled down only when ScaleDown is on. This is
    // the realm's actual setting: up yes, down no, so high level content stays
    // dangerous.
    EXPECT_FALSE(ShouldScale(60, 30, 30, 1, true, false));
    EXPECT_TRUE(ShouldScale(60, 30, 30, 1, false, true));
}

TEST(WorldScaleShouldScale, BelowTheMinimumPlayerLevelNothingIsScaled)
{
    EXPECT_FALSE(ShouldScale(2, 10, 10, /*minPlayerLevel*/ 11, true, true));
    EXPECT_TRUE(ShouldScale(2, 10, 10, /*minPlayerLevel*/ 10, true, true));
}

// ----------------------------------------------------------- ComputeMultipliers

TEST(WorldScaleMultipliers, ScalingUpMakesTheCreatureHitHarderAndTakeLessPerHit)
{
    // A level 10 creature (100 health, 10 damage) presented as level 60
    // (1000 health, 50 damage).
    Multipliers m;
    ASSERT_TRUE(ComputeMultipliers(100.0f, 1000.0f, 10.0f, 50.0f, MIN_MULT, MAX_MULT, m));

    EXPECT_FLOAT_EQ(m.creatureDamage, 5.0f);   // deals five times its own damage
    EXPECT_FLOAT_EQ(m.playerDamage, 0.1f);     // takes a tenth, standing in for ten times the health
}

TEST(WorldScaleMultipliers, ScalingDownIsTheMirrorImage)
{
    Multipliers m;
    ASSERT_TRUE(ComputeMultipliers(1000.0f, 100.0f, 50.0f, 10.0f, MIN_MULT, MAX_MULT, m));

    EXPECT_FLOAT_EQ(m.creatureDamage, 0.2f);
    EXPECT_FLOAT_EQ(m.playerDamage, 10.0f);
}

TEST(WorldScaleMultipliers, BothSidesAreClamped)
{
    Multipliers m;
    ASSERT_TRUE(ComputeMultipliers(1.0f, 10000.0f, 1.0f, 10000.0f, MIN_MULT, MAX_MULT, m));

    EXPECT_FLOAT_EQ(m.creatureDamage, MAX_MULT);
    EXPECT_FLOAT_EQ(m.playerDamage, MIN_MULT);
}

TEST(WorldScaleMultipliers, MissingStatsLeaveTheMultipliersAlone)
{
    // Creature base stats can be absent or zero for odd unit classes; the
    // caller's defaults (1.0) must survive rather than becoming a division by
    // zero or an inf.
    Multipliers m;
    EXPECT_FALSE(ComputeMultipliers(0.0f, 1000.0f, 10.0f, 50.0f, MIN_MULT, MAX_MULT, m));
    EXPECT_FALSE(ComputeMultipliers(100.0f, 0.0f, 10.0f, 50.0f, MIN_MULT, MAX_MULT, m));
    EXPECT_FALSE(ComputeMultipliers(100.0f, 1000.0f, 0.0f, 50.0f, MIN_MULT, MAX_MULT, m));
    EXPECT_FALSE(ComputeMultipliers(100.0f, 1000.0f, 10.0f, 0.0f, MIN_MULT, MAX_MULT, m));

    EXPECT_FLOAT_EQ(m.creatureDamage, 1.0f);
    EXPECT_FLOAT_EQ(m.playerDamage, 1.0f);
}

TEST(WorldScaleMultipliers, RoundTripRestoresTheOriginalDamage)
{
    // The rage fix depends on this: whatever the damage was multiplied by,
    // dividing by the same figure has to give the raw number back, because the
    // rage formula is calibrated against raw damage.
    Multipliers m;
    ASSERT_TRUE(ComputeMultipliers(100.0f, 1000.0f, 10.0f, 50.0f, MIN_MULT, MAX_MULT, m));

    std::uint32_t const raw = 250;
    std::uint32_t const scaled = std::uint32_t(float(raw) * m.playerDamage);
    EXPECT_EQ(Unscale(scaled, m.playerDamage), raw);
}

// -------------------------------------------------------------- PresentedLevel

TEST(WorldScalePresentedLevel, WeakerCreaturesAreShownAtTheTargetLevel)
{
    EXPECT_EQ(PresentedLevel(/*creature*/ 12, /*target*/ 40, true, true), 40);
}

TEST(WorldScalePresentedLevel, EqualOrStrongerCreaturesKeepTheirOwnLevel)
{
    EXPECT_EQ(PresentedLevel(40, 40, true, true), 0);
    EXPECT_EQ(PresentedLevel(55, 40, true, true), 0);
}

TEST(WorldScalePresentedLevel, RespectsItsSwitches)
{
    EXPECT_EQ(PresentedLevel(12, 40, /*presentLevel*/ false, true), 0);
    // Relabelling without scaling up would be a lie about an unchanged fight.
    EXPECT_EQ(PresentedLevel(12, 40, true, /*scaleUp*/ false), 0);
}

// ------------------------------------------------------------------ AggroRange

TEST(WorldScaleAggro, TakesTheConfiguredLevelsOffTheRange)
{
    EXPECT_FLOAT_EQ(AggroRange(20.0f, 5), 15.0f);
    EXPECT_FLOAT_EQ(AggroRange(20.0f, 0), 20.0f);
}

TEST(WorldScaleAggro, NeverGoesNegative)
{
    EXPECT_FLOAT_EQ(AggroRange(3.0f, 5), 0.0f);
}

// --------------------------------------------------------------------- Unscale

TEST(WorldScaleUnscale, UndoesAMultiplication)
{
    EXPECT_EQ(Unscale(100, 2.0f), 50u);
    EXPECT_EQ(Unscale(50, 0.5f), 100u);
}

TEST(WorldScaleUnscale, AHitThatLandedIsNeverWorthNothing)
{
    EXPECT_EQ(Unscale(1, 20.0f), 1u);
}

TEST(WorldScaleUnscale, AnUnusableMultiplierLeavesTheDamageAlone)
{
    EXPECT_EQ(Unscale(123, 0.0f), 123u);
    EXPECT_EQ(Unscale(123, -1.0f), 123u);
}

// -------------------------------------------------------------------- QuestXP

TEST(WorldScaleQuestXP, AQuestAtThePlayersLevelPaysTheBaseAmount)
{
    // diffFactor is 20 at parity, clamped to 10, so the result is baseXp.
    EXPECT_EQ(QuestXP(/*baseXp*/ 1000, /*playerLevel*/ 40, /*questLevel*/ 40), 1000u);
}

TEST(WorldScaleQuestXP, TheFirstFiveLevelsBelowThePlayerCostNothing)
{
    // diffFactor is clamp(2 * (questLevel - playerLevel) + 20, 1, 10), so it
    // is already at its ceiling of 10 for anything from five levels below the
    // player upwards: a level 35 quest pays a level 40 character exactly what
    // a level 40 quest does. The shoulder only starts beyond that.
    EXPECT_EQ(QuestXP(1000, 40, 35), QuestXP(1000, 40, 40));
    EXPECT_EQ(QuestXP(1000, 40, 50), QuestXP(1000, 40, 40));

    EXPECT_LT(QuestXP(1000, 40, 34), QuestXP(1000, 40, 40));
    EXPECT_LT(QuestXP(1000, 40, 30), QuestXP(1000, 40, 34));
}

TEST(WorldScaleQuestXP, TheDifficultyFactorIsClampedAtBothEnds)
{
    // Far below: the factor floors at 1, a tenth of the base.
    EXPECT_EQ(QuestXP(1000, 80, 1), 100u);
    // Far above: the factor caps at 10, so a quest well above the player pays
    // the same as one at the player's level. Without the cap, scaling a quest
    // up would pay unboundedly.
    EXPECT_EQ(QuestXP(1000, 10, 60), QuestXP(1000, 10, 10));
}

TEST(WorldScaleQuestXP, RoundsTheWayTheCoreDoes)
{
    // The core rounds into bands: to 5 below 100, 10 below 500, 25 below 1000,
    // 50 above. A mismatch here would show up as quest XP a few points off
    // what the client's own quest log predicted.
    EXPECT_EQ(QuestXP(93, 40, 40) % 5, 0u);
    EXPECT_EQ(QuestXP(430, 40, 40) % 10, 0u);
    EXPECT_EQ(QuestXP(930, 40, 40) % 25, 0u);
    EXPECT_EQ(QuestXP(4300, 40, 40) % 50, 0u);
}
