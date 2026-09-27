# TacticalMARL UAV/UGV runtime

`ATacticalUAVPawn` is the first policy-controlled agent used by the MARL test range.

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
