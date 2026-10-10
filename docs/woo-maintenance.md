# Grabber Woo Edit maintenance

Keep one portable app beside this checkout. The app profile stays outside Git:
settings.ini, site settings/cookies, blacklists, history, favorites, tabs, queued
downloads, the MD5 database, library.sqlite, discover.json (Discover's seen/hidden history),
models/ and recommendations/ are personal data. Never copy them into source or CI.
`src/dist/common/defaults/` holds the shipped first-run defaults (sources, presets, blacklist);
they apply only when a profile's settings.ini is missing or empty.

## Update and rebuild

1. Inspect `git status` and `git diff`; preserve existing edits.
2. Fetch `origin` (SenjuWoo) and `upstream` (Bionus). Compare both upstream branches.
   Merge into `develop`; resolve lockfile conflicts by keeping the actual newer dependency.
3. Run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-local.ps1`.
   It discovers Qt/OpenSSL/Ninja from env or the CMake cache and MSVC with vswhere,
   builds the desktop GUI/CLI, runs CTest, site tests and site lint, then stages a
   clean portable package in `release/`. CI pins the release SDKs; an older local SDK is a development compatibility check, not the release runtime.
4. Before deployment, close Grabber and make a verified ZIP of its personal files.
   Compare the app against its package-manifest.json: retain personal files and
   deliberately changed assets; take unchanged runtime files from the fresh package.
   Copy the profile into the package, verify JSON/SQLite and hashes, smoke-test it,
   then swap folders. Do not overlay new Qt DLLs onto an old runtime tree.
5. Keep the app, this checkout and one compact profile backup. Remove obsolete
   app copies, cached archives and generated packaging/coverage folders only after
   the replacement runs. Keep build/ and its CMake cache for the next edit.
6. Commit and push scoped source changes. Publishing a release requires the user's
   authorization and successful checks on the exact pushed SHA.

For a new checkout, install the normal upstream prerequisites and set QT_ROOT_DIR,
OPENSSL_ROOT_DIR and put Ninja, CMake, Node/npm in PATH. The scripts contain no
machine-specific drive letters or account names. They do not deploy over a live app.

The Windows portable ZIP includes official x64 Microsoft C++ runtime DLLs beside
Qt and the AI runtime, so it does not require running a separate runtime installer.
Package startup checks verify that Qt and Microsoft C++ libraries load from the
package directory. App updates also refresh these app-local libraries. See
[Microsoft deployment documentation](https://learn.microsoft.com/en-us/cpp/windows/deployment-in-visual-cpp).

## September 29, 2026 audit

- Official v7.14.0 was released August 14; the fresh installer is older than the
  local Fable build, not a replacement for its custom fixes.
- Merged develop 3eb00bcb (September 26): transparent video overlay and Java CI update.
- Merged master 56f673c6 (September 26): js-yaml 3.15.2; retain newer lockfile entries.
- The Gelbooru 0.2 XML error fix was already superseded by upstream's implementation
  in 7.14.0. No new upstream commit replaces the fork's remaining network, Kemono,
  sorting, theme or translation changes.
- The fork updater previously checked Bionus and treated -fable.N as an alpha suffix.
  It downloaded the official installer. It now checks SenjuWoo, reads tag_name,
  compares Fable revisions and opens the portable release page.
- The installed source scripts and themes matched the old generated package.
  Per-site settings and all profile files are retained; obsolete template language
  output and empty disabled sources are omitted from clean packages.
- At this audit, source defaulted to 7.14.0-fable.3. This was a local build revision, not a claim that
  a new public release exists. v7.14.0-fable.2 was the latest published archive at that audit.

- CTest now launches Jest through Node directly and runs CLI fixtures from src/lib.
  The save-failure regression uses a temporary file as the destination parent,
  so it works on hosts where Z: is a real drive. Packaging explicitly deploys
  the OpenSSL backend along with the verified OpenSSL runtime libraries.
- CI uses the current all_os Android Qt repository. The release-tag regression
  runs in stable builds because nightlies use commit hashes instead of release tags.
- Linux CI restores execute permissions on the Android SDK CMake wrappers; the
  shared Qt archive otherwise fails before configuration with exit code 126.

## Library milestone (7.14.0-fable.4)

- Native picture Library with independent likes/favorites, collection memberships,
  scope-specific ratings/notes, covers, text filters and batch actions.
- Shared image actions on search thumbnails, context menus, viewer and Library.
- Saved search files/monitors are unchanged; their UI is named Saved searches.
- SQLite schema version 1 and cached thumbnails live in library.sqlite. Restore
  corrupt databases from a backup; do not delete them to make startup succeed.
- Catalog removal never deletes originals or changes the download MD5 index.
- The portable package includes a blank settings.ini but excludes it from runtime hashes;
  a real profile replaces it. The SQLite driver is checked during packaging.

Implementation scope and verification are in [library-milestone.md](library-milestone.md).


## Foundation and import milestone (7.14.0-fable.5)

- File save/copy/move/link failures are propagated; failed moves retain the download
  index. Hashing streams bytes and unavailable files have no invented empty-file hash.
- Downloads check exact writes and flushes, retain initial bytes for HTML detection
  after streaming, and keep 64-bit byte counters. Disk failures cannot bypass validation.
- MD5 index replacement is one rollback-safe transaction, including chunk failures.
- FFmpeg/ImageMagick stage conversion output and atomically promote it after success.
  Failed/missing backends preserve existing destinations; same-file conversion is a no-op.
- Viewer navigation keeps the correct site and reuses its details window; unstarted
  thumbnail requests can be closed safely. Search input is debounced in Library.
- Offline local-file imports, metadata evidence, SHA-256 duplicate identity, MD5 source
  lookup, explicit local visual candidates, and stable source-link merges. Shared image
  actions follow aliases when a source entry is merged while its viewer remains open.
- Library schema 2 adds local paths and source aliases. Version 1 is backed up with
  SQLite VACUUM INTO before a transactional upgrade. Managed paths are profile-relative.
- ZIP backups check extraction/finalization failures, support Unicode paths, reject
  path escapes and preserve prior files on CRC/write failure.
- Built-in backup/restore includes consistent Library snapshots and managed media.
  Catalog restore retains the live store object and replaces records transactionally.

Deliberate scope: source lookup uses the selected source's existing authentication/API
support; not every website supports MD5 searches. Visual matching searches cached source
images already in Library and requires confirmation. No automatic local-file upload or
global reverse-search service is implemented. Supported import formats come from the
installed Qt image plugins. Since 7.15.3, images above 40 million pixels are retained
without an internal preview, keeping their metadata, original-file links and ratings.
The offline viewer shows up to a 4096-pixel preview and an animation's first
frame. Open original uses the system viewer for full-resolution/animated viewing.

The audit targets verified correctness and preservation failures; it is not a claim
that every old component has been rewritten or that live source availability is proven.
Architecture remains native Qt with no additional dependency. Optional ExifTool adds
embedded metadata coverage when discovered locally; basic import works without it.

Primary implementation references: [ExifTool JSON output](https://exiftool.sourceforge.net/exiftool_pod.html),
[Qt image decoding](https://doc.qt.io/qt-6/qimagereader.html),
[Windows download origin](https://techcommunity.microsoft.com/blog/microsoftdefenderatpblog/hunting-tip-of-the-month-browser-downloads/220454),
and [SQLite consistent snapshots](https://www.sqlite.org/lang_vacuum.html).

## Site development dependencies

The npm override keeps argparse at 2.0.1 to remove the vulnerable sprintf-js dependency while retaining the legacy CLI aliases used by js-yaml 3 in the lint and coverage tools. Argparse 3 removes those aliases. Check updates with a fresh npm ci, the YAML CLI, and npm run check/build/test before changing the override. Development dependencies are excluded from the portable app.
