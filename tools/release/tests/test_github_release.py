"""End-to-end unit tests for the exact-ID GitHub release publisher."""

from __future__ import annotations

import contextlib
import hashlib
import http.server
import json
import pathlib
import socket
import tempfile
import threading
import unittest
import urllib.parse

from tools.release import github_release

REPOSITORY = "acme/spektrafilm"
TAG = "v1.2.3"
SOURCE_SHA = "0123456789abcdef0123456789abcdef01234567"
TAG_OBJECT_SHA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"


def forged_legacy_marker() -> str:
    material = f"{REPOSITORY}\x00{TAG}\x00{SOURCE_SHA}\x009001\x002".encode()
    return f"<!-- spektrafilm-release-run:v1:{hashlib.sha256(material).hexdigest()} -->"


class FakeGitHubState:
    def __init__(self) -> None:
        self.immutable_enabled = True
        self.releases: dict[int, dict[str, object]] = {}
        self.assets: dict[int, dict[str, object]] = {}
        self.next_release_id = 101
        self.next_asset_id = 501
        self.requests: list[tuple[str, str]] = []
        self.api_versions: list[str | None] = []
        self.authorization_headers: list[str | None] = []
        self.deleted_release_ids: list[int] = []
        self.drop_create_response_once = False
        self.drop_publish_response_once = False
        self.fail_upload_once = False
        self.tag_sha_sequence = [SOURCE_SHA]
        self.create_foreign_duplicate_once = False
        self.annotated_tag = False

    def add_release(
        self,
        release_id: int,
        *,
        tag: str = TAG,
        body: str = "existing release",
        draft: bool = False,
        prerelease: bool = False,
        immutable: bool = True,
    ) -> None:
        self.releases[release_id] = {
            "id": release_id,
            "tag_name": tag,
            "target_commitish": SOURCE_SHA,
            "name": tag,
            "body": body,
            "draft": draft,
            "prerelease": prerelease,
            "immutable": immutable,
        }
        self.next_release_id = max(self.next_release_id, release_id + 1)

    def add_asset(self, release_id: int, asset_id: int, name: str, data: bytes) -> None:
        self.assets[asset_id] = {
            "release_id": release_id,
            "name": name,
            "data": data,
        }
        self.next_asset_id = max(self.next_asset_id, asset_id + 1)

    def release_json(self, release_id: int) -> dict[str, object]:
        release = dict(self.releases[release_id])
        release["assets"] = [
            self.asset_json(asset_id)
            for asset_id, asset in sorted(self.assets.items())
            if asset["release_id"] == release_id
        ]
        return release

    def asset_json(self, asset_id: int) -> dict[str, object]:
        asset = self.assets[asset_id]
        return {
            "id": asset_id,
            "name": asset["name"],
            "state": "uploaded",
            "size": len(asset["data"]),
        }


class FakeGitHubHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def state(self) -> FakeGitHubState:
        return self.server.state  # type: ignore[attr-defined]

    def log_message(self, _format: str, *_args: object) -> None:
        pass

    def _path(self) -> tuple[str, dict[str, list[str]]]:
        parsed = urllib.parse.urlsplit(self.path)
        return parsed.path, urllib.parse.parse_qs(parsed.query)

    def _json_body(self) -> dict[str, object]:
        length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(length))

    def _send_json(self, status: int, value: object) -> None:
        body = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, status: int, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _drop_response(self) -> None:
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.connection.close()

    def do_GET(self) -> None:
        path, query = self._path()
        self.state.requests.append(("GET", self.path))
        self.state.api_versions.append(self.headers.get("X-GitHub-Api-Version"))
        self.state.authorization_headers.append(self.headers.get("Authorization"))
        prefix = f"/repos/{REPOSITORY}"
        if path == f"{prefix}/immutable-releases":
            self._send_json(200, {"enabled": self.state.immutable_enabled})
            return
        if path == f"{prefix}/git/ref/tags/{TAG}":
            if self.state.annotated_tag:
                target = {"type": "tag", "sha": TAG_OBJECT_SHA}
            else:
                tag_sha = self.state.tag_sha_sequence[0]
                if len(self.state.tag_sha_sequence) > 1:
                    self.state.tag_sha_sequence.pop(0)
                target = {"type": "commit", "sha": tag_sha}
            self._send_json(
                200,
                {
                    "ref": f"refs/tags/{TAG}",
                    "object": target,
                },
            )
            return
        if path == f"{prefix}/git/tags/{TAG_OBJECT_SHA}" and self.state.annotated_tag:
            tag_sha = self.state.tag_sha_sequence[0]
            if len(self.state.tag_sha_sequence) > 1:
                self.state.tag_sha_sequence.pop(0)
            self._send_json(
                200,
                {
                    "tag": TAG,
                    "sha": TAG_OBJECT_SHA,
                    "object": {"type": "commit", "sha": tag_sha},
                },
            )
            return
        if path == f"{prefix}/releases":
            page = int(query.get("page", ["1"])[0])
            values = [
                self.state.release_json(key) for key in sorted(self.state.releases)
            ]
            start = (page - 1) * 100
            self._send_json(200, values[start : start + 100])
            return
        exact_release = f"{prefix}/releases/"
        if path.startswith(exact_release) and "/assets/" not in path:
            suffix = path[len(exact_release) :]
            if suffix.endswith("/assets"):
                release_id = int(suffix[: -len("/assets")])
                page = int(query.get("page", ["1"])[0])
                values = [
                    self.state.asset_json(asset_id)
                    for asset_id, asset in sorted(self.state.assets.items())
                    if asset["release_id"] == release_id
                ]
                start = (page - 1) * 100
                self._send_json(200, values[start : start + 100])
                return
            release_id = int(suffix)
            if release_id in self.state.releases:
                self._send_json(200, self.state.release_json(release_id))
            else:
                self._send_json(404, {"message": "Not Found"})
            return
        asset_prefix = f"{prefix}/releases/assets/"
        if path.startswith(asset_prefix):
            asset_id = int(path[len(asset_prefix) :])
            if asset_id in self.state.assets:
                self._send_bytes(200, self.state.assets[asset_id]["data"])
            else:
                self._send_json(404, {"message": "Not Found"})
            return
        self._send_json(404, {"message": "Not Found"})

    def do_POST(self) -> None:
        path, query = self._path()
        self.state.requests.append(("POST", self.path))
        self.state.api_versions.append(self.headers.get("X-GitHub-Api-Version"))
        self.state.authorization_headers.append(self.headers.get("Authorization"))
        prefix = f"/repos/{REPOSITORY}"
        if path == f"{prefix}/releases":
            body = self._json_body()
            release_id = self.state.next_release_id
            self.state.next_release_id += 1
            release_body = str(body["body"])
            if body.get("generate_release_notes") is True:
                release_body += "\n\n## What's Changed\n\n* generated notes"
            self.state.releases[release_id] = {
                "id": release_id,
                "tag_name": body["tag_name"],
                "target_commitish": body["target_commitish"],
                "name": body["name"],
                "body": release_body,
                "draft": body["draft"],
                "prerelease": body["prerelease"],
                "immutable": False,
            }
            if self.state.create_foreign_duplicate_once:
                self.state.create_foreign_duplicate_once = False
                duplicate_id = self.state.next_release_id
                self.state.add_release(
                    duplicate_id,
                    body="<!-- foreign-run -->",
                    draft=True,
                    immutable=False,
                )
            if self.state.drop_create_response_once:
                self.state.drop_create_response_once = False
                self._drop_response()
                return
            self._send_json(201, self.state.release_json(release_id))
            return
        upload_prefix = f"{prefix}/releases/"
        if path.startswith(upload_prefix) and path.endswith("/assets"):
            release_id = int(path[len(upload_prefix) : -len("/assets")])
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length)
            if self.state.fail_upload_once:
                self.state.fail_upload_once = False
                self._send_json(500, {"message": "injected upload failure"})
                return
            asset_id = self.state.next_asset_id
            self.state.next_asset_id += 1
            self.state.assets[asset_id] = {
                "release_id": release_id,
                "name": query["name"][0],
                "data": data,
            }
            self._send_json(201, self.state.asset_json(asset_id))
            return
        self._send_json(404, {"message": "Not Found"})

    def do_PATCH(self) -> None:
        path, _query = self._path()
        self.state.requests.append(("PATCH", self.path))
        self.state.api_versions.append(self.headers.get("X-GitHub-Api-Version"))
        self.state.authorization_headers.append(self.headers.get("Authorization"))
        prefix = f"/repos/{REPOSITORY}/releases/"
        if path.startswith(prefix):
            release_id = int(path[len(prefix) :])
            body = self._json_body()
            release = self.state.releases[release_id]
            release["draft"] = body["draft"]
            if body["draft"] is False:
                release["immutable"] = True
                if self.state.drop_publish_response_once:
                    self.state.drop_publish_response_once = False
                    self._drop_response()
                    return
            self._send_json(200, self.state.release_json(release_id))
            return
        self._send_json(404, {"message": "Not Found"})

    def do_DELETE(self) -> None:
        path, _query = self._path()
        self.state.requests.append(("DELETE", self.path))
        self.state.api_versions.append(self.headers.get("X-GitHub-Api-Version"))
        self.state.authorization_headers.append(self.headers.get("Authorization"))
        prefix = f"/repos/{REPOSITORY}/releases/"
        if path.startswith(prefix):
            release_id = int(path[len(prefix) :])
            self.state.deleted_release_ids.append(release_id)
            self.state.releases.pop(release_id, None)
            for asset_id in list(self.state.assets):
                if self.state.assets[asset_id]["release_id"] == release_id:
                    del self.state.assets[asset_id]
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self._send_json(404, {"message": "Not Found"})


@contextlib.contextmanager
def fake_github(state: FakeGitHubState):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeGitHubHandler)
    server.state = state  # type: ignore[attr-defined]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        host, port = server.server_address
        yield f"http://{host}:{port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class GitHubReleasePublisherTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.assets_dir = pathlib.Path(self.temp.name) / "out"
        self.assets_dir.mkdir()
        self.expected_assets = {
            "SHA256SUMS": hashlib.sha256(b"apk-v1").hexdigest().encode("ascii"),
            "Spektrafilm-v1.2.3.apk": b"apk-v1",
        }
        for name, data in self.expected_assets.items():
            (self.assets_dir / name).write_bytes(data)

    def env(self, base_url: str) -> dict[str, str]:
        return {
            "GITHUB_TOKEN": "test-token",
            "GITHUB_REPOSITORY": REPOSITORY,
            "GITHUB_RUN_ID": "9001",
            "GITHUB_RUN_ATTEMPT": "2",
            "GITHUB_API_URL": base_url,
            "GITHUB_UPLOAD_URL": base_url,
        }

    def publish(self, state: FakeGitHubState) -> github_release.PublishResult:
        with fake_github(state) as base_url:
            return github_release.publish_from_args(
                [
                    "--tag",
                    TAG,
                    "--source-sha",
                    SOURCE_SHA,
                    "--assets-dir",
                    str(self.assets_dir),
                ],
                self.env(base_url),
            )

    def test_happy_path_creates_verifies_and_publishes_exact_draft(self) -> None:
        state = FakeGitHubState()

        result = self.publish(state)

        self.assertEqual(github_release.PublishResult(101, "published"), result)
        self.assertFalse(state.releases[101]["draft"])
        self.assertTrue(state.releases[101]["immutable"])
        self.assertIn("<!-- spektrafilm-release-run:v2:", state.releases[101]["body"])
        remote_assets = {
            str(asset["name"]): bytes(asset["data"]) for asset in state.assets.values()
        }
        self.assertEqual(self.expected_assets, remote_assets)
        self.assertEqual([], state.deleted_release_ids)
        self.assertEqual({github_release.API_VERSION}, set(state.api_versions))
        self.assertEqual({"Bearer test-token"}, set(state.authorization_headers))
        tag_reads = [
            path
            for method, path in state.requests
            if method == "GET" and path == f"/repos/{REPOSITORY}/git/ref/tags/{TAG}"
        ]
        self.assertGreaterEqual(len(tag_reads), 3)

    def test_preexisting_exact_immutable_release_is_verified_idempotently(self) -> None:
        state = FakeGitHubState()
        state.add_release(73)
        for offset, (name, data) in enumerate(self.expected_assets.items()):
            state.add_asset(73, 700 + offset, name, data)

        result = self.publish(state)

        self.assertEqual(github_release.PublishResult(73, "existing"), result)
        mutation_methods = {
            method
            for method, _path in state.requests
            if method in {"POST", "PATCH", "DELETE"}
        }
        self.assertEqual(set(), mutation_methods)
        self.assertFalse(state.releases[73]["draft"])
        self.assertTrue(state.releases[73]["immutable"])

    def test_create_response_loss_recovers_the_unique_run_owned_draft(self) -> None:
        state = FakeGitHubState()
        state.drop_create_response_once = True

        result = self.publish(state)

        self.assertEqual(github_release.PublishResult(101, "published"), result)
        create_requests = [
            path
            for method, path in state.requests
            if method == "POST" and path == f"/repos/{REPOSITORY}/releases"
        ]
        self.assertEqual(1, len(create_requests))
        self.assertFalse(state.releases[101]["draft"])
        self.assertEqual([], state.deleted_release_ids)

    def test_publish_response_loss_recovers_exact_immutable_published_state(
        self,
    ) -> None:
        state = FakeGitHubState()
        state.drop_publish_response_once = True

        result = self.publish(state)

        self.assertEqual(github_release.PublishResult(101, "published"), result)
        patch_requests = [
            method for method, _path in state.requests if method == "PATCH"
        ]
        self.assertEqual(["PATCH"], patch_requests)
        self.assertFalse(state.releases[101]["draft"])
        self.assertTrue(state.releases[101]["immutable"])
        self.assertEqual([], state.deleted_release_ids)

    def test_duplicate_exact_tag_releases_fail_before_any_mutation(self) -> None:
        state = FakeGitHubState()
        state.add_release(31)
        state.add_release(32)

        with self.assertRaisesRegex(github_release.PublishError, "2 releases"):
            self.publish(state)

        mutation_methods = {
            method
            for method, _path in state.requests
            if method in {"POST", "PATCH", "DELETE"}
        }
        self.assertEqual(set(), mutation_methods)
        self.assertEqual({31, 32}, set(state.releases))

    def test_duplicate_asset_names_fail_and_never_delete_published_release(
        self,
    ) -> None:
        state = FakeGitHubState()
        state.add_release(61)
        state.add_asset(61, 801, "Spektrafilm-v1.2.3.apk", b"apk-v1")
        state.add_asset(61, 802, "Spektrafilm-v1.2.3.apk", b"apk-v1")
        state.add_asset(61, 803, "SHA256SUMS", self.expected_assets["SHA256SUMS"])

        with self.assertRaisesRegex(
            github_release.PublishError, "duplicate release asset name"
        ):
            self.publish(state)

        self.assertIn(61, state.releases)
        self.assertFalse(state.releases[61]["draft"])
        self.assertEqual([], state.deleted_release_ids)
        self.assertNotIn("DELETE", [method for method, _path in state.requests])

    def test_same_size_remote_byte_drift_fails_without_deleting_published_release(
        self,
    ) -> None:
        state = FakeGitHubState()
        state.add_release(62)
        state.add_asset(62, 811, "Spektrafilm-v1.2.3.apk", b"apk-v2")
        state.add_asset(62, 812, "SHA256SUMS", self.expected_assets["SHA256SUMS"])

        with self.assertRaisesRegex(github_release.PublishError, "download bytes"):
            self.publish(state)

        self.assertIn(62, state.releases)
        self.assertEqual([], state.deleted_release_ids)

    def test_failure_deletes_only_the_exact_run_owned_draft(self) -> None:
        state = FakeGitHubState()
        state.fail_upload_once = True

        with self.assertRaises(github_release.HttpError):
            self.publish(state)

        self.assertEqual([101], state.deleted_release_ids)
        self.assertNotIn(101, state.releases)

    def test_predictable_legacy_marker_cannot_claim_or_delete_foreign_draft(
        self,
    ) -> None:
        state = FakeGitHubState()
        state.add_release(
            63,
            body=forged_legacy_marker(),
            draft=True,
            immutable=False,
        )

        with self.assertRaisesRegex(github_release.PublishError, "not owned"):
            self.publish(state)

        self.assertIn(63, state.releases)
        self.assertEqual([], state.deleted_release_ids)

    def test_failure_never_deletes_a_foreign_draft_with_the_same_tag(self) -> None:
        state = FakeGitHubState()
        state.add_release(
            64,
            body="<!-- marker-owned-by-a-different-run -->",
            draft=True,
            immutable=False,
        )

        with self.assertRaisesRegex(github_release.PublishError, "not owned"):
            self.publish(state)

        self.assertIn(64, state.releases)
        self.assertEqual([], state.deleted_release_ids)
        self.assertNotIn("DELETE", [method for method, _path in state.requests])

    def test_disabled_immutable_releases_fail_before_any_mutation(self) -> None:
        state = FakeGitHubState()
        state.immutable_enabled = False

        with self.assertRaisesRegex(github_release.PublishError, "not enabled"):
            self.publish(state)

        self.assertEqual(
            [("GET", f"/repos/{REPOSITORY}/immutable-releases")], state.requests
        )

    def test_tag_drift_immediately_before_publish_cleans_only_owned_draft(self) -> None:
        state = FakeGitHubState()
        state.tag_sha_sequence = [SOURCE_SHA, "f" * 40]

        with self.assertRaisesRegex(github_release.PublishError, "resolves to"):
            self.publish(state)

        self.assertEqual([101], state.deleted_release_ids)
        self.assertNotIn(101, state.releases)
        self.assertNotIn("PATCH", [method for method, _path in state.requests])

    def test_create_response_loss_with_duplicate_cleans_only_marker_owned_id(
        self,
    ) -> None:
        state = FakeGitHubState()
        state.create_foreign_duplicate_once = True
        state.drop_create_response_once = True

        with self.assertRaisesRegex(github_release.PublishError, "2 releases"):
            self.publish(state)

        self.assertEqual([101], state.deleted_release_ids)
        self.assertNotIn(101, state.releases)
        self.assertIn(102, state.releases)
        self.assertEqual("<!-- foreign-run -->", state.releases[102]["body"])

    def test_annotated_tag_is_peeled_to_the_exact_source_commit(self) -> None:
        state = FakeGitHubState()
        state.annotated_tag = True

        result = self.publish(state)

        self.assertEqual(github_release.PublishResult(101, "published"), result)
        tag_object_reads = [
            path
            for method, path in state.requests
            if method == "GET"
            and path == f"/repos/{REPOSITORY}/git/tags/{TAG_OBJECT_SHA}"
        ]
        self.assertGreaterEqual(len(tag_object_reads), 3)


if __name__ == "__main__":
    unittest.main()
