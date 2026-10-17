# Condition functions

CTDA conditions are evaluated by one C++ evaluator
(`engine/extension/src/conditions/`, ADR 0004). This file lists which condition
functions the game data uses and, in the Bindings section, how each is bound.

## Census (Skyrim.esm + Update.esm, 2026-10-17)

68 143 CTDAs on DIAL, INFO, QUST and DLBR (none on DIAL and DLBR), 142
distinct functions; 4 767 CTDAs on 5 981 PACK records (75 distinct functions).
Run on: subject 63 281, reference 2 222, target 1 163, quest alias 954, combat
target 367, event data 153, linked ref 3 (PACK: subject 4 330, reference 245,
package data 109, quest alias 68, linked ref 15). The 70 most used functions of
dialogue and quests, by INFO + QUST uses; the first 25 cover 90 %, the first 40
95 %. PACK uses are for the same functions (0: no package uses it).

| Index | Function | INFO uses | QUST uses | PACK uses |
|---:|---|---:|---:|---:|
| 72 | GetIsID | 15432 | 3633 | 32 |
| 71 | GetInFaction | 4575 | 2205 | 89 |
| 426 | GetIsVoiceType | 6324 | 269 | 0 |
| 58 | GetStage | 6110 | 183 | 1519 |
| 566 | GetIsAliasRef | 5334 | 76 | 109 |
| 629 | GetVMQuestVariable | 3494 | 319 | 550 |
| 59 | GetStageDone | 2886 | 217 | 600 |
| 74 | GetGlobalValue | 1087 | 134 | 232 |
| 46 | GetDead | 586 | 298 | 104 |
| 67 | GetInCell | 553 | 230 | 70 |
| 560 | HasKeyword | 248 | 507 | 8 |
| 77 | GetRandomPercent | 693 | 7 | 23 |
| 561 | HasRefType | 5 | 688 | 0 |
| 359 | GetInCurrentLoc | 573 | 109 | 53 |
| 47 | GetItemCount | 589 | 80 | 3 |
| 606 | GetKeywordDataForLocation | 594 | 4 | 30 |
| 562 | LocationHasKeyword | 269 | 274 | 4 |
| 131 | GetPCIsSex | 472 | 0 | 0 |
| 579 | GetEquippedShout | 446 | 0 | 0 |
| 567 | GetIsEditorLocAlias | 0 | 436 | 2 |
| 249 | IsInDialogueWithPlayer | 428 | 0 | 0 |
| 543 | GetQuestCompleted | 389 | 24 | 35 |
| 403 | GetRelationshipRank | 298 | 64 | 0 |
| 14 | GetActorValue | 322 | 23 | 169 |
| 1 | GetDistance | 175 | 149 | 67 |
| 70 | GetIsSex | 278 | 3 | 0 |
| 310 | GetInWorldspace | 248 | 31 | 24 |
| 360 | GetInCurrentLocAlias | 12 | 258 | 4 |
| 130 | GetPCIsRace | 261 | 0 | 0 |
| 73 | GetFactionRank | 259 | 2 | 3 |
| 286 | IsSneaking | 247 | 1 | 0 |
| 650 | IsLinkedTo | 0 | 234 | 2 |
| 182 | GetEquipped | 229 | 1 | 9 |
| 56 | GetQuestRunning | 180 | 36 | 85 |
| 84 | GetDeadCount | 173 | 25 | 15 |
| 605 | LocAliasIsLocation | 121 | 76 | 2 |
| 69 | GetIsRace | 179 | 2 | 26 |
| 255 | GetOffersServicesNow | 165 | 10 | 0 |
| 161 | GetIsCurrentPackage | 160 | 13 | 1 |
| 300 | IsInInterior | 48 | 122 | 25 |
| 35 | GetDisabled | 111 | 53 | 54 |
| 503 | GetAllowWorldInteractions | 0 | 151 | 0 |
| 136 | GetIsReference | 33 | 113 | 80 |
| 181 | HasSameEditorLocAsRefAlias | 0 | 136 | 0 |
| 576 | GetEventData | 0 | 119 | 0 |
| 601 | GetLocAliasRefTypeAliveCount | 0 | 95 | 0 |
| 563 | LocationHasRefType | 0 | 94 | 0 |
| 80 | GetLevel | 40 | 36 | 1 |
| 596 | SpellHasKeyword | 74 | 0 | 0 |
| 66 | GetShouldAttack | 52 | 20 | 0 |
| 314 | IsActorAVictim | 63 | 0 | 0 |
| 432 | GetIsObjectType | 12 | 48 | 0 |
| 50 | GetTalkedToPC | 59 | 1 | 4 |
| 655 | GetIntimidateSuccess | 55 | 2 | 0 |
| 145 | IsTrespassing | 56 | 0 | 0 |
| 453 | GetPlayerTeammate | 54 | 0 | 0 |
| 630 | GetVMScriptVariable | 12 | 40 | 28 |
| 277 | GetBaseActorValue | 49 | 0 | 0 |
| 259 | HasFamilyRelationship | 0 | 48 | 0 |
| 125 | IsGuard | 16 | 31 | 0 |
| 48 | GetGold | 46 | 0 | 0 |
| 101 | IsWeaponMagicOut | 44 | 1 | 0 |
| 289 | IsInCombat | 31 | 13 | 15 |
| 654 | GetBribeSuccess | 44 | 0 | 0 |
| 624 | GetInContainer | 39 | 4 | 1 |
| 459 | GetCrimeGold | 41 | 2 | 0 |
| 640 | GetActorValuePercent | 42 | 0 | 1 |
| 18 | GetCurrentTime | 26 | 16 | 0 |
| 651 | GetKeywordDataForCurrentLocation | 2 | 38 | 0 |
| 144 | GetTrespassWarningLevel | 39 | 0 | 0 |

## Bindings

TODO: filled by the engine task (one row per function: bound, placeholder or
unknown, with the data it reads).
