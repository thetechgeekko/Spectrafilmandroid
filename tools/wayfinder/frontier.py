#!/usr/bin/env python3
"""Print the live dependency-aware Wayfinder frontier for this repository."""

from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
from dataclasses import dataclass, replace
from typing import Any, Iterable
from urllib.parse import urlsplit


DEFAULT_REPO = "thetechgeekko/Spektrafilm-android"
DEFAULT_MAP = 164
DEFAULT_GH_TIMEOUT_SECONDS = 20.0
TYPE_LABELS = {
    "wayfinder:grilling",
    "wayfinder:prototype",
    "wayfinder:research",
    "wayfinder:task",
}
ROUTE_LABELS = {"ready-for-agent", "ready-for-human"}
BLOCKED_BY_LINE = re.compile(
    r"^[ \t]*Blocked[ \t]+by[ \t]*:(?P<references>.*)$",
    re.IGNORECASE,
)
ISSUE_REFERENCE = re.compile(r"#([1-9][0-9]*)")


@dataclass(frozen=True)
class Ticket:
    number: int
    title: str
    state: str
    labels: frozenset[str]
    assignees: tuple[str, ...]
    blocked_by: int
    parent: int
    fallback_blockers: tuple[int, ...] = ()
    open_fallback_blockers: tuple[int, ...] = ()

    @property
    def is_map(self) -> bool:
        return "wayfinder:map" in self.labels

    @property
    def is_blocked(self) -> bool:
        return self.blocked_by > 0 or bool(self.open_fallback_blockers)


def _gh_payload(
    repo: str,
    endpoint: str,
    timeout_seconds: float = DEFAULT_GH_TIMEOUT_SECONDS,
    page: int | None = None,
) -> Any:
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise RuntimeError("gh timeout must be a finite number greater than zero")
    command = [
        "gh",
        "api",
        "--method",
        "GET",
        f"repos/{repo}/{endpoint}",
    ]
    if page is not None:
        command.extend(["-f", "per_page=100", "-f", f"page={page}"])
    try:
        result = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="strict",
            timeout=timeout_seconds,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("gh is required and was not found on PATH") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or exc.stdout.strip() or "unknown gh error"
        raise RuntimeError(detail) from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"gh timed out after {timeout_seconds:g} seconds while reading {endpoint}"
        ) from exc
    except OSError as exc:
        raise RuntimeError(f"could not execute gh while reading {endpoint}: {exc}") from exc
    return json.loads(result.stdout)


def _gh_json(
    repo: str,
    endpoint: str,
    page: int,
    timeout_seconds: float = DEFAULT_GH_TIMEOUT_SECONDS,
) -> list[dict[str, Any]]:
    payload = _gh_payload(repo, endpoint, timeout_seconds, page)
    if not isinstance(payload, list):
        raise RuntimeError(f"GitHub returned a non-list payload for {endpoint}")
    return payload


def _children(
    repo: str,
    map_number: int,
    timeout_seconds: float = DEFAULT_GH_TIMEOUT_SECONDS,
) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    page = 1
    while True:
        batch = _gh_json(
            repo,
            f"issues/{map_number}/sub_issues",
            page,
            timeout_seconds,
        )
        items.extend(batch)
        if len(batch) < 100:
            return items
        page += 1


def _fallback_blockers(body: object) -> tuple[int, ...]:
    """Parse the documented top-of-body ``Blocked by: #n, #n`` fallback."""
    if not isinstance(body, str):
        return ()
    first_content_line = next((line for line in body.splitlines() if line.strip()), "")
    match = BLOCKED_BY_LINE.fullmatch(first_content_line)
    if match is None:
        return ()
    references = tuple(
        dict.fromkeys(
            int(number) for number in ISSUE_REFERENCE.findall(match.group("references"))
        )
    )
    if not references:
        raise RuntimeError(
            "malformed top-of-body 'Blocked by:' fallback; expected at least one #issue"
        )
    return references


def _ticket(item: dict[str, Any], parent: int) -> Ticket:
    dependencies = item.get("issue_dependencies_summary") or {}
    if not isinstance(dependencies, dict):
        raise RuntimeError(
            f"issue #{item.get('number', '?')} has a malformed dependency summary"
        )
    return Ticket(
        number=int(item["number"]),
        title=str(item["title"]),
        state=str(item["state"]).lower(),
        labels=frozenset(str(label["name"]) for label in item.get("labels", [])),
        assignees=tuple(str(user["login"]) for user in item.get("assignees", [])),
        blocked_by=int(dependencies.get("blocked_by", 0)),
        parent=parent,
        fallback_blockers=_fallback_blockers(item.get("body")),
    )


def _require_same_repo(item: dict[str, Any], repo: str, context: str) -> None:
    repository_url = str(item.get("repository_url", ""))
    actual_path = urlsplit(repository_url).path.rstrip("/").casefold()
    expected_path = f"/repos/{repo}".casefold()
    if actual_path != expected_path:
        rendered = repository_url or "missing repository_url"
        raise RuntimeError(
            f"{context} is not in {repo}; GitHub reported {rendered}"
        )


def _validate_root_map(repo: str, root_map: int, timeout_seconds: float) -> None:
    payload = _gh_payload(repo, f"issues/{root_map}", timeout_seconds)
    if not isinstance(payload, dict):
        raise RuntimeError(f"GitHub returned a non-object payload for root #{root_map}")
    _require_same_repo(payload, repo, f"root #{root_map}")
    ticket = _ticket(payload, root_map)
    wayfinder_labels = {label for label in ticket.labels if label.startswith("wayfinder:")}
    if ticket.state != "open":
        raise RuntimeError(f"root #{root_map} is {ticket.state}; expected an open Wayfinder map")
    if wayfinder_labels != {"wayfinder:map"}:
        rendered = ", ".join(sorted(wayfinder_labels)) or "none"
        raise RuntimeError(
            f"root #{root_map} must carry exactly wayfinder:map; found {rendered}"
        )
    if ticket.labels & ROUTE_LABELS:
        raise RuntimeError(f"root #{root_map} must not carry a ready route")
    if ticket.assignees:
        raise RuntimeError(f"root #{root_map} must not carry an active claim")


def _fallback_blocker_state(
    repo: str,
    blocker_number: int,
    timeout_seconds: float,
) -> str:
    endpoint = f"issues/{blocker_number}"
    try:
        payload = _gh_payload(repo, endpoint, timeout_seconds)
    except (RuntimeError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"could not resolve fallback blocker #{blocker_number}: {exc}"
        ) from exc
    if not isinstance(payload, dict):
        raise RuntimeError(
            f"GitHub returned a non-object payload for fallback blocker #{blocker_number}"
        )
    _require_same_repo(payload, repo, f"fallback blocker #{blocker_number}")
    try:
        actual_number = int(payload["number"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RuntimeError(
            f"GitHub omitted a valid issue number for fallback blocker #{blocker_number}"
        ) from exc
    if actual_number != blocker_number:
        raise RuntimeError(
            f"GitHub returned issue #{actual_number} for fallback blocker #{blocker_number}"
        )
    state = str(payload.get("state", "")).lower()
    if state not in {"open", "closed"}:
        rendered = state or "missing"
        raise RuntimeError(
            f"fallback blocker #{blocker_number} has unsupported state {rendered}"
        )
    return state


def _resolve_fallback_blockers(
    repo: str,
    tickets: Iterable[Ticket],
    timeout_seconds: float,
) -> list[Ticket]:
    rows = list(tickets)
    state_cache: dict[int, str] = {}
    resolved: list[Ticket] = []
    for ticket in rows:
        if ticket.state != "open" or ticket.is_map or not ticket.fallback_blockers:
            resolved.append(ticket)
            continue
        open_blockers: list[int] = []
        for blocker_number in ticket.fallback_blockers:
            if blocker_number not in state_cache:
                state_cache[blocker_number] = _fallback_blocker_state(
                    repo,
                    blocker_number,
                    timeout_seconds,
                )
            if state_cache[blocker_number] == "open":
                open_blockers.append(blocker_number)
        resolved.append(
            replace(ticket, open_fallback_blockers=tuple(open_blockers))
        )
    return resolved


def load_tree(
    repo: str,
    root_map: int,
    timeout_seconds: float = DEFAULT_GH_TIMEOUT_SECONDS,
) -> list[Ticket]:
    _validate_root_map(repo, root_map, timeout_seconds)
    tickets: list[Ticket] = []
    visited_maps: set[int] = set()

    def visit(map_number: int) -> None:
        if map_number in visited_maps:
            return
        visited_maps.add(map_number)
        for item in _children(repo, map_number, timeout_seconds):
            _require_same_repo(item, repo, f"child of map #{map_number}")
            ticket = _ticket(item, map_number)
            tickets.append(ticket)
            if ticket.is_map:
                visit(ticket.number)

    visit(root_map)
    return _resolve_fallback_blockers(repo, tickets, timeout_seconds)


def _ticket_hygiene_errors(ticket: Ticket, states: dict[int, str]) -> list[str]:
    errors: list[str] = []
    routes = ticket.labels & ROUTE_LABELS
    types = ticket.labels & TYPE_LABELS
    wayfinder_labels = {
        label for label in ticket.labels if label.startswith("wayfinder:")
    }
    if ticket.state == "closed" and routes:
        errors.append(f"#{ticket.number}: closed ticket still has {', '.join(sorted(routes))}")
    if ticket.state == "open" and states.get(ticket.parent) == "closed":
        errors.append(f"#{ticket.number}: open child belongs to closed map #{ticket.parent}")
    if ticket.is_map:
        if wayfinder_labels != {"wayfinder:map"}:
            rendered = ", ".join(sorted(wayfinder_labels)) or "none"
            errors.append(
                f"#{ticket.number}: map must carry exactly wayfinder:map; found {rendered}"
            )
        if routes:
            errors.append(f"#{ticket.number}: map must not carry a ready route")
        if ticket.assignees:
            errors.append(f"#{ticket.number}: map must not carry an active claim")
        return errors
    if len(types) != 1:
        errors.append(
            f"#{ticket.number}: expected one Wayfinder type, found "
            f"{', '.join(sorted(types)) or 'none'}"
        )
    if len(routes) > 1:
        errors.append(f"#{ticket.number}: has both ready routes")
    if ticket.state == "open" and ticket.assignees and ticket.is_blocked:
        errors.append(f"#{ticket.number}: blocked ticket carries an active claim")
    if ticket.state == "open" and not ticket.assignees and len(routes) != 1:
        errors.append(f"#{ticket.number}: open unclaimed ticket needs exactly one ready route")
    if ticket.state == "open" and not ticket.assignees:
        if "wayfinder:grilling" in types and "ready-for-human" not in routes:
            errors.append(f"#{ticket.number}: grilling must route to ready-for-human")
        if "wayfinder:prototype" in types and "ready-for-human" not in routes:
            errors.append(f"#{ticket.number}: prototype must route to ready-for-human")
        if "wayfinder:research" in types and "ready-for-agent" not in routes:
            errors.append(f"#{ticket.number}: research must route to ready-for-agent")
    if "needs-triage" in ticket.labels and routes:
        errors.append(f"#{ticket.number}: needs-triage conflicts with an explicit ready route")
    return errors


def hygiene_errors(tickets: Iterable[Ticket]) -> list[str]:
    rows = list(tickets)
    states = {ticket.number: ticket.state for ticket in rows}
    return [
        error
        for ticket in rows
        for error in _ticket_hygiene_errors(ticket, states)
    ]


def _print_group(name: str, tickets: Iterable[Ticket]) -> int:
    rows = list(tickets)
    print(f"\n{name} ({len(rows)})")
    for ticket in rows:
        suffix = f" [map #{ticket.parent}]"
        if ticket.blocked_by:
            suffix += f" [{ticket.blocked_by} open blocker(s)]"
        if ticket.open_fallback_blockers:
            rendered = ", ".join(f"#{number}" for number in ticket.open_fallback_blockers)
            suffix += f" [open fallback blocker(s): {rendered}]"
        if ticket.assignees:
            suffix += f" [claimed: {', '.join(ticket.assignees)}]"
        print(f"  #{ticket.number} {ticket.title}{suffix}")
    return len(rows)


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="strict")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub OWNER/REPO")
    parser.add_argument("--map", type=int, default=DEFAULT_MAP, dest="map_number")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero when ticket routing hygiene is inconsistent",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=DEFAULT_GH_TIMEOUT_SECONDS,
        help=f"timeout for each gh API request (default: {DEFAULT_GH_TIMEOUT_SECONDS:g})",
    )
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be a finite number greater than zero")

    try:
        tickets = load_tree(args.repo, args.map_number, args.timeout_seconds)
    except (RuntimeError, json.JSONDecodeError) as exc:
        print(f"frontier: ERROR: {exc}", file=sys.stderr)
        return 1

    open_work = [ticket for ticket in tickets if ticket.state == "open" and not ticket.is_map]
    states = {ticket.number: ticket.state for ticket in tickets}
    invalid_numbers = {
        ticket.number
        for ticket in open_work
        if _ticket_hygiene_errors(ticket, states)
    }
    valid_work = [ticket for ticket in open_work if ticket.number not in invalid_numbers]
    agent = [
        ticket
        for ticket in valid_work
        if not ticket.assignees
        and not ticket.is_blocked
        and ticket.labels & ROUTE_LABELS == {"ready-for-agent"}
    ]
    human = [
        ticket
        for ticket in valid_work
        if not ticket.assignees
        and not ticket.is_blocked
        and ticket.labels & ROUTE_LABELS == {"ready-for-human"}
    ]
    claimed = [ticket for ticket in valid_work if ticket.assignees]
    blocked = [ticket for ticket in valid_work if not ticket.assignees and ticket.is_blocked]
    invalid = [ticket for ticket in open_work if ticket.number in invalid_numbers]

    print(f"Wayfinder frontier: {args.repo} map #{args.map_number}")
    _print_group("AGENT FRONTIER", agent)
    _print_group("HUMAN FRONTIER", human)
    _print_group("CLAIMED", claimed)
    _print_group("BLOCKED", blocked)
    _print_group("INVALID ROUTING", invalid)

    errors = hygiene_errors(tickets)
    print(f"\nHYGIENE ({len(errors)} error(s))")
    for error in errors:
        print(f"  {error}")

    if args.strict and errors:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
