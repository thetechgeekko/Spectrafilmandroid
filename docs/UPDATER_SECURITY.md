# Advisory updater security contract

Spektrafilm uses a **browser-only release-page design**. `AppUpdater` may tell the user that a newer
stable tag exists, but it does not download an APK, parse an asset URL, invoke PackageInstaller, or
verify a release artifact. This boundary is deliberate: the repository does not ship an independent
signed update-metadata format or an application-trusted release public key, so claiming in-app
signature/hash verification would be false.

## Accepted request and response

The only in-app request is:

```text
GET https://api.github.com/repos/thetechgeekko/Spektrafilm-android/releases/latest
```

The request contract is fail closed:

- exact HTTPS scheme, `api.github.com` host, default port, repository path, and no user-info, query, or
  fragment;
- platform system trust anchors and cleartext disabled;
- connection and read timeouts of 5 seconds each;
- automatic redirects disabled; any 3xx is failure rather than a second request;
- HTTP 200 only, with `application/json` or `application/vnd.github+json` media type;
- declared and streamed response size limited to 64 KiB;
- strict UTF-8 decoding and fail-closed handling for a non-progressing response stream; and
- no cache or persistent updater state.

Accepted metadata must contain exact boolean `draft: false` and `prerelease: false`, a canonical
`vMAJOR.MINOR.PATCH` tag without leading-zero components, and this exact browser destination:

```text
https://github.com/thetechgeekko/Spektrafilm-android/releases/tag/<same-tag>
```

HTTPS downgrade, subdomain/suffix confusion, user-info, alternate repository/path/tag, query/fragment,
nightly/prerelease metadata, redirects, wrong media types, malformed JSON, and oversized bodies all
return "couldn't check". The URL is validated again immediately before creating the browser Intent,
so even caller-constructed `UpdateInfo` cannot select another destination.

## Integrity boundary

This design provides bounded advisory metadata over GitHub TLS and a pinned browser destination. It
does **not** mean the app has verified the APK bytes, `SHA256SUMS`, release signer, Git tag, workflow
provenance, or GitHub account authorization. The protected release workflow may publish those
artifacts, but the installed app does not cryptographically bind its API response to them.

For an upgrade, Android's package manager requires signing-certificate continuity with the installed
app. For a first install, the user and chosen distribution channel must establish provenance. The app
does not request `REQUEST_INSTALL_PACKAGES`, so it cannot bypass that user-visible installation flow.

If a future release downloads artifacts in-app, it is a new security design. At minimum it needs a
versioned canonical metadata format, an offline application-trusted verification key and rotation
policy, signature verification before trusting hashes/URLs, an exact APK digest and byte limit,
package-name/signing-lineage checks, atomic storage/cleanup, downgrade/replay protection, and hostile
parser/redirect/TLS tests. A checksum fetched from the same unauthenticated response as an APK is not
an independent integrity control.

## Verification

`AppUpdaterTest` covers valid stable metadata plus redirect, media-type, declared/streamed oversize,
scheme/host/user-info/repository/path/tag/query, draft/prerelease, malformed tag, malformed UTF-8,
non-progressing response streams, and browser-handoff revalidation failures. Android
resource/manifest tests verify cleartext denial, system trust, the exact
documented API domain, and absence of package-install permission.

Android's declarative TLS/cleartext controls are documented in
[Network Security Configuration](https://developer.android.com/privacy-and-security/security-config).
