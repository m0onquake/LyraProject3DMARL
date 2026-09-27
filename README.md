# Lyra TacticalMARL overlay

This repository contains only the MARL-specific additions and Lyra source
overrides for the Unreal Engine 5.3 Lyra sample. It intentionally does not
redistribute the multi-gigabyte Lyra base content.

## Prerequisites

- Unreal Engine 5.3 installed at `D:\UE_5.3` (or adjust commands below).
- A clean Lyra Starter Game project for UE 5.3.
- Visual Studio 2022 with MSVC `14.38.33130`.
- Python 3; UE's bundled Python is also supported.

## Apply the overlay

Copy this repository over a UE 5.3 Lyra project while preserving directory
structure. The overlay supplies:

- `Plugins/TacticalMARL`: JSON/UDP runtime, scripted policy and stress tools.
- `Content/MARL`: the compact MARL test and Urban Depot maps/assets.
- `Source/LyraGame`: tactical red force, weapon and bot-spawn integration.
- `Config/DefaultEngine.ini` and `LyraProject.uproject`: required project setup.

Do not delete the remaining Lyra `Content`, `Plugins`, `Source`, or `Config`
files from the base installation; they are prerequisites, just not tracked here.

## Build

```powershell
D:\UE_5.3\Engine\Build\BatchFiles\Build.bat LyraEditor Win64 Development `
  -Project="D:\UEproject\LyraProject\LyraProject.uproject" `
  -CompilerVersion="14.38.33130"
```

## Run the scripted recon-strike policy

Start `Content/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot` in UE, then run:

```powershell
D:\UE_5.3\Engine\Binaries\ThirdParty\Python3\Win64\python.exe `
  Plugins\TacticalMARL\Python\scripted_policy.py `
  --episodes 10 --seed 42 --episode-end-delay 4
```

JSONL trajectories are written under `Saved/TacticalMARL/Logs` and remain
local-only. See `Plugins/TacticalMARL/README.md` for the UDP schema, stress
test command and report locations.

## Full local history

The development machine retains the former complete-project history on the
local branch `backup/full-lyra-history-20260928`. It is deliberately not
pushed because it contains approximately 2.3 GiB of upstream Lyra LFS assets.
