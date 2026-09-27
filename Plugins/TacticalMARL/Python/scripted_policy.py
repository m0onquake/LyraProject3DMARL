"""Deterministic recon-strike baseline using only TacticalMARL JSON/UDP actions."""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from pathlib import Path
from typing import Any, Mapping

from tactical_marl_env import TacticalMARLEnv


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_LOG_DIR = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Logs"


class JsonlTrajectoryLogger:
    """Append UTF-8 JSON objects; every physical line is independently readable."""

    def __init__(self, path: Path, mode: str = "a") -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._stream = path.open(mode, encoding="utf-8", newline="\n", buffering=1)

    def close(self) -> None:
        self._stream.close()

    def write(self, record: Mapping[str, Any]) -> None:
        self._stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")

    def episode_start(self, response: Mapping[str, Any]) -> None:
        episode = response.get("episode", {})
        self.write(
            {
                "record_type": "episode_start",
                "episode_id": episode.get("episode_id"),
                "seed": episode.get("seed"),
                "simulation_time": episode.get("elapsed_seconds", 0.0),
                "agents": [
                    item.get("agent_id")
                    for item in response.get("observations", {}).get("agents", [])
                ],
                "red_targets": response.get("mission", {}).get("targets", []),
            }
        )

    def step(
        self,
        response: Mapping[str, Any],
        actions: Mapping[str, Mapping[str, Any]],
    ) -> None:
        episode = response.get("episode", {})
        observations = {
            str(item.get("agent_id")): item
            for item in response.get("observations", {}).get("agents", [])
        }
        logged_actions = {
            agent_id: dict(
                actions.get(
                    agent_id,
                    {"task": "continue", "current_task": observation.get("task", "idle")},
                )
            )
            for agent_id, observation in observations.items()
        }
        terminations = response.get("terminations", {})
        truncations = response.get("truncations", {})
        mission = response.get("mission", {})
        done = bool(terminations.get("__all__") or truncations.get("__all__"))
        self.write(
            {
                "record_type": "step",
                "episode_id": episode.get("episode_id"),
                "seed": episode.get("seed"),
                "step": episode.get("step"),
                "simulation_time": episode.get("elapsed_seconds", 0.0),
                "observations": observations,
                "actions": logged_actions,
                "rewards": response.get("rewards", {}),
                "done": done,
                "termination_reason": episode.get("end_reason", ""),
                "mission_phase": mission.get("phase", "unknown"),
                "red_targets": mission.get("targets", []),
                "reward_components": mission.get("reward_components", {}),
                "terminations": terminations,
                "truncations": truncations,
                "diagnostics": response.get("diagnostics", {}),
            }
        )

    def episode_end(
        self,
        response: Mapping[str, Any],
        total_rewards: Mapping[str, float],
    ) -> None:
        episode = response.get("episode", {})
        mission = response.get("mission", {})
        success = bool(mission.get("success"))
        self.write(
            {
                "record_type": "episode_end",
                "episode_id": episode.get("episode_id"),
                "seed": episode.get("seed"),
                "success": success,
                "failure": not success,
                "total_reward": sum(total_rewards.values()),
                "agent_total_rewards": dict(total_rewards),
                "steps": episode.get("step", 0),
                "duration": episode.get("elapsed_seconds", 0.0),
                "termination_reason": episode.get("end_reason", ""),
                "mission_phase": mission.get("phase", "unknown"),
                "red_targets": mission.get("targets", []),
            }
        )


class ReconStrikeScriptedPolicy:
    """TacML-inspired search, confirm, track, approach, engage and assess policy."""

    SEARCH_POINTS = (
        (-8000.0, -8000.0), (-4000.0, -8000.0), (0.0, -8000.0),
        (4000.0, -8000.0), (8000.0, -8000.0), (8000.0, -4000.0),
        (4000.0, -4000.0), (0.0, -4000.0), (-4000.0, -4000.0),
        (-8000.0, -4000.0), (-8000.0, 0.0), (-4000.0, 0.0),
        (0.0, 0.0), (4000.0, 0.0), (8000.0, 0.0),
        (8000.0, 4000.0), (4000.0, 4000.0), (0.0, 4000.0),
        (-4000.0, 4000.0), (-8000.0, 4000.0), (-8000.0, 8000.0),
        (-4000.0, 8000.0), (0.0, 8000.0), (4000.0, 8000.0),
        (8000.0, 8000.0),
    )
    # UrbanDepot CandidateArea quadrants.  Every point is within the 30 m UAV
    # sensor radius of the objective cluster, but no exact target position is
    # used before the mission reports candidate_found.
    CANDIDATE_SEARCH_POINTS = (
        (6000.0, 6000.0), (8000.0, 6000.0),
        (6000.0, 8000.0), (8000.0, 8000.0),
    )

    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.sequence: dict[str, int] = {}
        self.agent_order: list[str] = []
        self.search_offset = self.rng.randrange(len(self.SEARCH_POINTS))

    def _next_sequence(self, agent_id: str) -> int:
        value = self.sequence.get(agent_id, 0) + 1
        self.sequence[agent_id] = value
        return value

    @staticmethod
    def _objective_location(mission: Mapping[str, Any]) -> list[float] | None:
        targets = [target for target in mission.get("targets", []) if target.get("alive", True)]
        explicit = [target for target in targets if target.get("mission_objective")]
        selected = explicit or targets
        location = selected[0].get("location") if selected else None
        return [float(v) for v in location[:3]] if location and len(location) >= 3 else None

    def _command(
        self,
        agent: Mapping[str, Any],
        task: str,
        target: list[float],
        role: str,
        **extra: Any,
    ) -> dict[str, Any]:
        command = {
            "sequence_id": self._next_sequence(str(agent["agent_id"])),
            "task": task,
            "target": target,
            "role": role,
        }
        command.update(extra)
        return command

    def actions(self, response: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
        agents = response.get("observations", {}).get("agents", [])
        if not self.agent_order:
            self.agent_order = sorted(str(agent["agent_id"]) for agent in agents)
        mission = response.get("mission", {})
        phase = str(mission.get("phase", "search"))
        step = int(response.get("episode", {}).get("step", 0))
        objective = self._objective_location(mission)
        alive_agents = sorted(agents, key=lambda item: self.agent_order.index(str(item["agent_id"])))
        strike_uavs = [agent for agent in alive_agents if str(agent.get("agent_type", "uav")) == "uav"]
        striker_id = (strike_uavs or alive_agents)[-1]["agent_id"] if alive_agents else ""
        uav_ids = [str(agent["agent_id"]) for agent in strike_uavs]
        result: dict[str, dict[str, Any]] = {}

        for index, agent in enumerate(alive_agents):
            agent_id = str(agent["agent_id"])
            agent_type = str(agent.get("agent_type", "uav"))
            current_task = str(agent.get("task", "idle"))
            task_state = str(agent.get("task_state", "idle"))
            busy = task_state in {"en_route", "executing"}
            if agent_type != "uav":
                role = "GroundSupportRole"
            elif phase in {"strike_ready", "engagement", "effect_assessment"} and agent_id == striker_id:
                role = "StrikeRole"
            elif uav_ids and agent_id == uav_ids[0]:
                role = "WideReconRole"
            else:
                role = "CloseReconRole"

            if phase in {"search", "candidate_found"} or objective is None:
                if agent_id not in self.sequence:
                    candidate_index = (self.search_offset + index) % len(self.CANDIDATE_SEARCH_POINTS)
                    x, y = self.CANDIDATE_SEARCH_POINTS[candidate_index]
                else:
                    point_index = (self.search_offset + index * 5 + step // 30) % len(self.SEARCH_POINTS)
                    x, y = self.SEARCH_POINTS[point_index]
                target = [x, y, 900.0 if agent_type == "uav" else 0.0]
                desired = "recon" if agent_type == "uav" else "patrol"
                if not busy:
                    result[agent_id] = self._command(
                        agent, desired, target, role, duration=2.0,
                        speed=1200.0 if agent_type == "uav" else 600.0,
                        acceptance_radius=250.0,
                        **({"altitude": 900.0, "orbit_radius": 500.0} if agent_type == "uav" else {"patrol_radius": 600.0}),
                    )
                continue

            if phase in {"target_confirmed", "track_established"}:
                desired = "surveillance"
                if not busy or current_task != desired:
                    result[agent_id] = self._command(
                        agent, desired, objective, role, duration=4.0,
                        speed=1000.0 if agent_type == "uav" else 500.0,
                        acceptance_radius=400.0,
                        **({"altitude": max(800.0, objective[2] + 800.0), "orbit_radius": 450.0} if agent_type == "uav" else {"patrol_radius": 450.0}),
                    )
                continue

            if phase in {"strike_ready", "engagement", "effect_assessment"} and agent_id == striker_id:
                desired = "strike" if agent_type == "uav" else "engage"
                if not busy or current_task != desired:
                    result[agent_id] = self._command(
                        agent, desired, objective, role,
                        speed=1200.0 if agent_type == "uav" else 600.0,
                        acceptance_radius=300.0,
                        **({"altitude": max(700.0, objective[2] + 700.0)} if agent_type == "uav" else {}),
                    )
            elif phase in {"strike_ready", "engagement", "effect_assessment"}:
                if not busy or current_task != "surveillance":
                    result[agent_id] = self._command(
                        agent, "surveillance", objective, role, duration=4.0,
                        acceptance_radius=500.0,
                        **({"altitude": max(900.0, objective[2] + 900.0), "orbit_radius": 550.0} if agent_type == "uav" else {"patrol_radius": 550.0}),
                    )
        return result


def run_episode(
    env: TacticalMARLEnv,
    seed: int,
    max_steps: int,
    logger: JsonlTrajectoryLogger | None = None,
) -> dict[str, Any]:
    env.reset(seed=seed)
    response = env.last_response
    initial_red_targets = response.get("mission", {}).get("targets", [])
    initial_agent_ids = [
        str(item.get("agent_id"))
        for item in response.get("observations", {}).get("agents", [])
    ]
    if logger:
        logger.episode_start(response)
    policy = ReconStrikeScriptedPolicy(seed)
    totals = {agent_id: 0.0 for agent_id in env.agents}
    for _ in range(max_steps):
        actions = policy.actions(response)
        env.step(actions)
        response = env.last_response
        for agent_id, reward in response.get("rewards", {}).items():
            totals[agent_id] = totals.get(agent_id, 0.0) + float(reward)
        if logger:
            logger.step(response, actions)
        done = response.get("terminations", {}).get("__all__") or response.get("truncations", {}).get("__all__")
        if done:
            break
    else:
        env.end("python_step_guard")
        response = env.last_response
    if logger:
        logger.episode_end(response, totals)
    return {
        "episode": response.get("episode", {}),
        "mission": response.get("mission", {}),
        "diagnostics": response.get("diagnostics", {}),
        "total_rewards": totals,
        "total_reward": sum(totals.values()),
        "initial_red_targets": initial_red_targets,
        "initial_agent_ids": initial_agent_ids,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--episodes", type=int, default=1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--decision-interval", type=float, default=0.1)
    parser.add_argument("--max-steps", type=int, default=1000)
    parser.add_argument(
        "--episode-end-delay",
        type=float,
        default=4.0,
        help="real-time seconds to keep the terminal episode visible before reset (default: 4)",
    )
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG_DIR / "scripted_trajectory.jsonl")
    args = parser.parse_args()
    if args.episode_end_delay < 0.0:
        parser.error("--episode-end-delay must be non-negative")
    logger = JsonlTrajectoryLogger(args.log)
    env = TacticalMARLEnv(args.host, args.port, args.decision_interval)
    successes = 0
    try:
        env.wait_until_ready()
        for index in range(args.episodes):
            result = run_episode(env, args.seed + index, args.max_steps, logger)
            successes += int(bool(result["mission"].get("success")))
            print(json.dumps(result, ensure_ascii=False))
            if index + 1 < args.episodes and args.episode_end_delay > 0.0:
                time.sleep(args.episode_end_delay)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"scripted policy failed: {error}", file=sys.stderr)
        return 2
    finally:
        env.close()
        logger.close()
    return 0 if successes == args.episodes else 1


if __name__ == "__main__":
    raise SystemExit(main())
