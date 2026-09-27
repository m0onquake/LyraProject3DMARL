"""Read TacticalMARL JSONL one line at a time and print an episode summary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from scripted_policy import DEFAULT_LOG_DIR


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, default=DEFAULT_LOG_DIR / "scripted_trajectory.jsonl")
    args = parser.parse_args()
    episodes = []
    steps = 0
    with args.path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise SystemExit(f"invalid JSONL at line {line_number}: {error}") from error
            steps += record.get("record_type") == "step"
            if record.get("record_type") == "episode_end":
                episodes.append(record)
    success_count = sum(bool(item.get("success")) for item in episodes)
    summary = {
        "path": str(args.path),
        "episodes": len(episodes),
        "steps": steps,
        "success_rate": success_count / len(episodes) if episodes else 0.0,
        "average_total_reward": sum(float(item.get("total_reward", 0.0)) for item in episodes) / len(episodes) if episodes else 0.0,
        "average_steps": sum(int(item.get("steps", 0)) for item in episodes) / len(episodes) if episodes else 0.0,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
