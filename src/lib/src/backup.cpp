#include "backup.h"
#include <QFile>
#include <QDirIterator>
#include <QFileInfo>
#include <QScopeGuard>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>
#include "functions.h"
#include "logger.h"
#include "models/favorite.h"
#include "models/profile.h"
#include "models/library-store.h"
#include "utils/file-utils.h"
#include "models/md5-database/md5-database-sqlite.h"
#include "utils/zip.h"
#include "reverse-search/reverse-search-engine.h"
#include "reverse-search/reverse-search-loader.h"


namespace
{
	bool backupDirectoryFiles(const QString &path, const QString &prefix, QHash<QString, QString> &files)
	{
		const QDir directory(path);
		if (!directory.exists()) {
			return true;
		}
		const QDir canonical(QFileInfo(path).canonicalFilePath());
		QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
		while (iterator.hasNext()) {
			const QString file = iterator.next();
			const QString relative = canonical.relativeFilePath(iterator.fileInfo().canonicalFilePath());
			if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) {
				return false;
			}
			files.insert(file, prefix + relative);
		}
		return true;
	}

	bool restoreBackupDirectory(const QString &source, const QString &target)
	{
		if (!QFileInfo::exists(source)) {
			return true;
		}
		if (!QDir().mkpath(target)) {
			return false;
		}
		QHash<QString, QString> files;
		if (!backupDirectoryFiles(source, QString(), files)) {
			return false;
		}
		const QDir canonicalTarget(QFileInfo(target).canonicalFilePath());
		for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
			const QString destination = QDir(target).filePath(it.value());
			if (QFileInfo(destination).isSymLink() || !ensureFileParent(destination)) {
				return false;
			}
			const QString parent = canonicalTarget.relativeFilePath(QFileInfo(QFileInfo(destination).absolutePath()).canonicalFilePath());
			if (parent == ".." || parent.startsWith("../") || QDir::isAbsolutePath(parent) || !atomicCopyFile(it.key(), destination)) {
				return false;
			}
		}
		return true;
	}
}

bool saveBackup(Profile *profile, const QString &filePath)
{
	QHash<QString, QString> files;

	// Save any pending changes
	profile->sync();

	// Common files
	static const QStringList backupFiles { "settings.ini", "favorites.json", "viewitlater.txt", "ignore.txt", "wordsc.txt", "blacklist.txt", "monitors.json", "restore.igl", "tabs.json", "history.json", "md5s.txt", "md5s.sqlite", "filenamehistory.txt" };
	for (const QString &file : backupFiles) {
		files.insert(profile->getPath() + "/" + file, file);
	}

	// Favorite thumbnails
	for (const Favorite &fav : profile->getFavorites()) {
		const QString relPath = "thumbs/" + fav.getName(true) + ".png";
		const QString favPath = savePath(relPath);
		files.insert(favPath, relPath);
	}

	// Web services icons
	ReverseSearchLoader loader(profile->getSettings());
	for (const auto &rse : loader.getAllReverseSearchEngines()) {
		const QString file = "webservices/" + QString::number(rse.id()) + ".ico";
		files.insert(profile->getPath() + "/" + file, file);
	}

	// Filter non-existing files
	QHash<QString, QString> zipFiles;
	for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
		if (QFile::exists(it.key())) {
			zipFiles.insert(it.key(), it.value());
		}
	}

	QTemporaryDir librarySnapshot;
	if (!librarySnapshot.isValid() || !profile->library()->backupTo(librarySnapshot.filePath("library.sqlite"))
		|| !backupDirectoryFiles(profile->getPath() + "/library-media", "library-media/", zipFiles)) {
		log("Could not snapshot the Library for backup", Logger::Error);
		return false;
	}
	zipFiles.insert(librarySnapshot.filePath("library.sqlite"), "library.sqlite");

	// Close SQLite connections while copying files
	auto *md5Database = qobject_cast<Md5DatabaseSqlite*>(profile->md5Database());
	if (md5Database != nullptr) {
		md5Database->close();
	}
	auto reopenMd5 = qScopeGuard([md5Database]() {
		if (md5Database != nullptr) {
			md5Database->load();
		}
	});

	// Create the backup ZIP
	const bool ok = createZip(filePath, zipFiles);
	if (!ok) {
		log("Failed to create backup ZIP file", Logger::Error);
	}

	return ok;
}

bool loadBackup(Profile *profile, const QString &filePath)
{
	// Create a temporary directory to store the extracted backup
	QTemporaryDir tmpDir;
	if (!tmpDir.isValid()) {
		log("Failed to create temporary directory to extract backup file", Logger::Error);
		return false;
	}

	// Unzip file
	if (!unzipFile(filePath, tmpDir.path())) {
		log("Failed to extract backup ZIP file", Logger::Error);
		return false;
	}

	const QString libraryFile = tmpDir.filePath("library.sqlite");
	if (QFileInfo::exists(libraryFile)) {
		LibraryStore incoming(libraryFile);
		if (!incoming.isReady()) {
			log("Could not validate the incoming Library: " + incoming.lastError(), Logger::Error);
			return false;
		}
	}

	// Save any pending settings changes
	profile->getSettings()->sync();

	// Close SQLite connections while copying files
	auto *md5Database = qobject_cast<Md5DatabaseSqlite*>(profile->md5Database());
	if (md5Database != nullptr) {
		md5Database->close();
	}
	auto reopenMd5 = qScopeGuard([md5Database]() {
		if (md5Database != nullptr) {
			md5Database->load();
		}
	});

	// Common files
	static const QStringList backupFiles { "settings.ini", "favorites.json", "viewitlater.txt", "ignore.txt", "wordsc.txt", "blacklist.txt", "monitors.json", "history.json", "md5s.txt", "md5s.sqlite", "filenamehistory.txt" };
	for (const QString &file : backupFiles) {
		const QString source = tmpDir.filePath(file);
		const QString target = profile->getPath() + "/" + file;

		if (QFile::exists(source)) {
			if (QFileInfo(target).isSymLink() || !atomicCopyFile(source, target)) {
				log("Could not restore " + file, Logger::Error);
				return false;
			}
		}
	}

	// Directories
	static const QMap<QString, QString> backupDirs {
		{"thumbs/", savePath("thumbs")},
		{"webservices/", profile->getPath() + "/webservices/"},
	};
	for (auto it = backupDirs.constBegin(); it != backupDirs.constEnd(); ++it) {
		const QString source = tmpDir.filePath(it.key());
		const QString &target = it.value();

		if (!restoreBackupDirectory(source, target)) {
			log("Could not restore " + it.key(), Logger::Error);
			return false;
		}
	}

	if (!restoreBackupDirectory(tmpDir.filePath("library-media"), profile->getPath() + "/library-media")
		|| (QFileInfo::exists(libraryFile) && !profile->library()->restoreFrom(libraryFile))) {
		log("Could not restore the Library and its managed media", Logger::Error);
		return false;
	}

	if (md5Database != nullptr) {
		md5Database->load();
	}
	reopenMd5.dismiss();
	// Reload the profile
	profile->reload();

	// TODO(Bionus): restore.igl, tabs.json

	return true;
}
