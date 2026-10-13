#ifndef LIBRARY_HEALTH_H
#define LIBRARY_HEALTH_H

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include "models/library-store.h"


/** One kind of problem found in the Library, with the pictures it affects. */
struct LibraryHealthIssue
{
	QString id;
	QString title;
	QString hint;
	QStringList keys;
};

/** Overview of what is wrong with the Library. Read-only: it never changes a picture. */
struct LibraryHealth
{
	int total = 0;
	int liked = 0;
	int favorites = 0;
	QList<LibraryHealthIssue> issues; // Only kinds that have at least one picture.

	/**
	 * `sites` are the addresses of the user's sources, `duplicates` the groups of LibraryStore::duplicateGroups() (keep first,
	 * the others are copies) and `unrated` the keys of LibraryStore::unratedKeys().
	 */
	static LibraryHealth compute(const QList<LibraryEntry> &entries, const QSet<QString> &sites, const QList<QStringList> &duplicates, const QStringList &unrated, const std::function<bool(const QString &)> &exists);
	const LibraryHealthIssue *issue(const QString &id) const;
};

#endif // LIBRARY_HEALTH_H
