"""Live S2 acceptance for local red sensing, LOS, delayed sharing, decay, and reset."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any, Callable, Mapping

from tactical_marl_env import TacticalMARLEnv


PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_REPORT = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Reports" / "s2_threat_check.json"
DEFAULT_JSONL = PROJECT_ROOT / "Saved" / "TacticalMARL" / "Logs" / "s2_threat_seed42.jsonl"


def threat(response: Mapping[str, Any]) -> dict[str, Any]:
    return dict(response.get("red_threat", {}))


def snapshot(record_type: str, response: Mapping[str, Any], **extra: Any) -> dict[str, Any]:
    return {
        "record_type": record_type,
        "episode": response.get("episode", {}),
        "red_threat": response.get("red_threat", {}),
        "diagnostics": response.get("diagnostics", {}),
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
        time.sleep(0.08)
    return last, time.monotonic() - started


def run_sequence(
    env: TacticalMARLEnv,
    seed: int,
    records: list[dict[str, Any]],
    label: str,
) -> tuple[list[str], dict[str, Any]]:
    env.reset(seed=seed)
    env._request({"request": "s2_test_stage", "stage": "occluded"})
    time.sleep(0.9)
    occluded = env.state()
    records.append(snapshot(f"{label}_occluded", occluded))
    env._request({"request": "s2_test_stage", "stage": "visible"})
    tracking, _ = poll_until(
        env,
        lambda response: response.get("red_threat", {}).get("shared_alert", {}).get("state") == "tracking",
        5.0,
        records,
        f"{label}_visible",
    )
    env._request({"request": "s2_test_stage", "stage": "lost_contact"})
    lost, _ = poll_until(
        env,
        lambda response: response.get("red_threat", {}).get("shared_alert", {}).get("state") == "lost_contact",
        3.0,
        records,
        f"{label}_lost",
    )
    sequence = [
        event
        for event in lost.get("red_threat", {}).get("event_sequence", [])
        if str(event).startswith("shared:")
    ]
    sequence = [event for index, event in enumerate(sequence) if index == 0 or event != sequence[index - 1]]
    return sequence, {"occluded": occluded, "tracking": tracking, "lost": lost}


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
        reset_response = env.last_response
        initial = threat(reset_response)
        records.append(snapshot("initial_reset", reset_response))
        if not initial.get("enabled") or initial.get("difficulty") != "D1":
            errors.append(f"S2 sensing is not enabled in D1 test mode: {initial}")
        if initial.get("shared_alert", {}).get("state") != "unaware":
            errors.append("shared alert is not unaware after reset")
        if initial.get("shared_alert", {}).get("last_known_location") is not None:
            errors.append("shared last-known location is not empty after reset")
        if len(initial.get("sensors", [])) != 5:
            errors.append(f"sensor count={len(initial.get('sensors', []))}, expected 5")

        env._request({"request": "s2_test_stage", "stage": "occluded"})
        time.sleep(1.1)
        occluded_response = env.state()
        occluded = threat(occluded_response)
        records.append(snapshot("occluded", occluded_response))
        if occluded.get("shared_alert", {}).get("state") != "unaware":
            errors.append(f"occluded target produced shared detection: {occluded.get('shared_alert')}")
        if any(sensor.get("direct_los") for sensor in occluded.get("sensors", [])):
            errors.append("occluded target produced direct LOS")
        if not any(sensor.get("last_trace_blocked") for sensor in occluded.get("sensors", [])):
            errors.append("occluded stage did not record a blocked visibility trace")

        env._request({"request": "s2_test_stage", "stage": "visible"})
        direct_started = time.monotonic()
        direct_response, direct_elapsed = poll_until(
            env,
            lambda response: any(
                sensor.get("state") in {"suspicious", "alerted", "tracking"}
                for sensor in response.get("red_threat", {}).get("sensors", [])
            ),
            2.0,
            records,
            "direct_detection",
        )
        shared_response, shared_elapsed_after_direct = poll_until(
            env,
            lambda response: response.get("red_threat", {}).get("shared_alert", {}).get("state") != "unaware",
            2.5,
            records,
            "shared_propagation",
        )
        shared_elapsed = time.monotonic() - direct_started
        shared = threat(shared_response).get("shared_alert", {})
        configured_delay = float(shared.get("propagation_delay", 0.0))
        if shared.get("state") == "unaware":
            errors.append("shared alert was not delivered")
        if configured_delay < 0.65 or configured_delay > 0.95:
            errors.append(f"seeded propagation delay {configured_delay:.3f} outside [0.65, 0.95]")
        if shared_elapsed + 0.12 < configured_delay:
            errors.append(
                f"shared alert arrived too early: elapsed={shared_elapsed:.3f}, configured={configured_delay:.3f}"
            )

        tracking_response, tracking_elapsed = poll_until(
            env,
            lambda response: response.get("red_threat", {}).get("shared_alert", {}).get("state") == "tracking",
            4.0,
            records,
            "tracking",
        )
        tracking_alert = threat(tracking_response).get("shared_alert", {})
        if tracking_alert.get("state") != "tracking":
            errors.append(f"shared alert never reached tracking: {tracking_alert}")
        tracking_confidence = float(tracking_alert.get("confidence", 0.0))

        env._request({"request": "s2_test_stage", "stage": "lost_contact"})
        lost_response, lost_elapsed = poll_until(
            env,
            lambda response: response.get("red_threat", {}).get("shared_alert", {}).get("state") == "lost_contact",
            3.0,
            records,
            "lost_contact",
        )
        lost_alert = threat(lost_response).get("shared_alert", {})
        if lost_alert.get("state") != "lost_contact":
            errors.append(f"shared alert did not enter lost_contact: {lost_alert}")
        lost_confidence = float(lost_alert.get("confidence", 0.0))
        time.sleep(0.8)
        decayed_response = env.state()
        decayed_confidence = float(threat(decayed_response).get("shared_alert", {}).get("confidence", 0.0))
        records.append(snapshot("confidence_decay", decayed_response))
        if decayed_confidence >= lost_confidence:
            errors.append(f"shared confidence did not decay: {lost_confidence} -> {decayed_confidence}")

        first_sequence, first_states = run_sequence(env, args.seed, records, "repeat_a")
        second_sequence, second_states = run_sequence(env, args.seed, records, "repeat_b")
        if first_sequence != second_sequence:
            errors.append(f"same-seed alert sequence differs: {first_sequence} != {second_sequence}")

        env.reset(seed=args.seed)
        final_response = env.last_response
        final_threat = threat(final_response)
        records.append(snapshot("final_reset", final_response))
        if final_threat.get("shared_alert", {}).get("state") != "unaware":
            errors.append("final reset did not restore shared unaware")
        if final_threat.get("shared_alert", {}).get("last_known_location") is not None:
            errors.append("final reset did not clear shared last-known location")
        for sensor in final_threat.get("sensors", []):
            if sensor.get("state") != "unaware" or sensor.get("target_agent_id") != "None":
                errors.append(f"final reset did not clear sensor: {sensor}")

        details = {
            "direct_detection_seconds": direct_elapsed,
            "shared_delivery_seconds_after_direct_poll": shared_elapsed_after_direct,
            "shared_delivery_seconds_total": shared_elapsed,
            "configured_propagation_delay": configured_delay,
            "tracking_seconds": tracking_elapsed,
            "lost_contact_seconds": lost_elapsed,
            "tracking_confidence": tracking_confidence,
            "lost_confidence": lost_confidence,
            "decayed_confidence": decayed_confidence,
            "deterministic_event_sequence": first_sequence,
            "repeat_sequences_equal": first_sequence == second_sequence,
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
        "stage": "S2",
        "difficulty": "D1",
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
