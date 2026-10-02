# Grabber Woo Edit backend audit

Audit date: October 1, 2026. Application version: 7.15.0.

## Source and dependency baseline

The fork contains upstream develop `3eb00bcbc47f36e37186fc6af03919d4916e2b35`
and master `56f673c6e821567e5a8af5361bf8004c62a55052`. A fresh remote check
found no newer commits on either branch at this audit.

Release builds pin Qt 6.11.3. Its available package metadata identifies the
September 24 build; the [Qt vulnerability list](https://wiki.qt.io/List_of_known_vulnerabilities_in_Qt_products)
documents fixes to network/XML/SVG parsing in recent Qt patch releases.
Windows CI replaces unsupported OpenSSL 3.1.4 with **3.5.9 LTS**, downloaded
from its [Windows binary publisher](https://slproweb.com/products/Win32OpenSSL.html)
and checked against a pinned SHA-256. The [OpenSSL release table](https://openssl-library.org/source/)
lists this LTS branch's support lifecycle. Qt requires the OpenSSL 3 ABI; OpenSSL
4 is not substituted merely because its version is newer.

The clean Lexbor submodule moves from the old 2.1.0 snapshot to upstream
[3.0.0](https://github.com/lexbor/lexbor/releases/tag/v3.0.0), with the changed CSS
API integrated. HTML documents now have shared ownership across child wrappers;
CSS queries and serialization release their temporary allocations. UTF-8 inputs
use byte counts rather than UTF-16 character counts. Existing source/parser
fixtures and added Unicode/child-lifetime checks verify compatibility.

Jest and ts-jest move to compatible current patch versions. TypeScript 5.9.3
remains the established transpiler; a major compiler migration is not required
to fix the dependency alerts. `npm audit` reports zero vulnerabilities after
the lockfile repair. CI repeats the audit and lint. Source CI uses Node.js 24
instead of end-of-life Node.js 16.

## APIs: observed availability versus parser proof

One anonymous read-only request per source was made from the audit host. These
responses identify access behavior, not authenticated end-to-end download proof.
No personal credentials were modified or included in this report.

| Source | Observed response | Meaning and changes |
| --- | --- | --- |
| Danbooru | HTTP 200, JSON | Public listing was reachable. Existing adapter retained. |
| Kemono | HTTP 200, JSON | The current `/api/v1/posts` endpoint was reachable. Existing v1 adapter retained. |
| Pixiv | HTTP 400, JSON without a token | Authentication remains required. Search/gallery/details report API errors or missing fields explicitly. |
| Reddit | HTTP 403 | Access denied on this host. The adapter now reports this directly; changing tags cannot fix access denial. |
| Gelbooru | HTTP 401 | Authentication required on the probed endpoint. Personal API settings remain unchanged. |
| e926 | HTTP 403, HTML | Host access was blocked. Parser fixtures do not prove live account access. |

Reddit's [API documentation](https://www.reddit.com/dev/api/) defines cursor
pagination. The parser now receives the actual request URL, preserves its
filters, replaces its cursor, and leaves total image count unknown. Previously,
numbered pages could repeat the first page and per-page media count was mistaken
for a total. Author keywords now use the search endpoint, empty subreddit queries
use listings, and crosspost media can be recovered from the parent.

The [Reddit access guidance](https://support.reddithelp.com/hc/en-us/articles/14945211791892-Developer-Platform-Accessing-Reddit-Data)
explains that API approval/authentication may be necessary. This audit does not
invent or embed an approved Reddit client. Pixiv's app API is an existing
integration, not a publicly guaranteed official API contract. Pinterest and
Internet-wide similarity services remain outside this release.

## Updates, packaging, and release contract

Update checks reject HTTP errors, invalid JSON, missing tags, and invalid release
URLs. An unsuccessful check does not emit the signal used for an up-to-date
verdict. Requests have an inactivity timeout and reject HTTPS downgrades.
Legacy fork revisions remain comparable, and 7.15.0 upgrades old Fable versions.

Windows CI now executes GUI and crash-reporter tests as well as library/CLI
tests; each failure propagates rather than being overwritten by a later command.
The upstream Homebrew publisher is disabled on forks. Source metadata names the
actual repository, and fork source publication is deferred until the matching
application commit is green instead of publishing independently of its checks.

Portable packaging starts from a clean runtime, includes compiled sources,
translations, TLS and SQLite plugins, and records commit/file hashes. Personal
settings and catalogs are never release assets. Public ZIP verification includes
fresh extraction, CRCs, required/forbidden files, manifest hashes, real entrypoint
execution, and profile-preserving installed deployment. Source tests alone are
not proof of a release archive; exact CI/artifact evidence belongs in release notes.
