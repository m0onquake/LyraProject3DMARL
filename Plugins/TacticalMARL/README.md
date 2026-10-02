# TacticalMARL UAV/UGV runtime

`ATacticalUAVPawn` is the first policy-controlled agent used by the MARL test range.

## S0/D0 visible baseline

`L_MARL_UrbanDepot` now starts with the S0 overlay enabled. It shows the
Episode, fixed seed, mission phase, passive-red alert phase, 6-blue/5-red
roster status, world-space Agent ID/side/role/task/health-state labels, the
blue base, red defense zone and primary Anti-UAV objective. The automatic
overview camera makes the complete deployment visible in Game and PIE.

The layers are independently switchable at runtime:

```text
tacticalmarl.S0HUD 0|1
tacticalmarl.S0Labels 0|1
tacticalmarl.S0Zones 0|1
tacticalmarl.S0OverviewCamera 0|1
```

Difficulty D0 keeps every red Lyra bot visible at its fixed spawn with its
configured role/loadout, while its stock combat brain remains paused. This is
the passive-red regression baseline; active red perception and fire belong to
later stages.

With the Game window running, validate the live v1 contract and exact roster:

```powershell
& 'E:\UE_5.3\Engine\Binaries\ThirdParty\Python3\Win64\python.exe' `
  Python\s0_baseline_check.py --seed 42
```

For a repeatable visual acceptance run, add
`-TacticalMARLS0AutoDemo -TacticalMARLS0Capture`. After Lyra warmup the game
starts seed 42 and saves `Saved/TacticalMARL/Screenshots/S0_UrbanDepot_seed42.png`.

## S1 blue health and disabled-state acceptance

UAV and UGV Pawns share `UTacticalAgentHealthComponent`. Their observations
now include `health`, `max_health`, `alive`, `disabled`, `status`,
`last_damage_source`, `last_hit_direction`, `damage_event_count`, and an
`action_mask`. A disabled agent rejects normal policy commands but its Pawn is
retained for observations and trajectory logs. UAVs perform a controlled
descent; UGVs stop and show a persistent smoke/damage indication. Episode
reset restores health, task state, visuals, and the original transform.

Run the dedicated visible demonstration with:

```powershell
& 'E:\UE_5.3\Engine\Binaries\Win64\UnrealEditor.exe' `
  'E:\UEproject\LyraProject\LyraProject.uproject' `
  '/Game/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot' `
  -game -log -windowed -ResX=1600 -ResY=900 `
  -TacticalMARLS1AutoDemo -TacticalMARLS1Capture
```

After warmup it resets to seed 42, applies fixed test damage to
`BLUE_UAV_01` and `BLUE_UGV_01`, and saves
`Saved/TacticalMARL/Screenshots/S1_HealthAndDisable_seed42.png`. The fixed
damage is an acceptance-only path and does not add an active red attack policy.

For live automated S1 acceptance, launch the Game with
`-TacticalMARLS1Test`, then run:

```powershell
& 'E:\UE_5.3\Engine\Binaries\ThirdParty\Python3\Win64\python.exe' `
  Python\s1_health_check.py --seed 42 --resets 10
```

The test records before/after/reset observations in
`Saved/TacticalMARL/Logs/s1_health_demo_seed42.jsonl` and writes the result to
`Saved/TacticalMARL/Reports/s1_health_check.json`. The UDP
`apply_test_damage` request is rejected unless the Game was explicitly started
with `-TacticalMARLS1Test` or `-TacticalMARLS1AutoDemo`.

## S2 red local perception and shared-alert acceptance

`UTacticalThreatSubsystem` gives each red combatant an independent sensor with
role-based range and FOV, visibility-channel LOS, and confidence accumulation.
Only a successful local LOS observation may create target identity/location
state. The shared alert transports that perceived snapshot after a
seed-deterministic delay and decays its confidence after LOS is lost; reset
clears every sensor, pending message, event and last-known location. S2 does not
enable red aiming, firing or damage.

S2 is disabled at D0. Use `tacticalmarl.Difficulty=1` (or one of the S2 test
flags below) and `tacticalmarl.S2Debug=1` to display sensor sectors, LOS lines,
state colors, confidence, propagation status and the shared last-known marker.
The normal UDP response gains a root `red_threat` object; decentralized blue
observations remain unchanged.

Run the repeatable visible demonstration with:

```powershell
& 'E:\UE_5.3\Engine\Binaries\Win64\UnrealEditor.exe' `
  'E:\UEproject\LyraProject\LyraProject.uproject' `
  '/Game/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot' `
  -game -log -windowed -ResX=1600 -ResY=900 `
  -TacticalMARLS2AutoDemo -TacticalMARLS2Capture
```

After warmup the demo resets to seed 42 and drives the acceptance-only stages
`occluded -> visible -> lost_contact`. It saves three PNG files below
`Saved/TacticalMARL/Screenshots`: `S2_Occluded_seed42.png`,
`S2_Tracking_seed42.png`, and `S2_LostContact_seed42.png`. The temporary wall
exists only in the guarded S2 acceptance path; regular sensing uses collision
from the real map through `ECC_Visibility`.

For automated acceptance, launch the Game with `-TacticalMARLS2Test`, then run:

```powershell
python Python\s2_threat_check.py --host 127.0.0.1 --port 7777 --seed 42
```

The test verifies five reset sensors, occlusion without detection, configured
message delay, confidence decay, Tracking-to-LostContact transition, identical
canonical alert sequences for two seed-42 runs, and complete reset clearing.
Its report is `Saved/TacticalMARL/Reports/s2_threat_check.json`; detailed
snapshots are recorded in `Saved/TacticalMARL/Logs/s2_threat_seed42.jsonl`.
The `s2_test_stage` UDP request is rejected unless the Game was explicitly
started with `-TacticalMARLS2Test` or `-TacticalMARLS2AutoDemo`.

## S3 D2 Anti-UAV launcher acceptance

`UTacticalAirDefenseSubsystem` consumes only the direct UAV contacts produced
by `RED_ANTIUAV_01`'s S2 sensor. Its rule sequence is:

```text
scanning -> confirming -> locking -> warning -> launching -> cooldown -> scanning
                                     \-> lost_lock -> scanning
```

The launcher prioritizes perceived `StrikeRole`, track/`CloseReconRole`,
`WideReconRole`, then other UAVs. It never attacks UGVs. A shot requires
continuous direct LOS through confirmation, lock and the visible warning
countdown. Hit probability is recorded and explained by perceived distance,
altitude, speed, lateral evasion and impact-time obstruction. Seeded random
rolls are repeatable. Hits call `UTacticalAgentHealthComponent`, while reset
restores ammo, heat, cooldown, target state, health and transient effects.

Run the visible D2 demonstration with:

```powershell
& 'E:\UE_5.3\Engine\Binaries\Win64\UnrealEditor.exe' `
  'E:\UEproject\LyraProject\LyraProject.uproject' `
  '/Game/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot' `
  -game -log -windowed -ResX=1600 -ResY=900 `
  -TacticalMARLS3AutoDemo -TacticalMARLS3Capture
```

It saves `S3_LockWarning_seed42.png`, `S3_Impact_seed42.png`, and
`S3_LostLock_seed42.png` below `Saved/TacticalMARL/Screenshots`. The HUD and
world markers show launcher state, target, lock line/countdown, missile/impact,
ammo, heat, hit probability, target health and `LOCK BROKEN / NO FIRE`.

For automated S3 acceptance, launch with `-TacticalMARLS3Test`, then run:

```powershell
python Python\s3_air_defense_check.py --host 127.0.0.1 --port 7777 --seed 42
```

The test covers occlusion/no fire, a deterministic hit through the S1 health
interface, warning/incoming observations, cooldown and ammo decrement, same-seed
roll/state reproduction, lateral evasion, warning-time lost lock, and reset.
Results are written to `Saved/TacticalMARL/Reports/s3_air_defense_check.json`.
The root response contains `red_air_defense`; every blue observation also has a
compact `threat` object. `s3_test_stage` is rejected outside the guarded S3 test
and auto-demo modes.

## Supported tasks

- `idle`
- `move` / `move_to`
- `recon`
- `surveillance` / `observe`
- `strike` / `attack`
- `rtb` / `return_to_base`

## Policy JSON

```json
{
  "sequence_id": 1,
  "task": "recon",
  "target": [1000, 1200, 900],
  "altitude": 900,
  "speed": 1200,
  "acceptance_radius": 150,
  "duration": 10,
  "orbit_radius": 600
}
```

Call `ReceivePolicyJson` or `ReceivePolicyCommand`. A compact RL bridge can call
`SubmitDiscreteAction`, where actions 0..5 map to idle, move, recon,
surveillance, strike and return-to-base.

Call `GetTelemetryJson` for observations. It reports agent id, position,
velocity, task state and currently detected actors.

Strike damage is disabled by default. `OnStrikeExecuted` still fires, allowing
the training environment to calculate rewards without changing Lyra health.

## Python policy bridge

In PIE/Game worlds the plugin listens on `udp://127.0.0.1:7777`. Send one JSON
datagram and receive one response containing `ok`, optional `error`, and the
latest observations for all blue agents.

```json
{"agent_id":"BLUE_UAV_01","sequence_id":1,"move_input":[1,0,0.2],"yaw_rate":15}
```

```json
{"agent_id":"BLUE_UGV_01","sequence_id":2,"throttle":1,"steering":-0.25,"brake":false}
```

Use `{"request":"observations"}` for a read-only step. A standard-library-only
client and environment adapter are provided in this plugin's `Python` directory.

## MARL episodes

`UTacticalMARLEpisodeSubsystem` provides a real-time parallel multi-agent API.
It captures all actors tagged `MARL.Agent` at the first reset, restores their
spawn transforms between episodes, resets UAV/UGV task and sensor state, and
tracks the episode id, seed, step, wall-clock duration and end reason.

The response schema follows the common parallel environment convention:

- `observations.agents`: per-agent telemetry
- `rewards`: reward keyed by agent id
- `terminations`: task/environment terminal flags plus `__all__`
- `truncations`: time/step-limit flags plus `__all__`
- `infos`: protocol and execution-mode metadata

Protocol requests are:

```json
{"request":"reset","seed":42}
{"request":"actions","actions":[{"agent_id":"BLUE_UAV_01","move_input":[1,0,0],"yaw_rate":0}]}
{"request":"advance"}
{"request":"end","reason":"evaluation_complete"}
```

For training, use `Python/tactical_marl_env.py`. Its `step()` method sends the
joint action, waits one configurable real-time decision interval, and then
advances the episode. A one-datagram `request=step` is also supported for manual
testing, but its observation is taken immediately after applying the action.
The Python adapter automatically retries an early reset while Lyra is still
initializing red-force PawnData and GAS components.

The default episode has 1000 steps or 300 seconds and uses the compact map's
200 m x 200 m bounds. Initial rewards are deliberately simple and inspectable:
step penalty, new detections, task completion/failure and out-of-bounds failure.

## Recon strike mission and red-force state

The mission runtime follows the supplied TacML chain:

`search -> candidate_found -> target_confirmed -> track_established -> strike_ready -> engagement -> effect_assessment -> success`

Red Lyra bots expose their real GAS health, maximum health, alive state, role,
death count and last-killer id through `UTacticalRedBotStateComponent`. An
Anti-UAV bot is treated as the primary mission objective by default; individual
spawn points can also set `bMissionObjective`. If no explicit objective exists,
all red combatants form the objective set. Episode reset heals surviving bots
or respawns dead bots at their configured tactical spawn points.

The shared team reward is intentionally event-based and is returned to every
blue agent. The response also includes `mission.reward_components`, so training
logs can audit each contribution:

- time/step cost and repeated-search cost
- new 10 m coverage cells
- first detection and target confirmation
- track establishment, maintenance, loss and reacquisition
- valid or invalid engagement under the confirm/track/ready constraints
- normalized objective damage and objective kill
- final mission success

For one environment step, the default shared reward is:

```text
R_team = -0.01 step
       + 0.02 * new_10m_cells
       - 0.005 if no new cell
       + 1.0 first_detection
       + 2.0 target_confirmed
       + 2.0 track_established
       + 0.02 track_maintained
       - 0.5 track_lost
       + 1.0 track_reacquired
       + 0.5 valid_engagement
       - 2.0 invalid_engagement
       + 2.0 * objective_health_fraction_removed
       + 5.0 objective_neutralized
       + 10.0 mission_success
```

Every blue agent receives `R_team`. The agent that caused damage or a kill also
receives the corresponding attribution reward, while the existing per-agent
movement/task reward remains local. This supports a centralized team objective
without discarding individual credit assignment.

`mission.targets` contains red health and kill state, while each blue-agent
observation receives only the compact mission phase/track fields. Centralized
critics may consume the full root mission state; decentralized actors should
use their individual observation objects.

MARL engagements clamp an objective at the configurable 1% health
neutralization threshold. This preserves real GAS damage and battle-damage
assessment while preventing Lyra's stock Elimination experience from ending
the round and destroying every red Pawn. Episode reset heals the neutralized
Pawn to full health.

## Deterministic scripted baseline

Start `L_MARL_UrbanDepot` in PIE or Game mode, then run from the plugin's Python
directory (no third-party Python packages are required):

```powershell
& 'D:\UE_5.3\Engine\Binaries\Win64\UnrealEditor.exe' `
  'D:\UEproject\LyraProject\LyraProject.uproject' `
  '/Game/MARL/UrbanDepot/Maps/L_MARL_UrbanDepot' -game -log
```

```powershell
python scripted_policy.py --episodes 1 --seed 42
```

The clients wait until the UrbanDepot red GAS actors are initialized and Lyra's
opening warmup has completed before starting the first Episode.

The policy uses only the existing UDP `reset`, `actions` and `advance`
requests. UAVs and UGVs perform deterministic area search, confirmation and
continuous surveillance; after the target is confirmed and the track is valid,
one strike-capable agent approaches and repeatedly performs legal engagements
until the mission enters effect assessment and success. Both UE and Python RNGs
are initialized from the episode seed.

## JSONL logs

Scripted and stress runs write UTF-8, one-object-per-line logs below
`Saved/TacticalMARL/Logs`. Step records contain the episode/seed/step,
simulation time, observations, submitted actions, rewards, done flags and
reason, mission phase, red target health, reward components and diagnostics.
Episode start/end records contain outcome, total reward, steps and duration.

Read and summarize a trajectory without loading it all into memory:

```powershell
python summarize_jsonl.py ..\..\..\Saved\TacticalMARL\Logs\scripted_trajectory.jsonl
```

## Automated stress test

With the test map running, the following command performs the full
`reset -> scripted policy -> step -> done -> reset` loop 1000 times:

```powershell
python stress_test.py --episodes 1000 --seed 42
```

Use `--episodes` and `--seed` for shorter/reproducible runs. The runner checks
success rate, returns, steps, timeouts, UDP/reset errors, duplicate Agent IDs or
Controllers, red-force reset state and sustained process-memory growth. Any
failed gate produces a non-zero exit code. Reports are written to
`Saved/TacticalMARL/Reports/stress_summary.json` and `stress_summary.md`.
