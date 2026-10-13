#include "models/library-health.h"
#include <QFile>
#include <QObject>
#include <algorithm>
#include <initializer_list>


LibraryHealth LibraryHealth::compute(const QList<LibraryEntry> &entries, const QSet<QString> &sites, const QList<QStringList> &duplicates, const QStringList &unrated, const std::function<bool(const QString &)> &exists)
{
	LibraryHealth health;
	LibraryHealthIssue missing { "missing-files", QObject::tr("Files missing from disk"), QObject::tr("The picture was moved or deleted. Locate it again, or remove it from the Library."), {} };
	LibraryHealthIssue thumbnails { "no-thumbnail", QObject::tr("No thumbnail"), QObject::tr("Recheck metadata (Import ▸ Recheck) or reopen the picture to rebuild it."), {} };
	LibraryHealthIssue untagged { "no-tags", QObject::tr("No tags"), QObject::tr("Discover cannot learn from these. Use Find sources for pictures without tags."), {} };
	LibraryHealthIssue foreign { "unknown-source", QObject::tr("Source is not in your sources"), QObject::tr("Add that website to your sources, or link the picture to one you use."), {} };
	LibraryHealthIssue errors { "metadata-errors", QObject::tr("Metadata errors"), QObject::tr("The file's embedded metadata could not be read fully."), {} };
	LibraryHealthIssue copies { "duplicates", QObject::tr("Duplicate copies"), QObject::tr("The same picture twice. Merge all duplicates in the Library."), {} };
	LibraryHealthIssue leftovers { "not-rated", QObject::tr("Not rated"), QObject::tr("Neither liked nor favorited and in no collection. Remove them or rate them."), {} };

	for (const auto &entry : entries) {
		++health.total;
		health.liked += entry.liked ? 1 : 0;
		health.favorites += entry.favorite ? 1 : 0;
		if (!entry.localPaths.isEmpty() && std::none_of(entry.localPaths.cbegin(), entry.localPaths.cend(), exists)) {
			missing.keys.append(entry.key);
		}
		if (entry.thumbnail.isEmpty()) {
			thumbnails.keys.append(entry.key);
		}
		if (entry.tags().isEmpty()) {
			untagged.keys.append(entry.key);
		}
		const QString website = entry.image.value("website").toString();
		if (!website.isEmpty() && !sites.contains(website)) {
			foreign.keys.append(entry.key);
		}
		if (!entry.metadataErrors().isEmpty()) {
			errors.keys.append(entry.key);
		}
	}
	for (const auto &group : duplicates) {
		copies.keys.append(group.mid(1));
	}
	leftovers.keys = unrated;

	for (const auto &issue : { missing, thumbnails, untagged, foreign, errors, copies, leftovers }) {
		if (!issue.keys.isEmpty()) {
			health.issues.append(issue);
		}
	}
	return health;
}

const LibraryHealthIssue *LibraryHealth::issue(const QString &id) const
{
	for (const auto &issue : issues) {
		if (issue.id == id) {
			return &issue;
		}
	}
	return nullptr;
}
