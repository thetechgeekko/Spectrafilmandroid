from __future__ import annotations

import importlib.util
import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "frontier.py"
SPEC = importlib.util.spec_from_file_location("wayfinder_frontier", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
frontier = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = frontier
SPEC.loader.exec_module(frontier)


def ticket(
    number: int,
    *,
    labels: set[str],
    state: str = "open",
    assignees: tuple[str, ...] = (),
    blocked_by: int = 0,
    title: str = "ticket",
    parent: int = 164,
):
    return frontier.Ticket(
        number=number,
        title=title,
        state=state,
        labels=frozenset(labels),
        assignees=assignees,
        blocked_by=blocked_by,
        parent=parent,
    )


class FrontierTest(unittest.TestCase):
    def test_ticket_preserves_utf8_title(self) -> None:
        parsed = frontier._ticket(
            {
                "number": 1,
                "title": "Define “bit-identical” — 1–2 s",
                "state": "open",
                "labels": [{"name": "wayfinder:grilling"}, {"name": "ready-for-human"}],
                "assignees": [],
                "issue_dependencies_summary": {"blocked_by": 0},
            },
            117,
        )
        self.assertEqual(parsed.title, "Define “bit-identical” — 1–2 s")

    @mock.patch.object(frontier.subprocess, "run")
    def test_gh_json_requests_utf8(self, run: mock.Mock) -> None:
        run.return_value = mock.Mock(stdout=json.dumps([{"title": "1–2 s"}], ensure_ascii=False))
        payload = frontier._gh_json("o/r", "issues/1/sub_issues", 1)
        self.assertEqual(payload[0]["title"], "1–2 s")
        self.assertEqual(run.call_args.kwargs["encoding"], "utf-8")
        self.assertEqual(run.call_args.kwargs["errors"], "strict")
        self.assertEqual(
            run.call_args.kwargs["timeout"],
            frontier.DEFAULT_GH_TIMEOUT_SECONDS,
        )

    @mock.patch.object(frontier.subprocess, "run")
    def test_gh_json_reports_bounded_timeout(self, run: mock.Mock) -> None:
        run.side_effect = frontier.subprocess.TimeoutExpired("gh", 3)
        with self.assertRaisesRegex(RuntimeError, "timed out after 3 seconds"):
            frontier._gh_json("o/r", "issues/1/sub_issues", 1, 3)

    @mock.patch.object(frontier.subprocess, "run")
    def test_gh_json_rejects_non_finite_timeout_before_spawn(self, run: mock.Mock) -> None:
        for timeout in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(timeout=timeout):
                with self.assertRaisesRegex(RuntimeError, "finite number greater than zero"):
                    frontier._gh_json("o/r", "issues/1/sub_issues", 1, timeout)
        run.assert_not_called()

    def test_recursive_map_traversal_preserves_map_order(self) -> None:
        root = [
            {
                "number": 10,
                "title": "first",
                "repository_url": "https://api.github.com/repos/o/r",
                "state": "open",
                "labels": [{"name": "wayfinder:task"}, {"name": "ready-for-agent"}],
                "assignees": [],
                "issue_dependencies_summary": {"blocked_by": 0},
            },
            {
                "number": 117,
                "title": "nested",
                "repository_url": "https://api.github.com/repos/o/r",
                "state": "open",
                "labels": [{"name": "wayfinder:map"}],
                "assignees": [],
                "issue_dependencies_summary": {"blocked_by": 0},
            },
        ]
        nested = [
            {
                "number": 11,
                "title": "nested child",
                "repository_url": "https://api.github.com/repos/o/r",
                "state": "open",
                "labels": [{"name": "wayfinder:task"}, {"name": "ready-for-agent"}],
                "assignees": [],
                "issue_dependencies_summary": {"blocked_by": 0},
            }
        ]
        with mock.patch.object(
            frontier,
            "_children",
            side_effect=lambda _repo, n, _timeout: root if n == 164 else nested,
        ), mock.patch.object(frontier, "_validate_root_map"):
            result = frontier.load_tree("o/r", 164)
        self.assertEqual([item.number for item in result], [10, 117, 11])

    def test_closed_nested_map_is_traversed_and_open_child_is_reported(self) -> None:
        closed_map = {
            "number": 117,
            "title": "closed map",
            "repository_url": "https://api.github.com/repos/o/r",
            "state": "closed",
            "labels": [{"name": "wayfinder:map"}],
            "assignees": [],
            "issue_dependencies_summary": {"blocked_by": 0},
        }
        open_child = {
            "number": 11,
            "title": "orphaned work",
            "repository_url": "https://api.github.com/repos/o/r",
            "state": "open",
            "labels": [{"name": "wayfinder:task"}, {"name": "ready-for-agent"}],
            "assignees": [],
            "issue_dependencies_summary": {"blocked_by": 0},
        }
        with mock.patch.object(
            frontier,
            "_children",
            side_effect=lambda _repo, n, _timeout: [closed_map] if n == 164 else [open_child],
        ), mock.patch.object(frontier, "_validate_root_map"):
            rows = frontier.load_tree("o/r", 164)
        self.assertIn(
            "open child belongs to closed map #117",
            "\n".join(frontier.hygiene_errors(rows)),
        )

    def test_root_must_be_an_open_map(self) -> None:
        payload = {
            "number": 172,
            "title": "open task",
            "repository_url": "https://api.github.com/repos/o/r",
            "state": "open",
            "labels": [{"name": "wayfinder:task"}],
            "assignees": [],
            "issue_dependencies_summary": {"blocked_by": 0},
        }
        with mock.patch.object(frontier, "_gh_payload", return_value=payload):
            with self.assertRaisesRegex(RuntimeError, "must carry exactly wayfinder:map"):
                frontier._validate_root_map("o/r", 172, 3)

    def test_closed_root_map_is_rejected(self) -> None:
        payload = {
            "number": 164,
            "title": "closed map",
            "repository_url": "https://api.github.com/repos/o/r",
            "state": "closed",
            "labels": [{"name": "wayfinder:map"}],
            "assignees": [],
            "issue_dependencies_summary": {"blocked_by": 0},
        }
        with mock.patch.object(frontier, "_gh_payload", return_value=payload):
            with self.assertRaisesRegex(RuntimeError, "expected an open Wayfinder map"):
                frontier._validate_root_map("o/r", 164, 3)

    def test_cross_repository_child_fails_closed(self) -> None:
        foreign = {
            "number": 10,
            "title": "foreign task",
            "repository_url": "https://api.github.com/repos/other/project",
            "state": "open",
            "labels": [{"name": "wayfinder:task"}, {"name": "ready-for-agent"}],
            "assignees": [],
            "issue_dependencies_summary": {"blocked_by": 0},
        }
        with mock.patch.object(frontier, "_validate_root_map"), mock.patch.object(
            frontier, "_children", return_value=[foreign]
        ):
            with self.assertRaisesRegex(RuntimeError, "is not in o/r"):
                frontier.load_tree("o/r", 164)

    def test_nested_map_rejects_type_route_and_claim(self) -> None:
        malformed = ticket(
            117,
            labels={"wayfinder:map", "wayfinder:task", "ready-for-agent"},
            assignees=("dev",),
        )
        errors = "\n".join(frontier.hygiene_errors([malformed]))
        self.assertIn("map must carry exactly wayfinder:map", errors)
        self.assertIn("map must not carry a ready route", errors)
        self.assertIn("map must not carry an active claim", errors)

    def test_hygiene_reports_route_type_claim_and_closed_errors(self) -> None:
        cases = [
            ticket(1, labels={"wayfinder:task", "wayfinder:research", "ready-for-agent"}),
            ticket(2, labels={"wayfinder:grilling", "ready-for-agent"}),
            ticket(3, labels={"wayfinder:research", "ready-for-human"}),
            ticket(9, labels={"wayfinder:prototype", "ready-for-agent"}),
            ticket(4, labels={"wayfinder:task", "ready-for-agent", "needs-triage"}),
            ticket(5, labels={"wayfinder:task", "ready-for-agent"}, assignees=("dev",), blocked_by=1),
            ticket(6, labels={"wayfinder:task", "ready-for-agent"}, state="closed"),
        ]
        errors = "\n".join(frontier.hygiene_errors(cases))
        self.assertIn("expected one Wayfinder type", errors)
        self.assertIn("grilling must route to ready-for-human", errors)
        self.assertIn("research must route to ready-for-agent", errors)
        self.assertIn("prototype must route to ready-for-human", errors)
        self.assertIn("needs-triage conflicts", errors)
        self.assertIn("blocked ticket carries an active claim", errors)
        self.assertIn("closed ticket still has", errors)

    def test_main_groups_claimed_and_blocked_exclusively(self) -> None:
        claimed_blocked = ticket(
            7,
            labels={"wayfinder:task", "ready-for-agent"},
            assignees=("dev",),
            blocked_by=2,
        )
        with mock.patch.object(frontier, "load_tree", return_value=[claimed_blocked]):
            output = io.StringIO()
            with redirect_stdout(output):
                code = frontier.main([])
        self.assertEqual(code, 0)
        self.assertEqual(output.getvalue().count("#7 ticket"), 1)

    def test_strict_returns_two_for_hygiene_errors(self) -> None:
        wrong = ticket(8, labels={"wayfinder:grilling", "ready-for-agent"})
        with mock.patch.object(frontier, "load_tree", return_value=[wrong]), redirect_stdout(io.StringIO()):
            self.assertEqual(frontier.main(["--strict"]), 2)

    def test_dual_route_ticket_appears_only_in_invalid_group(self) -> None:
        wrong = ticket(
            12,
            labels={"wayfinder:task", "ready-for-agent", "ready-for-human"},
        )
        with mock.patch.object(frontier, "load_tree", return_value=[wrong]):
            output = io.StringIO()
            with redirect_stdout(output):
                code = frontier.main([])
        self.assertEqual(code, 0)
        rendered = output.getvalue()
        self.assertEqual(rendered.count("#12 ticket"), 1)
        self.assertIn("INVALID ROUTING (1)", rendered)

    def test_singly_routed_hygiene_errors_never_appear_takeable(self) -> None:
        wrong = [
            ticket(13, labels={"wayfinder:grilling", "ready-for-agent"}),
            ticket(
                14,
                labels={"wayfinder:task", "wayfinder:research", "ready-for-agent"},
            ),
            ticket(
                15,
                labels={"wayfinder:task", "ready-for-agent", "needs-triage"},
            ),
        ]
        with mock.patch.object(frontier, "load_tree", return_value=wrong):
            output = io.StringIO()
            with redirect_stdout(output):
                code = frontier.main([])
        self.assertEqual(code, 0)
        rendered = output.getvalue()
        self.assertIn("AGENT FRONTIER (0)", rendered)
        self.assertIn("HUMAN FRONTIER (0)", rendered)
        self.assertIn("INVALID ROUTING (3)", rendered)


if __name__ == "__main__":
    unittest.main()
