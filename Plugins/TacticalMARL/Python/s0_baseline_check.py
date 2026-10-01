"""Validate the live S0/D0 TacticalMARL roster and unchanged v1 response contract."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Mapping

from tactical_marl_env import TacticalMARLEnv


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_REPORT = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Reports" / "s0_baseline_check.json"
EXPECTED_BLUE_IDS = {
    "BLUE_UAV_01",
    "BLUE_UAV_02",
    "BLUE_UAV_03",
    "BLUE_UGV_01",
    "BLUE_UGV_02",
    "BLUE_UGV_03",
}
EXPECTED_RED_IDS = {
    "RED_RIFLE_01",
    "RED_RIFLE_02",
    "RED_RIFLE_03",
    "RED_SNIPER_01",
    "RED_ANTIUAV_01",
}
EXPECTED_BLUE_DEPLOYMENT = {
    "BLUE_UAV_01": (-8300.0, -7200.0, 450.0),
    "BLUE_UAV_02": (-7300.0, -7200.0, 550.0),
    "BLUE_UAV_03": (-6300.0, -7200.0, 650.0),
    "BLUE_UGV_01": (-8300.0, -8750.0, 100.0),
    "BLUE_UGV_02": (-7300.0, -8750.0, 100.0),
    "BLUE_UGV_03": (-6300.0, -8750.0, 100.0),
}
EXPECTED_RED_DEPLOYMENT = {
    "RED_RIFLE_01": (6500.0, 7600.0, 120.0),
    "RED_RIFLE_02": (7000.0, 7600.0, 120.0),
    "RED_RIFLE_03": (7500.0, 7600.0, 120.0),
    "RED_SNIPER_01": (6750.0, 8100.0, 120.0),
    "RED_ANTIUAV_01": (7250.0, 8100.0, 120.0),
}
REQUIRED_ROOT_FIELDS = {
    "ok",
    "observations",
    "episode",
    "rewards",
    "terminations",
    "truncations",
    "infos",
    "mission",
    "diagnostics",
}
REQUIRED_AGENT_FIELDS = {
    "agent_id",
    "agent_type",
    "role",
    "sequence_id",
    "task",
    "task_state",
    "task_elapsed",
    "location",
    "velocity",
    "detected_target_count",
    "mission",
}


def _check_deployment(
    errors: list[str],
    agent_id: str,
    location: Any,
    expected: Mapping[str, tuple[float, float, float]],
) -> None:
    if agent_id not in expected or not isinstance(location, list) or len(location) != 3:
        return
    delta = sum((float(location[index]) - expected[agent_id][index]) ** 2 for index in range(3)) ** 0.5
    if delta > 5.0:
        errors.append(
            f"{agent_id} deployment moved: {location}, expected {list(expected[agent_id])} (delta {delta:.2f} cm)"
        )


def validate_response(response: Mapping[str, Any], seed: int) -> list[str]:
    errors: list[str] = []
    missing_root = sorted(REQUIRED_ROOT_FIELDS - set(response))
    if missing_root:
        errors.append(f"missing root fields: {missing_root}")

    episode = response.get("episode", {})
    if int(episode.get("seed", -1)) != seed:
        errors.append(f"episode seed is {episode.get('seed')!r}, expected {seed}")
    if episode.get("phase") != "running":
        errors.append(f"episode phase is {episode.get('phase')!r}, expected 'running'")

    agents = response.get("observations", {}).get("agents", [])
    blue_ids = [str(item.get("agent_id")) for item in agents]
    if len(agents) != 6:
        errors.append(f"blue agent count is {len(agents)}, expected 6")
    if set(blue_ids) != EXPECTED_BLUE_IDS:
        errors.append(f"blue IDs differ: {sorted(blue_ids)}")
    if len(blue_ids) != len(set(blue_ids)):
        errors.append("duplicate blue Agent ID")
    for agent in agents:
        missing_agent = sorted(REQUIRED_AGENT_FIELDS - set(agent))
        if missing_agent:
            errors.append(f"{agent.get('agent_id', '<unknown>')} missing fields: {missing_agent}")
        location = agent.get("location", [])
        velocity = agent.get("velocity", [])
        if len(location) != 3 or len(velocity) != 3:
            errors.append(f"{agent.get('agent_id', '<unknown>')} has non-XYZ location/velocity")
        _check_deployment(errors, str(agent.get("agent_id")), location, EXPECTED_BLUE_DEPLOYMENT)

    targets = response.get("mission", {}).get("targets", [])
    red_ids = [str(item.get("agent_id")) for item in targets]
    if len(targets) != 5:
        errors.append(f"red unit count is {len(targets)}, expected 5")
    if set(red_ids) != EXPECTED_RED_IDS:
        errors.append(f"red IDs differ: {sorted(red_ids)}")
    if len(red_ids) != len(set(red_ids)):
        errors.append("duplicate red Agent ID")
    role_counts = {"MARL.Role.Rifleman": 0, "MARL.Role.Sniper": 0, "MARL.Role.AntiUAV": 0}
    for target in targets:
        role = str(target.get("role"))
        if role in role_counts:
            role_counts[role] += 1
        if not target.get("alive", False):
            errors.append(f"{target.get('agent_id')} is not alive after reset")
        if float(target.get("max_health", 0.0)) <= 0.0:
            errors.append(f"{target.get('agent_id')} has no valid health")
        _check_deployment(
            errors,
            str(target.get("agent_id")),
            target.get("location", []),
            EXPECTED_RED_DEPLOYMENT,
        )
    if role_counts != {"MARL.Role.Rifleman": 3, "MARL.Role.Sniper": 1, "MARL.Role.AntiUAV": 1}:
        errors.append(f"red role counts differ: {role_counts}")

    diagnostics = response.get("diagnostics", {})
    for field in ("duplicate_agent_id_count", "duplicate_controller_count", "unpossessed_agent_count"):
        if int(diagnostics.get(field, -1)) != 0:
            errors.append(f"diagnostics.{field}={diagnostics.get(field)!r}, expected 0")
    if int(diagnostics.get("agent_actor_count", -1)) != 6:
        errors.append(f"diagnostics.agent_actor_count={diagnostics.get('agent_actor_count')!r}, expected 6")
    if response.get("infos", {}).get("api") != "tactical_marl_v1":
        errors.append("infos.api is not tactical_marl_v1")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    args = parser.parse_args()

    env = TacticalMARLEnv(args.host, args.port, decision_interval=0.0)
    try:
        env.wait_until_ready()
        env.reset(seed=args.seed)
        response = env.last_response
        errors = validate_response(response, args.seed)
    except (OSError, RuntimeError, ValueError) as error:
        errors = [f"runtime check failed: {error}"]
        response = {}
    finally:
        env.close()

    report = {
        "stage": "S0",
        "difficulty": "D0",
        "seed": args.seed,
        "passed": not errors,
        "errors": errors,
        "episode": response.get("episode", {}),
        "blue_agent_ids": sorted(
            str(item.get("agent_id"))
            for item in response.get("observations", {}).get("agents", [])
        ),
        "red_agent_ids": sorted(
            str(item.get("agent_id"))
            for item in response.get("mission", {}).get("targets", [])
        ),
        "diagnostics": response.get("diagnostics", {}),
        "api": response.get("infos", {}).get("api"),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
