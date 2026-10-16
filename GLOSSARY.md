# SkyDot

An open-source engine that plays Skyrim from the player's own installation,
converted into SkyDot's own data format.

## Language

### Data

**Edition**:
The release of the original game that source data comes from: SE (incl. AE), LE or VR. A property of the data, never of how it is played.
_Avoid_: version, flavour

**Pack**:
The player's game data from one Edition, plus their mods, converted into SkyDot's format.
_Avoid_: conversion, export, build

### Mods

**Data mod**:
A mod made only of assets and plugin records (ESP/ESM/ESL); it contains no code of its own beyond vanilla-style Papyrus scripts.
_Avoid_: asset mod (too narrow)

**Script mod**:
A SkyDot-native mod made of interpreted scripts and data that talks to the engine only through the mod API; it may use any licence.

**Native mod**:
A SkyDot-native mod containing compiled code (GDExtension); it stays under the GPL.
_Avoid_: power mod, plugin (an ESP is a plugin)

### Running the game

**Mode**:
How a session is played: flatscreen or VR. A property of the session; any Pack can be played in any Mode.
_Avoid_: platform, edition

**Game layer**:
The game's logic and world state (scripts, AI, activation, saves), which knows nothing of camera, input devices, debug overlay or shell settings.
_Avoid_: viewer, game

**Shell**:
The program that hosts the Game layer for one Mode and owns camera, input and presentation.
_Avoid_: frontend, client

**Flat shell**:
The Shell for flatscreen Mode.
_Avoid_: viewer, cell viewer (the current code name)

**VR shell**:
The Shell for VR Mode.

### Milestones

**Vertical slice**:
The route Helgen → Riverwood → Bleak Falls Barrow → Whiterun → the first dragon (Dragon Rising) on which M1, the whole vanilla game, is verified.
_Avoid_: demo, MVP

**Debug start**:
A development-only way to begin play at any point of the slice with a ready character, skipping Helgen.
_Avoid_: alternate start (the name of a player mod)

**Tier**:
The importance class of a game system: T1 blocks the slice, T2 makes the slice feel wrong, T3 is not exercised by the slice.
_Avoid_: priority level
