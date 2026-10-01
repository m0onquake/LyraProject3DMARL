"""Live S1 acceptance check for health, disabled action masks, and reset stability."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Mapping

from tactical_marl_env import TacticalMARLEnv


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_REPORT = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Reports" / "s1_health_check.json"
DEFAULT_JSONL = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Logs" / "s1_health_demo_seed42.jsonl"
TARGETS = ("BLUE_UAV_01", "BLUE_UGV_01")


def observations(response: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item["agent_id"]): dict(item)
        for item in response.get("observations", {}).get("agents", [])
    }


def state_snapshot(response: Mapping[str, Any]) -> dict[str, Any]:
    agents = observations(response)
    return {
        "episode": response.get("episode", {}),
        "agents": agents,
        "diagnostics": response.get("diagnostics", {}),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--resets", type=int, default=10)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--jsonl", type=Path, default=DEFAULT_JSONL)
    args = parser.parse_args()

    errors: list[str] = []
    records: list[dict[str, Any]] = []
    action_rejections: dict[str, str] = {}
    reset_checks: list[dict[str, Any]] = []
    env = TacticalMARLEnv(args.host, args.port, decision_interval=0.0)
    try:
        env.wait_until_ready()
        baseline, _ = env.reset(seed=args.seed)
        baseline_response = env.last_response
        baseline_positions = {
            agent_id: list(agent["location"])
            for agent_id, agent in observations(baseline_response).items()
        }
        records.append({"record_type": "before_damage", **state_snapshot(baseline_response)})

        for target in TARGETS:
            response = env._request({"request": "apply_test_damage", "agent_id": target, "damage": 35})
            records.append({"record_type": "partial_damage", "target": target, **state_snapshot(response)})
            agent = observations(response).get(target, {})
            if float(agent.get("health", -1)) != 65.0 or not agent.get("alive") or agent.get("disabled"):
                errors.append(f"{target} partial-damage state invalid: {agent}")
            if str(agent.get("last_damage_source")) != "S1_TEST_DAMAGE":
                errors.append(f"{target} damage source missing: {agent.get('last_damage_source')!r}")

        for target in TARGETS:
            response = env._request({"request": "apply_test_damage", "agent_id": target, "damage": 75})
            records.append({"record_type": "disabled", "target": target, **state_snapshot(response)})
            agent = observations(response).get(target, {})
            if float(agent.get("health", -1)) != 0.0 or agent.get("alive") or not agent.get("disabled"):
                errors.append(f"{target} disabled state invalid: {agent}")
            action_mask = agent.get("action_mask", {})
            if not action_mask or any(bool(value) for value in action_mask.values()):
                errors.append(f"{target} disabled action mask permits an action: {action_mask}")
            try:
                env._request({
                    "request": "actions",
                    "actions": [{"agent_id": target, "task": "idle", "sequence_id": 999}],
                })
                errors.append(f"{target} accepted a normal action while disabled")
            except RuntimeError as error:
                action_rejections[target] = str(error)
                if "agent_disabled" not in str(error):
                    errors.append(f"{target} rejected action for unexpected reason: {error}")

        disabled_response = env.state()
        disabled_diagnostics = disabled_response.get("diagnostics", {})
        if int(disabled_diagnostics.get("blue_alive_count", -1)) != 4:
            errors.append(f"disabled blue_alive_count={disabled_diagnostics.get('blue_alive_count')!r}, expected 4")
        if int(disabled_diagnostics.get("blue_disabled_count", -1)) != 2:
            errors.append(f"disabled blue_disabled_count={disabled_diagnostics.get('blue_disabled_count')!r}, expected 2")

        for index in range(args.resets):
            env.reset(seed=args.seed)
            response = env.last_response
            agents = observations(response)
            diagnostics = response.get("diagnostics", {})
            check_errors: list[str] = []
            for agent_id, agent in agents.items():
                if float(agent.get("health", -1)) != 100.0 or not agent.get("alive") or agent.get("disabled"):
                    check_errors.append(f"{agent_id}: health/alive/disabled")
                if agent.get("task") != "idle" or agent.get("task_state") != "idle":
                    check_errors.append(f"{agent_id}: task={agent.get('task')}/{agent.get('task_state')}")
                actual = [float(value) for value in agent.get("location", [])]
                expected = [float(value) for value in baseline_positions.get(agent_id, [])]
                if len(actual) != 3 or len(expected) != 3 or any(abs(a - b) > 5.0 for a, b in zip(actual, expected)):
                    check_errors.append(f"{agent_id}: location={actual}, expected={expected}")
                if not agent.get("action_mask") or not all(bool(value) for value in agent["action_mask"].values()):
                    check_errors.append(f"{agent_id}: action mask not restored")
            if len(agents) != 6:
                check_errors.append(f"agent count={len(agents)}")
            for field, expected in (
                ("agent_actor_count", 6),
                ("unique_agent_id_count", 6),
                ("duplicate_agent_id_count", 0),
                ("duplicate_controller_count", 0),
                ("unpossessed_agent_count", 0),
                ("blue_alive_count", 6),
                ("blue_disabled_count", 0),
            ):
                if int(diagnostics.get(field, -1)) != expected:
                    check_errors.append(f"diagnostics.{field}={diagnostics.get(field)!r}, expected {expected}")
            reset_checks.append({"index": index + 1, "passed": not check_errors, "errors": check_errors})
            errors.extend(f"reset {index + 1}: {error}" for error in check_errors)
            records.append({"record_type": "reset", "reset_index": index + 1, **state_snapshot(response)})

    except (OSError, RuntimeError, ValueError, KeyError) as error:
        errors.append(f"runtime check failed: {error}")
    finally:
        env.close()

    args.jsonl.parent.mkdir(parents=True, exist_ok=True)
    with args.jsonl.open("w", encoding="utf-8", newline="\n") as stream:
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")

    parsed_records = 0
    with args.jsonl.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                json.loads(line)
                parsed_records += 1
            except json.JSONDecodeError as error:
                errors.append(f"JSONL line {line_number} invalid: {error}")

    report = {
        "stage": "S1",
        "difficulty": "D0",
        "seed": args.seed,
        "passed": not errors,
        "errors": errors,
        "targets": list(TARGETS),
        "action_rejections": action_rejections,
        "reset_checks": reset_checks,
        "jsonl": str(args.jsonl),
        "jsonl_records": parsed_records,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
