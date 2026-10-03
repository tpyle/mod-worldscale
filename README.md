# mod-worldscale

Per-player creature level scaling for the open world.

mod-autobalance handles instances. This does the other 99% of the map: a
creature below the player's level fights, hits and rewards as though it were
the player's level, per observer, so old content stays worth doing and a
levelling character can go wherever the story leads.

Damage in both directions, health, armour and XP are scaled. The creature's
real level is never changed - everything is computed per observer, which is
essential on a realm where hundreds of bots of every level share the same
world and a single global level could not be right for all of them.

## What it touches

| | |
| --- | --- |
| damage dealt and taken | `ModifyMeleeDamage`, `ModifySpellDamageTaken`, `ModifyPeriodicDamageAurasTick` |
| health and armour | on creature spawn, per observer |
| kill and quest XP | `OnPlayerGiveXP`, `OnPlayerQuestComputeExp` |
| the displayed level | `UNIT_FIELD_LEVEL` is patched per observer in the update block, so a mob that hits like level 60 stops showing a grey name |
| aggro range | scaled-up creatures otherwise aggro from the range their real level implies |
| rage | the rage formula reads raw damage, so a scaled exchange gave the wrong rage in both directions; the damage is unscaled again before the formula sees it |

## Skinning, and who gets told the truth

A scaled-up corpse is unskinnable to the client and skinnable to the server at
the same time. The client works the required skill out itself, from the level it
holds for the corpse, and will not even send the cast; the server disagrees,
because both `Spell::CheckCast` and `Spell::EffectSkinning` read
`GetUnitTarget()->GetLevel()` - the real level - and not `getLevelForTarget`. So
a level 5 hare presented at 30 cannot be skinned by anyone, with no error to
explain why.

A skinnable corpse therefore stops lying, but only to the observers the lie is
actually blocking. Three things have to hold for one observer:

1. they have some of the skill the corpse asks for - the template's own, so
   herbalism, mining or engineering where those apply, not skinning assumed;
2. the level they are being shown puts it out of reach;
3. the real level does not.

The third is the point. If they could not skin it at its true level either, the
lie is not what is stopping them, and renumbering the corpse in front of them
would be a cosmetic change that bought nothing - so it keeps its scaled level.

This is decided **per observer**, which is the grain the whole mechanism works
at: the level is patched into each client's packet separately, so one person can
be told the real level while everybody else goes on seeing the scaled one. The
resend on death is not per observer - a change mask covers the field for
everyone - but that costs nothing, because anybody who does not need the truth
is sent the presented value again, unchanged, and their client has nothing to
react to.

The arithmetic is `WorldScaleMath::SkinningRequirement` and
`TruthWouldAllowSkinning`, so it is tested without a server. It replicates the
core's own formula rather than approximating it:

    ReqValue = (skillValue < 100 ? (level - 10) * 10 : level * 5)

which is worth knowing is discontinuous - a hundredth point of skill moves the
requirement a long way, and below level 10 the first branch goes negative and
means no requirement at all.

## Configuration (`mod_worldscale.conf`)

`WorldScale.Enable`, `.ScaleUp`, `.ScaleDown`, `.LevelDelta`,
`.MinPlayerLevel`, `.AffectElites`, `.AffectWorldBosses`, `.ScaleXP`,
`.ScaleQuests`, `.MinMultiplier`, `.MaxMultiplier`, `.PresentLevel`,
`.Aggro.LevelsBelow`.

Scaling *down* (making high level content safe) is a separate switch from
scaling up and is off by default.

## Requirements

A core carrying these additive hooks, each with its call site:
`UnitScript::OnUnitRewardRage`, `UnitScript::OnUnitGetLevelForTarget`,
`AllCreatureScript::OnCreatureGetAggroRange`,
`PlayerScript::OnPlayerQuestComputeLevel` and `::OnPlayerQuestComputeExp`.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its
modules use. See [LICENSE](LICENSE).
