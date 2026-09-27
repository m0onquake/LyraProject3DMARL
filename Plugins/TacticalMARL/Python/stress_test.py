"""Run repeated TacticalMARL reset-policy-step episodes and emit JSON/Markdown reports."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from scripted_policy import DEFAULT_LOG_DIR, JsonlTrajectoryLogger, PROJECT_ROOT, run_episode
from tactical_marl_env import TacticalMARLEnv


DEFAULT_REPORT_DIR = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Reports"


def red_state_reset(targets: list[dict[str, Any]]) -> bool:
    return bool(targets) and all(
        bool(target.get("alive"))
        and float(target.get("health", -1.0)) >= float(target.get("max_health", 0.0)) - 0.01
        and int(target.get("deaths", 0)) == 0
        and str(target.get("last_killer_agent_id", "")) in {"", "None"}
        for target in targets
    )


def write_reports(summary: dict[str, Any], json_path: Path, markdown_path: Path) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    checks = summary["checks"]
    metrics = summary["metrics"]
    failures = summary["failure_reasons"]
    markdown = f"""# TacticalMARL stress test report

- Status: **{summary['status']}**
- Generated: {summary['generated_at_utc']}
- Requested episodes: {summary['requested_episodes']}
- Completed episodes: {summary['completed_episodes']}
- Base seed: {summary['base_seed']}

## Metrics

| Metric | Value |
|---|---:|
| Success rate | {metrics['success_rate']:.4f} |
| Average total reward | {metrics['average_total_reward']:.4f} |
| Average steps | {metrics['average_steps']:.2f} |
| Average duration seconds | {metrics['average_duration_seconds']:.3f} |
| Timeouts | {metrics['timeouts']} |
| UDP errors | {metrics['udp_errors']} |
| Reset failures | {metrics['reset_failures']} |
| Duplicate Agent IDs | {metrics['duplicate_agent_ids']} |
| Duplicate Controllers | {metrics['duplicate_controllers']} |
| Red reset failures | {metrics['red_reset_failures']} |
| Memory growth MiB | {metrics['memory_growth_mb']:.2f} |

## Checks

"""
    markdown += "\n".join(f"- {'PASS' if value else 'FAIL'}: `{name}`" for name, value in checks.items())
    markdown += "\n\n## Failure reasons\n\n"
    markdown += "\n".join(f"- {reason}" for reason in failures) if failures else "- None"
    markdown += f"\n\nTrajectory: `{summary['trajectory_log']}`\n"
    markdown_path.write_text(markdown + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--episodes", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--decision-interval", type=float, default=0.1)
    parser.add_argument("--max-steps", type=int, default=1000)
    parser.add_argument("--min-success-rate", type=float, default=0.95)
    parser.add_argument("--memory-growth-limit-mb", type=float, default=256.0)
    parser.add_argument("--trajectory", type=Path, default=DEFAULT_LOG_DIR / "stress_trajectory.jsonl")
    parser.add_argument("--json-report", type=Path, default=DEFAULT_REPORT_DIR / "stress_summary.json")
    parser.add_argument("--markdown-report", type=Path, default=DEFAULT_REPORT_DIR / "stress_summary.md")
    args = parser.parse_args()

    if args.episodes <= 0 or args.max_steps <= 0:
        parser.error("--episodes and --max-steps must be positive")

    env = TacticalMARLEnv(args.host, args.port, args.decision_interval)
    logger = JsonlTrajectoryLogger(args.trajectory, mode="w")
    results: list[dict[str, Any]] = []
    reset_failures = 0
    duplicate_agent_ids = 0
    duplicate_controllers = 0
    red_reset_failures = 0
    runtime_errors: list[str] = []
    memory_samples: list[float] = []
    try:
        environment_ready = True
        try:
            env.wait_until_ready()
        except (OSError, RuntimeError, ValueError) as error:
            runtime_errors.append(str(error))
            environment_ready = False
        for index in range(args.episodes if environment_ready else 0):
            episode_seed = args.seed + index
            try:
                result = run_episode(env, episode_seed, args.max_steps, logger)
            except RuntimeError as error:
                reset_failures += int("reset" in str(error) or "combatants_not_ready" in str(error))
                runtime_errors.append(f"seed {episode_seed}: {error}")
                continue
            except (OSError, ValueError, json.JSONDecodeError) as error:
                runtime_errors.append(f"seed {episode_seed}: {error}")
                break

            results.append(result)
            ids = result["initial_agent_ids"]
            diagnostics = result.get("diagnostics", {})
            duplicate_agent_ids += max(
                max(0, len(ids) - len(set(ids))),
                int(diagnostics.get("duplicate_agent_id_count", 0)),
            )
            duplicate_controllers += int(diagnostics.get("duplicate_controller_count", 0))
            if not red_state_reset(result["initial_red_targets"]):
                red_reset_failures += 1
            if "process_memory_mb" in diagnostics:
                memory_samples.append(float(diagnostics["process_memory_mb"]))
            if (index + 1) % max(1, min(25, args.episodes // 10 or 1)) == 0:
                print(f"completed {index + 1}/{args.episodes} episodes", flush=True)
    finally:
        logger.close()
        env.close()

    successes = sum(bool(item.get("mission", {}).get("success")) for item in results)
    timeouts = sum(
        item.get("episode", {}).get("end_reason") in {"time_limit", "step_limit", "python_step_guard"}
        for item in results
    )
    rewards = [float(item.get("total_reward", 0.0)) for item in results]
    steps = [int(item.get("episode", {}).get("step", 0)) for item in results]
    durations = [float(item.get("episode", {}).get("elapsed_seconds", 0.0)) for item in results]
    memory_growth_mb = 0.0
    monotonic_ratio = 0.0
    if len(memory_samples) >= 10:
        window = max(3, len(memory_samples) // 10)
        memory_growth_mb = statistics.median(memory_samples[-window:]) - statistics.median(memory_samples[:window])
        monotonic_ratio = sum(b >= a for a, b in zip(memory_samples, memory_samples[1:])) / (len(memory_samples) - 1)
    continuous_memory_growth = (
        len(memory_samples) >= 10
        and memory_growth_mb > args.memory_growth_limit_mb
        and monotonic_ratio >= 0.8
    )
    completed = len(results)
    success_rate = successes / completed if completed else 0.0
    checks = {
        "all_episodes_completed": completed == args.episodes,
        "success_rate": success_rate >= args.min_success_rate,
        "no_timeouts": timeouts == 0,
        "no_udp_errors": env.udp_error_count == 0,
        "no_reset_failures": reset_failures == 0,
        "no_duplicate_agents": duplicate_agent_ids == 0,
        "no_duplicate_controllers": duplicate_controllers == 0,
        "red_state_reset": red_reset_failures == 0,
        "no_continuous_memory_growth": not continuous_memory_growth,
        "no_runtime_errors": not runtime_errors,
    }
    failure_reasons = [name for name, passed in checks.items() if not passed]
    summary = {
        "schema_version": 1,
        "status": "PASS" if all(checks.values()) else "FAIL",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "requested_episodes": args.episodes,
        "completed_episodes": completed,
        "base_seed": args.seed,
        "trajectory_log": str(args.trajectory.resolve()),
        "metrics": {
            "successes": successes,
            "success_rate": success_rate,
            "average_total_reward": statistics.fmean(rewards) if rewards else 0.0,
            "average_steps": statistics.fmean(steps) if steps else 0.0,
            "average_duration_seconds": statistics.fmean(durations) if durations else 0.0,
            "timeouts": timeouts,
            "udp_errors": env.udp_error_count,
            "reset_failures": reset_failures,
            "duplicate_agent_ids": duplicate_agent_ids,
            "duplicate_controllers": duplicate_controllers,
            "red_reset_failures": red_reset_failures,
            "memory_first_mb": memory_samples[0] if memory_samples else 0.0,
            "memory_last_mb": memory_samples[-1] if memory_samples else 0.0,
            "memory_growth_mb": memory_growth_mb,
            "memory_monotonic_ratio": monotonic_ratio,
        },
        "checks": checks,
        "failure_reasons": failure_reasons,
        "runtime_errors": runtime_errors[:100],
        "episodes": [
            {
                "episode_id": item.get("episode", {}).get("episode_id"),
                "seed": item.get("episode", {}).get("seed"),
                "success": bool(item.get("mission", {}).get("success")),
                "total_reward": item.get("total_reward", 0.0),
                "steps": item.get("episode", {}).get("step", 0),
                "duration": item.get("episode", {}).get("elapsed_seconds", 0.0),
                "termination_reason": item.get("episode", {}).get("end_reason", ""),
            }
            for item in results
        ],
    }
    write_reports(summary, args.json_report, args.markdown_report)
    print(json.dumps(summary["metrics"], ensure_ascii=False, indent=2))
    print(f"JSON report: {args.json_report.resolve()}")
    print(f"Markdown report: {args.markdown_report.resolve()}")
    if failure_reasons:
        print("FAILED: " + ", ".join(failure_reasons), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
