# Library milestone

Implement a native Qt Library tab, portable SQLite catalog, independent likes and
favorites, and named collections with covers and picture notes. Search thumbnails,
image context menus, the viewer and Library share the same actions and live state.
An image may belong to several collections; each collection has its own preference
state. Library-wide preferences remain separate. Favorites are a stronger explicit
signal than likes; no recommendation engine or cloud processing ships in this step.

Saved tag searches and monitors keep their existing files and settings. Their tab
is named Saved searches so picture favorites are unambiguous. Library removal only
removes catalog records and cached previews, never downloaded originals.

Store source-qualified image identity, existing Image JSON, and bounded cached
thumbnails in library.sqlite in the profile. Preserve ratings when source metadata
refreshes. Refuse corrupt, unrecognized or newer schemas without replacing files.
SQLite foreign keys protect memberships; schema creation is transactional. No new dependency.

Verification: real SQLite reopen, source identity collisions, many-to-many and
collection-specific state, gallery children, safe deletion, corrupt/future schemas;
real GUI action propagation, collection operations and rendered Library; existing
build-local.ps1 suite and clean portable packaging. Deploy only after these pass,
backing up and comparing personal profile files. Keep one installed app and source.

Home recommendations, creator following, semantic search, imports and new sources
are subsequent milestones. Home can later mix explicit Library-wide and collection
signals; collection recommendations use only the chosen collection by default.
