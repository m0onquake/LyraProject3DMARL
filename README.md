# Lyra TacticalMARL overlay

This repository contains only the MARL-specific additions and Lyra source
overrides for the Unreal Engine 5.3 Lyra sample. It intentionally does not
redistribute the multi-gigabyte Lyra base content.

## Prerequisites

- Unreal Engine 5.3 installed at `E:\UE_5.3` (or adjust commands below).
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
E:\UE_5.3\Engine\Build\BatchFiles\Build.bat LyraEditor Win64 Development `
  -Project="E:\UEproject\LyraProject\LyraProject.uproject" `
  -CompilerVersion="14.38.33130"
```

## Run the scripted recon-strike policy

Start `Content/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot` in UE, then run:

```powershell
E:\UE_5.3\Engine\Binaries\ThirdParty\Python3\Win64\python.exe `
  Plugins\TacticalMARL\Python\scripted_policy.py `
  --episodes 10 --seed 42 --episode-end-delay 4
```

JSONL trajectories are written under `Saved/TacticalMARL/Logs` and remain
local-only. See `Plugins/TacticalMARL/README.md` for the UDP schema, stress
test command and report locations.

## S0-S3 acceptance

The UrbanDepot Game mode includes the S0 overview/HUD and S1 blue-force health
and disabled-state visualization. D1 adds S2 red-force local sensing, delayed
shared alerts, confidence decay, LOS occlusion and last-known-position debug
visualization without enabling red attacks. Run
`Plugins/TacticalMARL/Python/s0_baseline_check.py` for the baseline contract,
`s1_health_check.py --seed 42 --resets 10` for health/action-mask/reset checks,
and launch with `-TacticalMARLS2Test` before running
`s2_threat_check.py --seed 42` for S2. Detailed evidence is recorded in the
corresponding `Docs/S0...`, `Docs/S1...`, and `Docs/S2...` acceptance records.
S3 adds the D2 Anti-UAV launcher state machine, visible lock warning, missile
flight/impact, deterministic hit calculation, ammunition/heat limits and S1
health damage. Launch with `-TacticalMARLS3Test` and run
`s3_air_defense_check.py --seed 42`; see the S3 acceptance record for evidence.

## Full local history

The development machine retains the former complete-project history on the
local branch `backup/full-lyra-history-20260928`. It is deliberately not
pushed because it contains approximately 2.3 GiB of upstream Lyra LFS assets.
