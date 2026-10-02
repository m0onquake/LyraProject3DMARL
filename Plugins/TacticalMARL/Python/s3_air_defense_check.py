"""Live S3 acceptance for the deterministic D2 Anti-UAV launcher."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any, Callable, Mapping

from tactical_marl_env import TacticalMARLEnv


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_REPORT = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Reports" / "s3_air_defense_check.json"
DEFAULT_JSONL = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Logs" / "s3_air_defense_seed42.jsonl"
TARGET_ID = "BLUE_UAV_01"


def defense(response: Mapping[str, Any]) -> dict[str, Any]:
    return dict(response.get("red_air_defense", {}))


def target_observation(response: Mapping[str, Any]) -> dict[str, Any]:
    for item in response.get("observations", {}).get("agents", []):
        if item.get("agent_id") == TARGET_ID:
            return dict(item)
    return {}


def target_health(response: Mapping[str, Any]) -> float:
    return float(target_observation(response).get("health", 0.0))


def snapshot(record_type: str, response: Mapping[str, Any], **extra: Any) -> dict[str, Any]:
    target = target_observation(response)
    return {
        "record_type": record_type,
        "episode": response.get("episode", {}),
        "red_threat": response.get("red_threat", {}),
        "red_air_defense": response.get("red_air_defense", {}),
        "target": {
            "agent_id": target.get("agent_id"),
            "role": target.get("role"),
            "health": target.get("health"),
            "disabled": target.get("disabled"),
            "velocity": target.get("velocity"),
            "threat": target.get("threat"),
        },
        **extra,
    }


def poll_until(
    env: TacticalMARLEnv,
    predicate: Callable[[dict[str, Any]], bool],
    timeout: float,
    records: list[dict[str, Any]],
    record_type: str,
) -> tuple[dict[str, Any], float]:
    started = time.monotonic()
    last: dict[str, Any] = {}
    while time.monotonic() - started < timeout:
        last = env.state()
        records.append(snapshot(record_type, last, elapsed=time.monotonic() - started))
        if predicate(last):
            return last, time.monotonic() - started
        time.sleep(0.05)
    return last, time.monotonic() - started


def resolved_shot(response: Mapping[str, Any]) -> bool:
    state = defense(response)
    return int(state.get("shot_count", 0)) >= 1 and state.get("last_shot_result") not in {"none", "in_flight"}


def run_visible_shot(
    env: TacticalMARLEnv,
    seed: int,
    records: list[dict[str, Any]],
    label: str,
) -> tuple[dict[str, Any], float, bool, bool]:
    env.reset(seed=seed)
    env._request({"request": "s3_test_stage", "stage": "visible_hit"})
    saw_warning = False
    saw_incoming = False
    started = time.monotonic()
    result: dict[str, Any] = {}
    while time.monotonic() - started < 9.0:
        result = env.state()
        state = defense(result)
        target_threat = target_observation(result).get("threat", {})
        saw_warning |= state.get("state") == "warning" and bool(target_threat.get("locked"))
        saw_incoming |= state.get("state") == "launching" and bool(target_threat.get("incoming"))
        records.append(snapshot(label, result, elapsed=time.monotonic() - started))
        if resolved_shot(result):
            break
        time.sleep(0.05)
    return result, time.monotonic() - started, saw_warning, saw_incoming


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--jsonl", type=Path, default=DEFAULT_JSONL)
    args = parser.parse_args()

    errors: list[str] = []
    records: list[dict[str, Any]] = []
    details: dict[str, Any] = {}
    env = TacticalMARLEnv(args.host, args.port, decision_interval=0.0)
    try:
        env.wait_until_ready()

        env.reset(seed=args.seed)
        initial = env.last_response
        initial_defense = defense(initial)
        records.append(snapshot("initial_reset", initial))
        if not initial_defense.get("enabled") or initial_defense.get("difficulty") != "D2":
            errors.append(f"S3 D2 is not enabled: {initial_defense}")
        if initial_defense.get("state") != "scanning":
            errors.append(f"reset state is not scanning: {initial_defense.get('state')}")
        if int(initial_defense.get("ammo_remaining", -1)) != int(initial_defense.get("ammo_capacity", -2)):
            errors.append("reset did not restore full ammunition")
        if int(initial_defense.get("shot_count", -1)) != 0 or float(initial_defense.get("heat", -1.0)) != 0.0:
            errors.append("reset did not clear shot count and heat")

        # Occlusion: the launcher may know an old shared location, but cannot
        # confirm, lock or fire without its own direct S2 contact.
        env._request({"request": "s3_test_stage", "stage": "occluded"})
        occluded_health = target_health(env.last_response)
        time.sleep(3.0)
        occluded = env.state()
        records.append(snapshot("occluded_no_fire", occluded))
        if int(defense(occluded).get("shot_count", 0)) != 0:
            errors.append("launcher fired while target was occluded")
        if target_health(occluded) != occluded_health:
            errors.append("occluded target took damage")
        launcher_sensor = next(
            (item for item in occluded.get("red_threat", {}).get("sensors", [])
             if item.get("sensor_agent_id") == "RED_ANTIUAV_01"),
            {},
        )
        if launcher_sensor.get("direct_los"):
            errors.append("launcher sensor reported direct LOS through blocker")

        first, first_seconds, saw_warning, saw_incoming = run_visible_shot(
            env, args.seed, records, "visible_hit_a"
        )
        first_state = defense(first)
        first_health = target_health(first)
        if not resolved_shot(first):
            errors.append(f"visible target did not produce a resolved shot: {first_state}")
        if first_state.get("last_shot_result") != "hit" or int(first_state.get("hit_count", 0)) != 1:
            errors.append(f"seed-42 visible shot did not hit: {first_state}")
        if first_health >= 100.0:
            errors.append(f"S1 health interface did not receive S3 damage: health={first_health}")
        if not saw_warning:
            errors.append("lock warning was not observable in target threat observation")
        if not saw_incoming:
            errors.append("incoming missile was not observable in target threat observation")
        expected_sequence = ["confirming", "locking", "warning", "launching", "cooldown"]
        events = [str(item).removeprefix("air_defense:") for item in first_state.get("event_sequence", [])]
        positions = [events.index(item) if item in events else -1 for item in expected_sequence]
        if any(index < 0 for index in positions) or positions != sorted(positions):
            errors.append(f"attack state sequence incomplete or unordered: {events}")
        shot_count = int(first_state.get("shot_count", 0))
        time.sleep(1.0)
        cooldown_probe = env.state()
        records.append(snapshot("cooldown_probe", cooldown_probe))
        if int(defense(cooldown_probe).get("shot_count", 0)) != shot_count:
            errors.append("launcher fired again before cooldown completed")

        # Same seed and same stationary trajectory must reproduce the roll,
        # result and state sequence.
        repeated, _, _, _ = run_visible_shot(env, args.seed, records, "visible_hit_b")
        repeated_state = defense(repeated)
        first_canonical = [item for item in first_state.get("event_sequence", []) if item != "air_defense:scanning"]
        repeat_canonical = [item for item in repeated_state.get("event_sequence", []) if item != "air_defense:scanning"]
        if first_state.get("last_random_roll") != repeated_state.get("last_random_roll"):
            errors.append("same seed did not reproduce the hit roll")
        if first_state.get("last_shot_result") != repeated_state.get("last_shot_result"):
            errors.append("same seed did not reproduce the shot result")
        if first_canonical != repeat_canonical:
            errors.append(f"same-seed attack sequence differs: {first_canonical} != {repeat_canonical}")

        # Evasion uses real lateral UAV motion and must lower the explainable
        # probability or break the lock, without increasing damage.
        env.reset(seed=args.seed)
        env._request({"request": "s3_test_stage", "stage": "evade"})
        evade_start_health = target_health(env.last_response)
        evade, evade_seconds = poll_until(
            env,
            lambda response: resolved_shot(response)
            or defense(response).get("state") == "lost_lock",
            9.0,
            records,
            "evade",
        )
        evade_state = defense(evade)
        evasion_succeeded = evade_state.get("state") == "lost_lock" or (
            evade_state.get("last_shot_result") != "hit" and target_health(evade) == evade_start_health
        )
        if not evasion_succeeded:
            errors.append(f"lateral evasion did not avoid the attack: {evade_state}")
        if resolved_shot(evade) and float(evade_state.get("last_hit_probability", 1.0)) >= float(first_state.get("last_hit_probability", 0.0)):
            errors.append("evasion did not reduce hit probability")

        # Lost lock during the warning window must cancel before launch.
        env.reset(seed=args.seed)
        env._request({"request": "s3_test_stage", "stage": "visible_hit"})
        warning, warning_seconds = poll_until(
            env,
            lambda response: defense(response).get("state") == "warning",
            7.0,
            records,
            "lost_lock_setup",
        )
        pre_lost_shots = int(defense(warning).get("shot_count", 0))
        pre_lost_health = target_health(warning)
        if defense(warning).get("state") != "warning":
            errors.append("lost-lock scenario never reached warning")
        else:
            env._request({"request": "s3_test_stage", "stage": "lost_lock"})
            lost, lost_seconds = poll_until(
                env,
                lambda response: defense(response).get("state") == "lost_lock",
                2.0,
                records,
                "lost_lock",
            )
            if defense(lost).get("state") != "lost_lock":
                errors.append("LOS blocker did not break the lock")
            time.sleep(1.0)
            lost_probe = env.state()
            records.append(snapshot("lost_lock_no_fire", lost_probe))
            if int(defense(lost_probe).get("shot_count", 0)) != pre_lost_shots:
                errors.append("launcher fired after LOS was lost during warning")
            if target_health(lost_probe) != pre_lost_health:
                errors.append("target took damage after warning lock was broken")

        env.reset(seed=args.seed)
        final = env.last_response
        final_state = defense(final)
        records.append(snapshot("final_reset", final))
        if final_state.get("state") != "scanning":
            errors.append("final reset did not restore scanning")
        if int(final_state.get("ammo_remaining", -1)) != int(final_state.get("ammo_capacity", -2)):
            errors.append("final reset did not restore ammunition")
        if int(final_state.get("shot_count", -1)) != 0 or int(final_state.get("hit_count", -1)) != 0:
            errors.append("final reset did not clear shot/hit counters")
        if final_state.get("target_agent_id") != "None" or final_state.get("last_perceived_location") is not None:
            errors.append("final reset did not clear target and last perceived location")
        if target_health(final) != 100.0:
            errors.append("final reset did not restore UAV health")

        details = {
            "visible_shot_seconds": first_seconds,
            "visible_hit_probability": first_state.get("last_hit_probability"),
            "visible_random_roll": first_state.get("last_random_roll"),
            "visible_result": first_state.get("last_shot_result"),
            "health_after_hit": first_health,
            "warning_observed": saw_warning,
            "incoming_observed": saw_incoming,
            "same_seed_roll_equal": first_state.get("last_random_roll") == repeated_state.get("last_random_roll"),
            "same_seed_sequence_equal": first_canonical == repeat_canonical,
            "evade_seconds": evade_seconds,
            "evade_state": evade_state.get("state"),
            "evade_result": evade_state.get("last_shot_result"),
            "evade_probability": evade_state.get("last_hit_probability"),
            "lost_lock_warning_seconds": warning_seconds,
            "cooldown_seconds": first_state.get("cooldown_remaining_seconds"),
            "ammo_after_shot": first_state.get("ammo_remaining"),
            "heat_after_shot": first_state.get("heat"),
        }
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        errors.append(f"runtime check failed: {error}")
    finally:
        env.close()

    args.jsonl.parent.mkdir(parents=True, exist_ok=True)
    with args.jsonl.open("w", encoding="utf-8", newline="\n") as stream:
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n")
    parsed = 0
    with args.jsonl.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                json.loads(line)
                parsed += 1
            except json.JSONDecodeError as error:
                errors.append(f"JSONL line {line_number} invalid: {error}")

    report = {
        "stage": "S3",
        "difficulty": "D2",
        "seed": args.seed,
        "passed": not errors,
        "errors": errors,
        "details": details,
        "jsonl": str(args.jsonl),
        "jsonl_records": parsed,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
