# Fable maintenance

Keep one portable app beside this checkout. The app profile stays outside Git:
settings.ini, site settings/cookies, blacklists, history, favorites, tabs, queued
downloads and the MD5 database are personal data. Never copy them into source or CI.

## Update and rebuild

1. Inspect `git status` and `git diff`; preserve existing edits.
2. Fetch `origin` (SenjuWoo) and `upstream` (Bionus). Compare both upstream branches.
   Merge into `develop`; resolve lockfile conflicts by keeping the actual newer dependency.
3. Run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-local.ps1`.
   It discovers Qt/OpenSSL/Ninja from env or the CMake cache and MSVC with vswhere,
   builds the desktop GUI/CLI, runs CTest, site tests and site lint, then stages a
   clean portable package in `release/`. Qt 6.9.3 and OpenSSL 3 are the current local SDKs.
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
- Source defaults to 7.14.0-fable.3. This is a local build revision, not a claim that
  a new public release exists. v7.14.0-fable.2 remains the latest published archive.

- CTest now launches Jest through Node directly and runs CLI fixtures from src/lib.
  The save-failure regression uses a temporary file as the destination parent,
  so it works on hosts where Z: is a real drive. Packaging explicitly deploys
  the OpenSSL backend along with the verified OpenSSL runtime libraries.
- CI uses the current all_os Android Qt repository. The release-tag regression
  runs in stable builds because nightlies use commit hashes instead of release tags.
