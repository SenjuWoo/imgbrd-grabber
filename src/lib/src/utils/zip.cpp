#include "zip.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <limits>
#include <vendor/miniz.h>
#include "logger.h"

namespace
{
	size_t readDevice(void *opaque, mz_uint64 offset, void *buffer, size_t count)
	{
		auto *device = static_cast<QIODevice*>(opaque);
		if (offset > mz_uint64(std::numeric_limits<qint64>::max()) || count > size_t(std::numeric_limits<qint64>::max()) || !device->seek(qint64(offset))) {
			return 0;
		}
		return size_t(qMax<qint64>(0, device->read(static_cast<char*>(buffer), qint64(count))));
	}

	size_t writeDevice(void *opaque, mz_uint64 offset, const void *buffer, size_t count)
	{
		auto *device = static_cast<QIODevice*>(opaque);
		if (offset > mz_uint64(std::numeric_limits<qint64>::max()) || count > size_t(std::numeric_limits<qint64>::max()) || !device->seek(qint64(offset))) {
			return 0;
		}
		return size_t(qMax<qint64>(0, device->write(static_cast<const char*>(buffer), qint64(count))));
	}

	bool relativeName(const QString &name)
	{
		if (name.isEmpty() || name.contains(QChar(0)) || name.contains(':') || name.startsWith('/') || QDir::isAbsolutePath(name)) {
			return false;
		}
		for (const auto &part : name.split('/')) {
			if (part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) {
				return false;
			}
			const QString stem = part.section('.', 0, 0).toUpper();
			if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '1' && stem[3] <= '9')) {
				return false;
			}
		}
		return true;
	}

	bool safeTarget(const QDir &root, const QString &name)
	{
		QString path = root.absolutePath();
		for (const auto &part : name.split('/', Qt::SkipEmptyParts)) {
			path = QDir(path).filePath(part);
			if (QFileInfo(path).isSymLink()) {
				return false;
			}
		}
		const QString relative = root.relativeFilePath(QDir::cleanPath(path));
		return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
	}
}

bool createZip(const QString &filePath, const QHash<QString, QString> &files)
{
	QSaveFile output(filePath);
	output.setDirectWriteFallback(false);
	if (!output.open(QIODevice::WriteOnly)) {
		return false;
	}
	mz_zip_archive archive {};
	archive.m_pWrite = writeDevice;
	archive.m_pIO_opaque = &output;
	if (!mz_zip_writer_init(&archive, 0)) {
		return false;
	}
	auto end = qScopeGuard([&]() { mz_zip_writer_end(&archive); });
	for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
		const QString name = QString(it.value()).replace('\\', '/');
		QFile source(it.key());
		if (!relativeName(name) || !source.open(QIODevice::ReadOnly) || !QFileInfo(it.key()).isFile()
			|| !mz_zip_writer_add_read_buf_callback(&archive, name.toUtf8().constData(), readDevice, &source, mz_uint64(source.size()), nullptr, nullptr, 0, MZ_BEST_COMPRESSION, nullptr, 0, nullptr, 0)
			|| source.error() != QFileDevice::NoError) {
			log(QStringLiteral("Could not add `%1` to backup; existing archive preserved").arg(it.key()), Logger::Error);
			return false;
		}
	}
	const bool finalized = mz_zip_writer_finalize_archive(&archive);
	const bool closed = mz_zip_writer_end(&archive);
	end.dismiss();
	return finalized && closed && output.commit();
}

bool unzipFile(const QString &filePath, const QString &destinationDir)
{
	if (!QDir().mkpath(destinationDir) || QFileInfo(destinationDir).isSymLink()) {
		return false;
	}
	QDir root(QFileInfo(destinationDir).canonicalFilePath());
	QFile input(filePath);
	if (!input.open(QIODevice::ReadOnly)) {
		return false;
	}
	mz_zip_archive archive {};
	archive.m_pRead = readDevice;
	archive.m_pIO_opaque = &input;
	if (!mz_zip_reader_init(&archive, mz_uint64(input.size()), 0)) {
		return false;
	}
	auto end = qScopeGuard([&]() { mz_zip_reader_end(&archive); });
	QStringList names;
	QSet<QString> seen;
	// Validate every entry before creating any file inside the extraction tree.
	for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
		mz_zip_archive_file_stat stat;
		const mz_uint length = mz_zip_reader_get_filename(&archive, i, nullptr, 0);
		if (!length || length > 32768 || !mz_zip_reader_file_stat(&archive, i, &stat) || ((stat.m_external_attr >> 16) & 0170000) == 0120000) {
			return false;
		}
		QByteArray raw(int(length), '\0');
		if (mz_zip_reader_get_filename(&archive, i, raw.data(), length) != length) {
			return false;
		}
		raw.chop(1);
		QString name = QString::fromUtf8(raw);
		if (name.toUtf8() != raw) {
			return false;
		}
		name.replace('\\', '/');
		QString identity = QDir::cleanPath(name);
		#ifdef Q_OS_WIN
			identity = identity.toCaseFolded();
		#endif
		if (!relativeName(name) || !safeTarget(root, name) || seen.contains(identity)) {
			log(QStringLiteral("Unsafe or duplicate ZIP entry: `%1`").arg(name), Logger::Error);
			return false;
		}
		seen.insert(identity);
		names.append(name);
	}
	for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
		const QString target = root.filePath(names[int(i)]);
		if (!safeTarget(root, names[int(i)])) {
			return false;
		}
		if (mz_zip_reader_is_file_a_directory(&archive, i)) {
			if (!QDir().mkpath(target)) {
				return false;
			}
		} else {
			if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !safeTarget(root, names[int(i)])) {
				return false;
			}
			QSaveFile output(target);
			output.setDirectWriteFallback(false);
			if (!output.open(QIODevice::WriteOnly) || !mz_zip_reader_extract_to_callback(&archive, i, writeDevice, &output, 0) || !output.commit()) {
				return false;
			}
		}
	}
	const bool closed = mz_zip_reader_end(&archive);
	end.dismiss();
	return closed;
}
