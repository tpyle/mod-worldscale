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
