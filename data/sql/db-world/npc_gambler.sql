-- Asmongold <Deathroll Addict> (Tofu Worldview fork of mod-npc-gambler)
-- Schema: acore_world. Applied by worldserver's updater as module SQL; re-runnable (DELETE then INSERT).
-- Restart needed (creature_template). Players must delete the client Cache folder to see name, model, and text changes.
-- Spawn is not here: it is a separate custom SQL file (tofu_*.sql).
-- Check: SELECT entry, name, subname, ScriptName FROM creature_template WHERE entry = 601020; SELECT CreatureDisplayID FROM creature_template_model WHERE CreatureID = 601020; SELECT ID FROM npc_text WHERE ID BETWEEN 900020 AND 900027;
-- Expect: 601020 | Asmongold | Deathroll Addict | gamble_npc; display 428; eight npc_text rows.

DELETE FROM `creature_template` WHERE `entry` = 601020;
INSERT INTO `creature_template` (`entry`, `difficulty_entry_1`, `difficulty_entry_2`, `difficulty_entry_3`, `KillCredit1`, `KillCredit2`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `speed_swim`, `speed_flight`, `detection_range`, `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `PetSpellDataId`, `VehicleId`, `mingold`, `maxgold`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`) VALUES
(601020, 0, 0, 0, 0, 0, 'Asmongold', 'Deathroll Addict', NULL, 0, 80, 80, 2, 35, 1, 1, 1.14286, 1, 1, 20, 0, 0, 1, 2000, 2000, 1, 1, 1, 0, 2048, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 16777216, 'gamble_npc', 0);

-- Display 428: Creature\HumanMaleFarmer, skin HumanMaleFarmerSkin01, NPCSoundID 0 (silent gossip greeting).
DELETE FROM `creature_template_model` WHERE `CreatureID` = 601020;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(601020, 0, 428, 1, 1, 12340);

-- Gossip texts 900020 to 900027 (registry: npc_text 900020 to 900029). $B is a line break.
DELETE FROM `npc_text` WHERE `ID` BETWEEN 900020 AND 900029;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`) VALUES
(900020, 'Oh, look who it is. You here to lose some gold or what?$B$BPick your poison: a deathroll, some gamba, or rock-paper-scissors if you''re broke.', 'Oh, look who it is. You here to lose some gold or what?$B$BPick your poison: a deathroll, some gamba, or rock-paper-scissors if you''re broke.', 1),
(900021, 'Roll 1 to 49 and you double up. 50 to 99, the house eats it. Hit a 100 and that''s the jackpot, triple your bet.$B$BHow much are we putting on it?', 'Roll 1 to 49 and you double up. 50 to 99, the house eats it. Hit a 100 and that''s the jackpot, triple your bet.$B$BHow much are we putting on it?', 1),
(900022, 'Rock, paper, scissors. Beat me and I''ll give you a copper. Free money, apparently.$B$BPick one.', 'Rock, paper, scissors. Beat me and I''ll give you a copper. Free money, apparently.$B$BPick one.', 1),
(900023, 'A deathroll. You roll, then I roll under your number, back and forth. First one to hit a 0 loses. Winner takes double.$B$BHow much gold are you putting up?', 'A deathroll. You roll, then I roll under your number, back and forth. First one to hit a 0 loses. Winner takes double.$B$BHow much gold are you putting up?', 1),
(900024, 'What are we rolling to?$B$BSudden Death is 0 or 1. Rollie is 100. Standard is ten times your bet. Or name your own number.', 'What are we rolling to?$B$BSudden Death is 0 or 1. Rollie is 100. Standard is ten times your bet. Or name your own number.', 1),
(900025, 'Gold''s on the table. You roll first.$B$BLast chance to back out.', 'Gold''s on the table. You roll first.$B$BLast chance to back out.', 1),
(900026, 'Okay, okay. My turn. Watch this.', 'Okay, okay. My turn. Watch this.', 1),
(900027, 'Your roll. Don''t choke.', 'Your roll. Don''t choke.', 1);
