#ifndef SITE_MIRRORS_H
#define SITE_MIRRORS_H

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>


/** Source addresses that serve one and the same database, so searching all of them only repeats every request. */
namespace SiteMirrors
{
	/** Key shared by every address of one server; any other address is its own key. */
	inline QString group(const QString &url)
	{
		// danbooru.donmai.us is balanced over these hosts: same posts, same ids, same MD5s.
		static const QSet<QString> danbooru {
			QStringLiteral("danbooru.donmai.us"), QStringLiteral("hijiribe.donmai.us"),
			QStringLiteral("sonohara.donmai.us"), QStringLiteral("shima.donmai.us"),
		};
		return danbooru.contains(url) ? QStringLiteral("danbooru.donmai.us") : url;
	}

	/** Keeps the first address of each server; `dropped` (optional) receives the skipped ones. */
	inline QStringList unique(const QStringList &urls, QStringList *dropped = nullptr)
	{
		QStringList kept;
		QSet<QString> seen;
		for (const QString &url : urls) {
			if (seen.contains(group(url))) {
				if (dropped != nullptr) {
					dropped->append(url);
				}
				continue;
			}
			seen.insert(group(url));
			kept.append(url);
		}
		return kept;
	}
}

#endif // SITE_MIRRORS_H
