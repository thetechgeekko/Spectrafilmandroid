#!/usr/bin/env python3
"""Fail-closed publisher for an already-verified GitHub release asset set.

Every mutable operation is addressed by the numeric database ID returned by the
GitHub REST API.  A draft is the only state this program will ever delete, and
only when its exact tag and private run marker prove that this invocation owns
it.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import http.client
import json
import os
import pathlib
import re
import secrets
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Mapping, Sequence
from typing import IO, Any

API_VERSION = "2026-03-10"
DEFAULT_API_URL = "https://api.github.com"
DEFAULT_UPLOAD_URL = "https://uploads.github.com"
HTTP_TIMEOUT_SECONDS = 60
MAX_PAGES = 1_000
PAGE_SIZE = 100
READ_CHUNK_SIZE = 1024 * 1024

_TAG_PATTERN = re.compile(r"^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")
_SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
_REPOSITORY_PATTERN = re.compile(
    r"^[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])?/[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])?$"
)
_ASSET_NAME_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


class PublishError(RuntimeError):
    """The release contract could not be proven, so publication stopped."""


class HttpError(PublishError):
    """GitHub returned an HTTP error response."""

    def __init__(self, method: str, url: str, status: int, detail: str) -> None:
        super().__init__(f"{method} {url} returned HTTP {status}: {detail}")
        self.status = status


class TransportError(PublishError):
    """The client could not determine the server's response."""


@dataclasses.dataclass(frozen=True)
class PublishResult:
    release_id: int
    status: str


@dataclasses.dataclass(frozen=True)
class AssetSpec:
    name: str
    path: pathlib.Path
    size: int
    sha256: str


@dataclasses.dataclass(frozen=True)
class Config:
    token: str
    repository: str
    run_id: str
    run_attempt: str
    tag: str
    source_sha: str
    assets_dir: pathlib.Path
    api_url: str
    upload_url: str


class _SafeRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Prevent a REST bearer token from following a cross-origin redirect."""

    def redirect_request(
        self,
        req: urllib.request.Request,
        fp: IO[bytes],
        code: int,
        msg: str,
        headers: http.client.HTTPMessage,
        newurl: str,
    ) -> urllib.request.Request | None:
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected is None:
            return None
        if (
            urllib.parse.urlsplit(req.full_url).scheme.lower() == "https"
            and urllib.parse.urlsplit(newurl).scheme.lower() != "https"
        ):
            raise PublishError("refusing an HTTPS-to-HTTP API redirect")
        if _origin(req.full_url) != _origin(newurl):
            redirected.remove_header("Authorization")
        return redirected


def _origin(url: str) -> tuple[str, str, int | None]:
    try:
        parsed = urllib.parse.urlsplit(url)
        port = parsed.port
    except ValueError as exc:
        raise PublishError("API redirect contains an invalid URL authority") from exc
    scheme = parsed.scheme.lower()
    if port is None:
        port = {"http": 80, "https": 443}.get(scheme)
    return scheme, (parsed.hostname or "").lower(), port


class ApiClient:
    def __init__(self, config: Config) -> None:
        self._token = config.token
        self.api_url = config.api_url.rstrip("/")
        self.upload_url = config.upload_url.rstrip("/")
        self._opener = urllib.request.build_opener(_SafeRedirectHandler())

    def api_json(
        self,
        method: str,
        path: str,
        *,
        payload: Mapping[str, object] | None = None,
        expected: tuple[int, ...] = (200,),
    ) -> object:
        data = None
        headers: dict[str, str] = {"Accept": "application/vnd.github+json"}
        if payload is not None:
            data = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode(
                "utf-8"
            )
            headers["Content-Type"] = "application/json"
        response = self._open(
            method,
            self.api_url + path,
            data=data,
            headers=headers,
            expected=expected,
        )
        with response:
            try:
                body = response.read()
            except (http.client.HTTPException, OSError) as exc:
                raise TransportError(
                    f"response body was lost for {method} {self.api_url + path}: {exc}"
                ) from exc
        try:
            return json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PublishError(
                f"{method} {self.api_url + path} returned invalid JSON"
            ) from exc

    def upload_asset(
        self, repository: str, release_id: int, asset: AssetSpec
    ) -> object:
        query = urllib.parse.urlencode({"name": asset.name})
        path = f"/repos/{repository}/releases/{release_id}/assets?{query}"
        url = self.upload_url + path
        headers = {
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/octet-stream",
            "Content-Length": str(asset.size),
        }
        try:
            stream = asset.path.open("rb")
        except OSError as exc:
            raise PublishError(
                f"cannot open release asset {asset.path}: {exc}"
            ) from exc
        with stream:
            response = self._open(
                "POST", url, data=stream, headers=headers, expected=(201,)
            )
            with response:
                try:
                    body = response.read()
                except (http.client.HTTPException, OSError) as exc:
                    raise TransportError(
                        f"response body was lost for POST {url}: {exc}"
                    ) from exc
        try:
            return json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PublishError(f"POST {url} returned invalid JSON") from exc

    def download_digest(self, repository: str, asset_id: int) -> tuple[int, str]:
        path = f"/repos/{repository}/releases/assets/{asset_id}"
        url = self.api_url + path
        response = self._open(
            "GET",
            url,
            data=None,
            headers={"Accept": "application/octet-stream"},
            expected=(200,),
        )
        digest = hashlib.sha256()
        size = 0
        with response:
            try:
                while True:
                    chunk = response.read(READ_CHUNK_SIZE)
                    if not chunk:
                        break
                    size += len(chunk)
                    digest.update(chunk)
            except (http.client.HTTPException, OSError) as exc:
                raise TransportError(
                    f"asset {asset_id} download was interrupted: {exc}"
                ) from exc
        return size, digest.hexdigest()

    def delete(self, path: str) -> None:
        response = self._open(
            "DELETE",
            self.api_url + path,
            data=None,
            headers={"Accept": "application/vnd.github+json"},
            expected=(204,),
        )
        response.close()

    def _open(
        self,
        method: str,
        url: str,
        *,
        data: bytes | IO[bytes] | None,
        headers: Mapping[str, str],
        expected: tuple[int, ...],
    ) -> Any:
        request = urllib.request.Request(url, data=data, method=method)
        request.add_header("Authorization", f"Bearer {self._token}")
        request.add_header("X-GitHub-Api-Version", API_VERSION)
        request.add_header("User-Agent", "spektrafilm-exact-release-publisher/1")
        for name, value in headers.items():
            request.add_header(name, value)
        try:
            response = self._opener.open(request, timeout=HTTP_TIMEOUT_SECONDS)
        except urllib.error.HTTPError as exc:
            try:
                detail_bytes = exc.read(4096)
            except OSError:
                detail_bytes = b""
            detail = detail_bytes.decode("utf-8", errors="replace").strip()
            raise HttpError(method, url, exc.code, detail or exc.reason) from exc
        except (urllib.error.URLError, http.client.HTTPException, OSError) as exc:
            raise TransportError(
                f"no authoritative response for {method} {url}: {exc}"
            ) from exc
        status = getattr(response, "status", response.getcode())
        if status not in expected:
            response.close()
            raise PublishError(
                f"{method} {url} returned unexpected HTTP {status}; expected {expected}"
            )
        return response


class Publisher:
    def __init__(self, config: Config, assets: Sequence[AssetSpec]) -> None:
        self.config = config
        self.assets = tuple(assets)
        self.api = ApiClient(config)
        self.marker = f"<!-- spektrafilm-release-run:v2:{secrets.token_hex(32)} -->"
        self._owned_release_id: int | None = None

    @property
    def _repo_path(self) -> str:
        return f"/repos/{self.config.repository}"

    def publish(self) -> PublishResult:
        self._assert_immutable_releases_enabled()
        self._assert_tag_points_to_source()

        discovered = self._find_unique_tag_release()
        if discovered is not None:
            release_id = _positive_id(discovered, "release")
            release = self._get_release(release_id)
            if release.get("draft") is False:
                self._accept_published(release_id, release)
                return PublishResult(release_id, "existing")
            self._assert_owned_draft(release, release_id)
            self._owned_release_id = release_id

        try:
            if self._owned_release_id is None:
                self._create_or_recover_draft()
            owned_release_id = self._owned_release_id
            if owned_release_id is None:
                raise PublishError("release draft ownership was not established")

            self._assert_unique_release_id(owned_release_id)
            self._synchronize_assets(owned_release_id)

            # These checks deliberately sit immediately next to publication.
            self._assert_immutable_releases_enabled()
            self._assert_unique_release_id(owned_release_id)
            draft = self._get_release(owned_release_id)
            self._assert_owned_draft(draft, owned_release_id)
            self._verify_exact_assets(owned_release_id)
            self._assert_local_assets_unchanged()
            self._assert_tag_points_to_source()

            self._publish_or_recover(owned_release_id)
            return PublishResult(owned_release_id, "published")
        except PublishError as exc:
            if self._owned_release_id is not None:
                cleanup_error = self._cleanup_owned_draft(self._owned_release_id)
                if cleanup_error is not None:
                    raise PublishError(
                        f"{exc}; owned-draft cleanup was not completed: {cleanup_error}"
                    ) from exc
            raise

    def _assert_immutable_releases_enabled(self) -> None:
        value = self.api.api_json("GET", f"{self._repo_path}/immutable-releases")
        obj = _object(value, "immutable releases response")
        if obj.get("enabled") is not True:
            raise PublishError("repository immutable releases are not enabled")

    def _assert_tag_points_to_source(self) -> None:
        encoded_tag = urllib.parse.quote(self.config.tag, safe="")
        value = self.api.api_json(
            "GET", f"{self._repo_path}/git/ref/tags/{encoded_tag}"
        )
        ref = _object(value, "tag reference")
        if ref.get("ref") != f"refs/tags/{self.config.tag}":
            raise PublishError("GitHub returned a different tag reference")
        target = _object(ref.get("object"), "tag reference object")
        seen: set[str] = set()
        for _depth in range(16):
            object_type = target.get("type")
            sha = target.get("sha")
            if not isinstance(sha, str) or _SHA_PATTERN.fullmatch(sha) is None:
                raise PublishError("tag reference contains an invalid object SHA")
            if object_type == "commit":
                if sha != self.config.source_sha:
                    raise PublishError(
                        f"tag {self.config.tag} resolves to {sha}, not {self.config.source_sha}"
                    )
                return
            if object_type != "tag":
                raise PublishError(
                    f"tag reference resolves to unsupported type {object_type!r}"
                )
            if sha in seen:
                raise PublishError("annotated tag chain contains a cycle")
            seen.add(sha)
            value = self.api.api_json("GET", f"{self._repo_path}/git/tags/{sha}")
            tag_object = _object(value, "annotated tag")
            target = _object(tag_object.get("object"), "annotated tag target")
        raise PublishError("annotated tag chain exceeds 16 objects")

    def _list_releases(self) -> list[dict[str, object]]:
        result: list[dict[str, object]] = []
        for page in range(1, MAX_PAGES + 1):
            value = self.api.api_json(
                "GET",
                f"{self._repo_path}/releases?per_page={PAGE_SIZE}&page={page}",
            )
            items = _array(value, "release list")
            for item in items:
                result.append(_object(item, "release list entry"))
            if len(items) < PAGE_SIZE:
                return result
        raise PublishError("release pagination exceeded its safety limit")

    def _matching_tag_releases(self) -> list[dict[str, object]]:
        return [
            release
            for release in self._list_releases()
            if release.get("tag_name") == self.config.tag
        ]

    def _find_unique_tag_release(self) -> dict[str, object] | None:
        matches = self._matching_tag_releases()
        if len(matches) > 1:
            raise PublishError(
                f"found {len(matches)} releases for exact tag {self.config.tag}; expected at most one"
            )
        return matches[0] if matches else None

    def _assert_unique_release_id(self, release_id: int) -> None:
        release = self._find_unique_tag_release()
        if release is None or _positive_id(release, "release") != release_id:
            raise PublishError(
                f"exact tag {self.config.tag} is not uniquely bound to release ID {release_id}"
            )

    def _get_release(self, release_id: int) -> dict[str, object]:
        value = self.api.api_json("GET", f"{self._repo_path}/releases/{release_id}")
        release = _object(value, "release")
        if _positive_id(release, "release") != release_id:
            raise PublishError("exact release lookup returned a different database ID")
        if release.get("tag_name") != self.config.tag:
            raise PublishError("exact release lookup returned a different tag")
        return release

    def _assert_owned_draft(
        self, release: Mapping[str, object], release_id: int
    ) -> None:
        if _positive_id(release, "release") != release_id:
            raise PublishError("draft release database ID changed")
        if release.get("tag_name") != self.config.tag:
            raise PublishError("draft release tag changed")
        if release.get("name") != self.config.tag:
            raise PublishError("draft release name is not the exact stable tag")
        if release.get("draft") is not True:
            raise PublishError("run-owned release is no longer a draft")
        if release.get("prerelease") is not False:
            raise PublishError("run-owned release unexpectedly became a prerelease")
        if not self._body_has_run_marker(release.get("body")):
            raise PublishError("draft release is not owned by this workflow run")

    def _body_has_run_marker(self, body: object) -> bool:
        return body == self.marker

    def _create_or_recover_draft(self) -> int:
        payload = {
            "tag_name": self.config.tag,
            "target_commitish": self.config.source_sha,
            "name": self.config.tag,
            "body": self.marker,
            "draft": True,
            "prerelease": False,
            "generate_release_notes": False,
        }
        create_error: Exception | None = None
        try:
            value = self.api.api_json(
                "POST",
                f"{self._repo_path}/releases",
                payload=payload,
                expected=(201,),
            )
            created = _object(value, "created release")
            release_id = _positive_id(created, "created release")
        except PublishError as exc:
            create_error = exc
            release_id = 0

        if release_id:
            try:
                release = self._get_release(release_id)
                self._assert_owned_draft(release, release_id)
                self._owned_release_id = release_id
            except PublishError as exc:
                if create_error is None:
                    create_error = exc

        try:
            matches = self._matching_tag_releases()
        except PublishError as discovery_error:
            if create_error is not None:
                raise create_error from discovery_error
            raise
        if len(matches) > 1:
            if self._owned_release_id is None:
                candidates = [
                    release
                    for release in matches
                    if self._body_has_run_marker(release.get("body"))
                    and release.get("name") == self.config.tag
                    and release.get("draft") is True
                    and release.get("prerelease") is False
                ]
                if len(candidates) == 1:
                    candidate_id = _positive_id(candidates[0], "run-owned release")
                    candidate = self._get_release(candidate_id)
                    self._assert_owned_draft(candidate, candidate_id)
                    self._owned_release_id = candidate_id
            raise PublishError(
                f"found {len(matches)} releases for exact tag {self.config.tag}; expected at most one"
            )
        if not matches:
            if create_error is not None:
                raise create_error
            raise PublishError("created release cannot be found by its exact tag")
        discovered = matches[0]
        recovered_id = _positive_id(discovered, "recovered release")
        if release_id and recovered_id != release_id:
            raise PublishError("created release ID is not the tag's unique database ID")
        release = self._get_release(recovered_id)
        self._assert_owned_draft(release, recovered_id)
        self._owned_release_id = recovered_id
        return recovered_id

    def _list_assets(self, release_id: int) -> list[dict[str, object]]:
        result: list[dict[str, object]] = []
        for page in range(1, MAX_PAGES + 1):
            value = self.api.api_json(
                "GET",
                f"{self._repo_path}/releases/{release_id}/assets?per_page={PAGE_SIZE}&page={page}",
            )
            items = _array(value, "asset list")
            for item in items:
                result.append(_object(item, "asset list entry"))
            if len(items) < PAGE_SIZE:
                return result
        raise PublishError("asset pagination exceeded its safety limit")

    def _asset_inventory(
        self, release_id: int
    ) -> dict[str, tuple[int, Mapping[str, object]]]:
        inventory: dict[str, tuple[int, Mapping[str, object]]] = {}
        ids: set[int] = set()
        for asset in self._list_assets(release_id):
            asset_id = _positive_id(asset, "asset")
            name = asset.get("name")
            if not isinstance(name, str):
                raise PublishError("release asset has no valid name")
            if asset_id in ids:
                raise PublishError(f"duplicate asset database ID {asset_id}")
            if name in inventory:
                raise PublishError(f"duplicate release asset name {name!r}")
            ids.add(asset_id)
            inventory[name] = asset_id, asset
        return inventory

    def _synchronize_assets(self, release_id: int) -> None:
        expected = {asset.name: asset for asset in self.assets}
        inventory = self._asset_inventory(release_id)
        unexpected = sorted(set(inventory) - set(expected))
        if unexpected:
            raise PublishError(
                f"draft contains unexpected release assets: {unexpected}"
            )
        for name, (asset_id, remote) in inventory.items():
            self._verify_one_asset(expected[name], asset_id, remote)

        for asset in self.assets:
            if asset.name in inventory:
                continue
            self._assert_local_asset_unchanged(asset)
            value = self.api.upload_asset(self.config.repository, release_id, asset)
            uploaded = _object(value, "uploaded asset")
            asset_id = _positive_id(uploaded, "uploaded asset")
            if uploaded.get("name") != asset.name:
                raise PublishError("asset upload response returned a different name")
            if uploaded.get("state") != "uploaded":
                raise PublishError("asset upload did not reach uploaded state")
            uploaded_size = uploaded.get("size")
            if type(uploaded_size) is not int or uploaded_size != asset.size:
                raise PublishError(
                    "asset upload response returned a different byte size"
                )
            self._verify_one_asset(asset, asset_id, uploaded)

        self._verify_exact_assets(release_id)

    def _verify_exact_assets(self, release_id: int) -> None:
        expected = {asset.name: asset for asset in self.assets}
        inventory = self._asset_inventory(release_id)
        if set(inventory) != set(expected):
            missing = sorted(set(expected) - set(inventory))
            extra = sorted(set(inventory) - set(expected))
            raise PublishError(
                f"release asset inventory mismatch (missing={missing}, extra={extra})"
            )
        for name, asset in expected.items():
            asset_id, remote = inventory[name]
            self._verify_one_asset(asset, asset_id, remote)

    def _verify_one_asset(
        self,
        expected: AssetSpec,
        asset_id: int,
        remote: Mapping[str, object],
    ) -> None:
        if remote.get("name") != expected.name:
            raise PublishError(f"asset ID {asset_id} has a different name")
        if remote.get("state") != "uploaded":
            raise PublishError(f"asset {expected.name} is not in uploaded state")
        remote_size_value = remote.get("size")
        if type(remote_size_value) is not int or remote_size_value != expected.size:
            raise PublishError(f"asset {expected.name} has a different byte size")
        remote_size, remote_sha256 = self.api.download_digest(
            self.config.repository, asset_id
        )
        if remote_size != expected.size or remote_sha256 != expected.sha256:
            raise PublishError(
                f"asset {expected.name} download bytes do not match local input"
            )

    def _assert_local_assets_unchanged(self) -> None:
        for asset in self.assets:
            self._assert_local_asset_unchanged(asset)

    @staticmethod
    def _assert_local_asset_unchanged(asset: AssetSpec) -> None:
        try:
            stat = asset.path.stat()
        except OSError as exc:
            raise PublishError(
                f"cannot re-read release asset {asset.path}: {exc}"
            ) from exc
        if (
            not asset.path.is_file()
            or asset.path.is_symlink()
            or stat.st_size != asset.size
        ):
            raise PublishError(f"local release asset changed: {asset.path}")
        if _sha256_file(asset.path) != asset.sha256:
            raise PublishError(f"local release asset bytes changed: {asset.path}")

    def _publish_or_recover(self, release_id: int) -> None:
        publish_error: Exception | None = None
        try:
            value = self.api.api_json(
                "PATCH",
                f"{self._repo_path}/releases/{release_id}",
                payload={"draft": False},
            )
            response_release = _object(value, "published release")
            if _positive_id(response_release, "published release") != release_id:
                raise PublishError("publish response returned a different release ID")
        except PublishError as exc:
            publish_error = exc

        try:
            release = self._get_release(release_id)
            self._validate_published(release_id, release)
            self._assert_unique_release_id(release_id)
            self._verify_exact_assets(release_id)
            self._assert_local_assets_unchanged()
            self._assert_tag_points_to_source()
        except PublishError:
            if publish_error is not None:
                raise publish_error
            raise

    def _accept_published(self, release_id: int, release: Mapping[str, object]) -> None:
        self._validate_published(release_id, release)
        self._assert_unique_release_id(release_id)
        self._verify_exact_assets(release_id)
        self._assert_local_assets_unchanged()
        self._assert_immutable_releases_enabled()
        self._assert_tag_points_to_source()

    def _validate_published(
        self, release_id: int, release: Mapping[str, object]
    ) -> None:
        if _positive_id(release, "published release") != release_id:
            raise PublishError("published release database ID changed")
        if release.get("tag_name") != self.config.tag:
            raise PublishError("published release tag changed")
        if release.get("name") != self.config.tag:
            raise PublishError("published release name is not the exact stable tag")
        if release.get("draft") is not False:
            raise PublishError("release is still a draft")
        if release.get("prerelease") is not False:
            raise PublishError("stable release is marked prerelease")
        if release.get("immutable") is not True:
            raise PublishError("published release is not immutable")

    def _cleanup_owned_draft(self, release_id: int) -> PublishError | None:
        try:
            release = self._get_release(release_id)
            self._assert_owned_draft(release, release_id)
            self.api.delete(f"{self._repo_path}/releases/{release_id}")
            return None
        except HttpError as exc:
            if exc.status == 404:
                return None
            return exc
        except PublishError as exc:
            # A published, renamed, retagged, or foreign release is never deleted.
            return exc


def _positive_id(value: Mapping[str, object], description: str) -> int:
    object_id = value.get("id")
    if type(object_id) is not int or object_id <= 0:
        raise PublishError(f"{description} has no positive integer database ID")
    return object_id


def _object(value: object, description: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise PublishError(f"{description} is not a JSON object")
    if not all(isinstance(key, str) for key in value):
        raise PublishError(f"{description} contains a non-string key")
    return value


def _array(value: object, description: str) -> list[object]:
    if not isinstance(value, list):
        raise PublishError(f"{description} is not a JSON array")
    return value


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(READ_CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as exc:
        raise PublishError(f"cannot hash release asset {path}: {exc}") from exc
    return digest.hexdigest()


def _validate_base_url(value: str, name: str) -> str:
    try:
        parsed = urllib.parse.urlsplit(value)
        hostname = parsed.hostname
        _port = parsed.port
    except ValueError as exc:
        raise PublishError(f"{name} contains an invalid URL authority") from exc
    if parsed.scheme not in {"http", "https"} or not hostname:
        raise PublishError(f"{name} must be an absolute HTTP(S) URL")
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise PublishError(
            f"{name} must not contain credentials, a query, or a fragment"
        )
    if parsed.scheme == "http" and hostname not in {
        "127.0.0.1",
        "localhost",
        "::1",
    }:
        raise PublishError(f"{name} must use HTTPS except on loopback")
    return value.rstrip("/")


def _required_env(environ: Mapping[str, str], name: str) -> str:
    value = environ.get(name, "")
    if not value or "\n" in value or "\r" in value:
        raise PublishError(
            f"required environment variable {name} is missing or invalid"
        )
    return value


def _load_assets(directory: pathlib.Path) -> tuple[AssetSpec, ...]:
    if directory.is_symlink():
        raise PublishError(f"assets directory must not be a symlink: {directory}")
    try:
        resolved = directory.resolve(strict=True)
    except OSError as exc:
        raise PublishError(f"assets directory does not exist: {directory}") from exc
    if not resolved.is_dir() or resolved.is_symlink():
        raise PublishError(f"assets path is not a real directory: {resolved}")
    try:
        children = sorted(resolved.iterdir(), key=lambda path: path.name)
    except OSError as exc:
        raise PublishError(
            f"cannot enumerate assets directory {resolved}: {exc}"
        ) from exc
    if not children:
        raise PublishError("assets directory is empty")

    assets: list[AssetSpec] = []
    seen_casefolded: set[str] = set()
    for path in children:
        if _ASSET_NAME_PATTERN.fullmatch(path.name) is None:
            raise PublishError(f"unsafe release asset name: {path.name!r}")
        folded = path.name.casefold()
        if folded in seen_casefolded:
            raise PublishError(
                f"case-insensitive duplicate release asset: {path.name!r}"
            )
        seen_casefolded.add(folded)
        if path.is_symlink() or not path.is_file():
            raise PublishError(
                f"release asset is not a regular non-symlink file: {path}"
            )
        try:
            size = path.stat().st_size
        except OSError as exc:
            raise PublishError(f"cannot stat release asset {path}: {exc}") from exc
        assets.append(AssetSpec(path.name, path, size, _sha256_file(path)))
    return tuple(assets)


def _build_config(
    args: argparse.Namespace, environ: Mapping[str, str]
) -> tuple[Config, tuple[AssetSpec, ...]]:
    tag = args.tag
    if _TAG_PATTERN.fullmatch(tag) is None:
        raise PublishError(f"tag {tag!r} is not an exact stable vMAJOR.MINOR.PATCH tag")
    source_sha = args.source_sha
    if _SHA_PATTERN.fullmatch(source_sha) is None:
        raise PublishError(
            "source SHA must be exactly 40 lowercase hexadecimal characters"
        )
    repository = _required_env(environ, "GITHUB_REPOSITORY")
    if _REPOSITORY_PATTERN.fullmatch(repository) is None:
        raise PublishError("GITHUB_REPOSITORY must be an exact owner/name pair")
    run_id = _required_env(environ, "GITHUB_RUN_ID")
    run_attempt = _required_env(environ, "GITHUB_RUN_ATTEMPT")
    if not run_id.isdecimal() or int(run_id) <= 0:
        raise PublishError("GITHUB_RUN_ID must be a positive decimal integer")
    if not run_attempt.isdecimal() or int(run_attempt) <= 0:
        raise PublishError("GITHUB_RUN_ATTEMPT must be a positive decimal integer")
    token = _required_env(environ, "GITHUB_TOKEN")
    api_url = _validate_base_url(
        environ.get("GITHUB_API_URL", DEFAULT_API_URL), "GITHUB_API_URL"
    )
    upload_url = _validate_base_url(
        environ.get("GITHUB_UPLOAD_URL", DEFAULT_UPLOAD_URL), "GITHUB_UPLOAD_URL"
    )
    assets = _load_assets(pathlib.Path(args.assets_dir))
    return (
        Config(
            token=token,
            repository=repository,
            run_id=run_id,
            run_attempt=run_attempt,
            tag=tag,
            source_sha=source_sha,
            assets_dir=pathlib.Path(args.assets_dir),
            api_url=api_url,
            upload_url=upload_url,
        ),
        assets,
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Publish a verified directory as one exact immutable GitHub release"
    )
    parser.add_argument("--tag", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--assets-dir", required=True)
    return parser


def publish_from_args(
    argv: Sequence[str], environ: Mapping[str, str] | None = None
) -> PublishResult:
    args = _parser().parse_args(list(argv))
    config, assets = _build_config(args, os.environ if environ is None else environ)
    return Publisher(config, assets).publish()


def _workflow_command_escape(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main(argv: Sequence[str] | None = None) -> int:
    try:
        result = publish_from_args(
            sys.argv[1:] if argv is None else argv,
            os.environ,
        )
    except PublishError as exc:
        print(f"::error::{_workflow_command_escape(str(exc))}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {"release_id": result.release_id, "status": result.status},
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
