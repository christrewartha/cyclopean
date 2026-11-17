# Cyclopean

A VR prototype for Meta Quest (Unreal Engine 5.8, OpenXR): fire a Cyclops-style optic blast from your visor
at bats flying through a ruined courtyard.

## Gameplay
- **Optic blast** (`Source/Cyclops/.../OpticBlastComponent`): hold **A / X** (or **F** / LMB on desktop).
  Energy-based: the first hit is a strong burst, holding drains power, release to recharge. Knocks physics
  props over and scorches surfaces.
- **Enemies** (`FlyingEnemy`, `EnemySpawner`): placeholder bats that announce themselves, weave towards your head
  and break apart when killed.
- **Menus** (`GameFlowManager`): start screen and pause menu (left **Menu** button / Tab); press buttons by
  blasting them.
- **Wrist perf panel** (`WristPerfPanel`): FPS / thread timings on the left wrist; left-stick click toggles.

## Getting it running
1. UE 5.8 with Android/Quest set up.
2. Install the **"Low Poly Nature"** pack from FAB into `Content/VRS_LowPolyNature` (not included here; it
   can't be redistributed). The arena map `/Game/Cyclops/Maps/L_Arena` uses it.
3. Open `Cyclops.uproject` (it builds the `Cyclops` C++ module) and Launch on a Quest.

## Release builds
Release signing uses a keystore that is **not** in this repo. Put it in `Build/Android/` and its passwords in
`Config/UserEngine.ini` (git-ignored):

```ini
[/Script/AndroidRuntimeSettings.AndroidRuntimeSettings]
KeyStorePassword=...
KeyPassword=...
```

Bump `StoreVersion` in `Config/DefaultEngine.ini` for every store upload.
