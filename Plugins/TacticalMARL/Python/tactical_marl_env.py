"""Small PettingZoo-style parallel environment adapter for TacticalMARL.

No third-party package is required. Unreal must be running in PIE or Game mode.
The adapter applies all actions, lets the real-time world evolve for one decision
interval, then advances the episode and returns the resulting transition.
"""

from __future__ import annotations

import json
import socket
import time
from typing import Any, Mapping


Action = Mapping[str, Any]


class TacticalMARLEnv:
    metadata = {"name": "TacticalMARL-v1", "is_parallelizable": True}

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 7777,
        decision_interval: float = 0.1,
        timeout: float = 2.0,
        startup_retry_seconds: float = 10.0,
    ) -> None:
        self.address = (host, port)
        self.decision_interval = decision_interval
        self.startup_retry_seconds = startup_retry_seconds
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Explicitly bind so non-blocking stale-datagram draining is valid on
        # Windows before the first sendto() implicitly assigns a local port.
        self.socket.bind(("127.0.0.1", 0))
        self.timeout = max(5.0, timeout)
        self.socket.settimeout(self.timeout)
        self.agents: list[str] = []
        self.last_response: dict[str, Any] = {}
        self.udp_error_count = 0

    def close(self) -> None:
        self.socket.close()

    def _request(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        try:
            # Discard late replies from an earlier timed-out startup request.
            self.socket.setblocking(False)
            try:
                while True:
                    self.socket.recvfrom(65507)
            except BlockingIOError:
                pass
            finally:
                self.socket.settimeout(self.timeout)
            self.socket.sendto(
                json.dumps(payload, separators=(",", ":")).encode("utf-8"),
                self.address,
            )
            data, _ = self.socket.recvfrom(65507)
            response = json.loads(data.decode("utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            self.udp_error_count += 1
            raise
        self.last_response = response
        if not response.get("ok", False):
            raise RuntimeError(response.get("error", "TacticalMARL request failed"))
        return response

    @staticmethod
    def _observations(response: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
        agents = response.get("observations", {}).get("agents", [])
        return {str(item["agent_id"]): item for item in agents}

    def reset(self, seed: int | None = None) -> tuple[dict[str, Any], dict[str, Any]]:
        deadline = time.monotonic() + self.startup_retry_seconds
        while True:
            try:
                response = self._request({"request": "reset", "seed": 0 if seed is None else seed})
                break
            except RuntimeError as error:
                if "combatants_not_ready" not in str(error) or time.monotonic() >= deadline:
                    raise
                time.sleep(0.25)
        observations = self._observations(response)
        self.agents = list(observations)
        infos = {
            agent: {
                "episode": response["episode"],
                "mission": response.get("mission", {}),
                "diagnostics": response.get("diagnostics", {}),
            }
            for agent in self.agents
        }
        return observations, infos

    def step(
        self, actions: Mapping[str, Action]
    ) -> tuple[
        dict[str, Any],
        dict[str, float],
        dict[str, bool],
        dict[str, bool],
        dict[str, Any],
    ]:
        batch = []
        for agent_id, action in actions.items():
            item = dict(action)
            item["agent_id"] = agent_id
            batch.append(item)
        self._request({"request": "actions", "actions": batch})
        if self.decision_interval > 0.0:
            time.sleep(self.decision_interval)
        response = self._request({"request": "advance"})
        observations = self._observations(response)
        rewards = {str(k): float(v) for k, v in response.get("rewards", {}).items()}
        terminations = {str(k): bool(v) for k, v in response.get("terminations", {}).items()}
        truncations = {str(k): bool(v) for k, v in response.get("truncations", {}).items()}
        infos = {
            agent: {
                "episode": response["episode"],
                "mission": response.get("mission", {}),
                "diagnostics": response.get("diagnostics", {}),
            }
            for agent in self.agents
        }
        if terminations.get("__all__") or truncations.get("__all__"):
            self.agents = []
        return observations, rewards, terminations, truncations, infos

    def state(self) -> dict[str, Any]:
        """Return the centralized state used by a critic or evaluator."""
        return self._request({"request": "observations"})

    def wait_until_ready(
        self,
        minimum_world_seconds: float = 35.0,
        timeout_seconds: float = 90.0,
    ) -> dict[str, Any]:
        """Wait for Lyra warmup and red GAS initialization to finish."""
        deadline = time.monotonic() + timeout_seconds
        last_reason = "no_response"
        while time.monotonic() < deadline:
            try:
                response = self.state()
                diagnostics = response.get("diagnostics", {})
                targets = response.get("mission", {}).get("targets", [])
                agents = response.get("observations", {}).get("agents", [])
                world_ready = float(diagnostics.get("world_time_seconds", 0.0)) >= minimum_world_seconds
                providers_ready = bool(targets)
                if world_ready and providers_ready and agents:
                    return response
                last_reason = f"world_ready={world_ready}, providers_ready={providers_ready}, agents={len(agents)}"
            except (OSError, RuntimeError, ValueError) as error:
                last_reason = str(error)
            time.sleep(0.25)
        raise RuntimeError(f"environment_not_ready: {last_reason}")

    def end(self, reason: str = "evaluation_complete") -> dict[str, Any]:
        """End the running episode through the same public JSON protocol."""
        return self._request({"request": "end", "reason": reason})
