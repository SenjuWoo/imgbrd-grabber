#ifndef LIBRARY_IMPORTER_H
#define LIBRARY_IMPORTER_H

#include <QByteArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

class QImage;

struct LibraryImportData
{
	QString path, sha256, md5, sourceMd5, visualHash, title;
	QSize size;
	QJsonObject metadata;
	QStringList sourceUrls, tags;
	QByteArray thumbnail;
	QString error;
	bool copied = false;
};

class LibraryImporter
{
	public:
		static QStringList imageFiles(const QStringList &roots, const std::shared_ptr<std::atomic_bool> &cancel = {});
		static LibraryImportData inspect(const QString &path, const QString &managedDirectory = {}, const std::shared_ptr<std::atomic_bool> &cancel = {});
		static QString visualHash(const QImage &image);
		static int visualDistance(const QString &left, const QString &right);
};

#endif // LIBRARY_IMPORTER_H
