#include "models/library-importer.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <functional>


namespace
{
	constexpr qint64 MetadataLimit = 256 * 1024;
	constexpr qint64 ChunkSize = 1024 * 1024;

	bool cancelled(const std::shared_ptr<std::atomic_bool> &cancel)
	{
		return cancel && cancel->load();
	}

	QString fieldName(QString key)
	{
		key = key.section(':', -1).toLower();
		key.remove('_');
		key.remove('-');
		return key;
	}

	bool md5Value(const QString &value)
	{
		static const QRegularExpression expression("^[0-9a-fA-F]{32}$");
		return expression.match(value).hasMatch();
	}

	bool isSourceField(const QString &key)
	{
		return key.endsWith("url") || key == "source" || key == "sources" || key == "identifier"
			|| key == "relation" || key == "webstatement";
	}

	bool isTagField(const QString &key)
	{
		return key == "tags" || key.endsWith("tagstring") || key == "keywords" || key == "subject";
	}

	void addUrls(LibraryImportData &data, const QString &text)
	{
		static const QRegularExpression expression("https?://[^\\s<>\"']+", QRegularExpression::CaseInsensitiveOption);
		auto matches = expression.globalMatch(text.left(MetadataLimit));
		while (matches.hasNext() && data.sourceUrls.size() < 64) {
			QString candidate = matches.next().captured();
			while (candidate.endsWith(')') || candidate.endsWith(',') || candidate.endsWith('.')) {
				candidate.chop(1);
			}
			QUrl url(candidate, QUrl::StrictMode);
			if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
				|| (url.scheme().toLower() != "https" && url.scheme().toLower() != "http")) {
				continue;
			}
			url.setFragment({});
			const QString normalized = url.toString(QUrl::FullyEncoded);
			if (normalized.size() <= 4096 && !data.sourceUrls.contains(normalized)) {
				data.sourceUrls.append(normalized);
			}
		}
	}

	void addTags(LibraryImportData &data, const QString &text)
	{
		static const QRegularExpression separator("[\\s,;]+");
		for (QString tag : text.left(16384).split(separator, Qt::SkipEmptyParts)) {
			tag = tag.trimmed();
			if (data.tags.size() >= 256) {
				break;
			}
			if (tag.size() <= 128 && !tag.contains("://") && !data.tags.contains(tag)) {
				data.tags.append(tag);
			}
		}
	}

	void readFields(LibraryImportData &data, const QJsonValue &value, const QString &key = {}, int depth = 0)
	{
		if (depth > 12) {
			return;
		}
		if (value.isObject()) {
			const QJsonObject object = value.toObject();
			for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
				readFields(data, it.value(), fieldName(it.key()), depth + 1);
			}
		} else if (value.isArray()) {
			const QJsonArray array = value.toArray();
			for (qsizetype index = 0; index < array.size() && index < 1024; ++index) {
				readFields(data, array.at(index), key, depth + 1);
			}
		} else if (value.isString()) {
			const QString text = value.toString();
			if (isSourceField(key)) {
				addUrls(data, text);
			}
			if (isTagField(key)) {
				addTags(data, text);
			}
			if ((key == "sourcemd5" || key == "originalmd5" || key == "md5") && data.sourceMd5.isEmpty() && md5Value(text)) {
				data.sourceMd5 = text.toLower();
			}
			if (key == "title" && !text.trimmed().isEmpty() && text.size() <= 512) {
				data.title = text.trimmed();
			}
		}
	}

	QJsonObject xmlFields(const QByteArray &bytes, QString &error)
	{
		QXmlStreamReader reader(bytes);
		reader.setEntityExpansionLimit(4096);
		QStringList ancestors, texts;
		QJsonObject result;
		QJsonArray tags, urls;
		while (!reader.atEnd()) {
			const auto token = reader.readNext();
			if (token == QXmlStreamReader::DTD) {
				error = "XMP document types are unsupported.";
				return {};
			}
			if (token == QXmlStreamReader::StartElement) {
				if (ancestors.size() >= 32) {
					error = "XMP nesting is too deep.";
					return {};
				}
				ancestors.append(fieldName(reader.name().toString()));
				texts.append(QString());
				for (const auto &attribute : reader.attributes()) {
					const QString name = fieldName(attribute.name().toString());
					if (isSourceField(name) || (name == "resource" && ancestors.size() > 1 && isSourceField(ancestors.at(ancestors.size() - 2)))) {
						urls.append(attribute.value().toString());
					}
				}
			} else if (token == QXmlStreamReader::Characters && !texts.isEmpty()) {
				texts.last().append(reader.text());
			} else if (token == QXmlStreamReader::EndElement && !ancestors.isEmpty()) {
				const QString name = ancestors.takeLast();
				const QString text = texts.takeLast().trimmed();
				if (isTagField(name) || (name == "li" && (ancestors.contains("subject") || ancestors.contains("keywords")))) {
					tags.append(text);
				}
				if (isSourceField(name)) {
					urls.append(text);
				}
				if (name == "sourcemd5" || name == "originalmd5" || name == "md5") {
					result.insert(name, text);
				}
				if (!text.isEmpty() && (name == "title" || (name == "li" && ancestors.contains("title")))) {
					result.insert("title", text);
				}
			}
		}
		if (reader.hasError()) {
			error = reader.errorString();
		}
		result.insert("tags", tags);
		result.insert("sources", urls);
		return result;
	}

	QByteArray readBounded(const QString &path, QString &error)
	{
		QFile file(path);
		if (!file.open(QFile::ReadOnly)) {
			error = file.errorString();
			return {};
		}
		if (file.size() > MetadataLimit) {
			error = "Metadata exceeds the 256 KiB import limit.";
			return {};
		}
		const QByteArray bytes = file.read(MetadataLimit + 1);
		if (file.error() != QFileDevice::NoError || bytes.size() > MetadataLimit) {
			error = "Unable to read bounded metadata.";
			return {};
		}
		return bytes;
	}

	void addEvidence(LibraryImportData &data, QJsonArray &evidence, const QString &kind, const QString &path, const QJsonValue &value, const QString &raw = {}, const QString &error = {})
	{
		QJsonObject item {{ "kind", kind }, { "path", path }};
		if (!value.isUndefined()) {
			item.insert("data", value);
			if (error.isEmpty()) {
				readFields(data, value);
			}
		}
		if (!raw.isEmpty()) {
			item.insert("raw", raw);
		}
		if (!error.isEmpty()) {
			item.insert("error", error);
		}
		evidence.append(item);
	}

	void readExiftool(LibraryImportData &data, QJsonArray &evidence, const std::shared_ptr<std::atomic_bool> &cancel)
	{
		QString executable = QStandardPaths::findExecutable("exiftool");
		if (executable.isEmpty()) {
			executable = QStandardPaths::findExecutable("exiftool", { QCoreApplication::applicationDirPath() });
		}
		if (executable.isEmpty() || cancelled(cancel)) {
			return;
		}
		QProcess process;
		process.setReadChannel(QProcess::StandardOutput);
		process.start(executable, { "-config", "", "-j", "-G1", "-a", "-s", "-charset", "filename=utf8", "--", data.path });
		if (!process.waitForStarted(1000)) {
			addEvidence(data, evidence, "exiftool", data.path, {}, {}, process.errorString());
			return;
		}
		QElapsedTimer timer;
		timer.start();
		QByteArray output;
		QString error;
		while (process.state() != QProcess::NotRunning) {
			process.waitForReadyRead(50);
			output.append(process.read(MetadataLimit + 1 - output.size()));
			process.readAllStandardError();
			if (output.size() > MetadataLimit || timer.elapsed() > 5000 || cancelled(cancel)) {
				error = cancelled(cancel) ? "Import cancelled." : "ExifTool output or time limit exceeded.";
				process.kill();
				process.waitForFinished(1000);
				break;
			}
		}
		output.append(process.read(qMax(qint64(0), MetadataLimit + 1 - output.size())));
		if (output.size() > MetadataLimit) {
			error = "ExifTool metadata exceeds the import limit.";
		}
		QJsonParseError parseError;
		const auto document = QJsonDocument::fromJson(output, &parseError);
		if (error.isEmpty() && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !document.isArray())) {
			error = "ExifTool did not return a metadata array.";
		}
		addEvidence(data, evidence, "exiftool", data.path, error.isEmpty() ? QJsonValue(document.array()) : QJsonValue(), {}, error);
	}

	bool unchanged(const QFileInfo &original, qint64 size, const QDateTime &modified)
	{
		const QFileInfo current(original.absoluteFilePath());
		return current.exists() && current.size() == size && current.lastModified() == modified;
	}

	bool matchesHash(const QString &path, const QString &sha256, const std::shared_ptr<std::atomic_bool> &cancel)
	{
		QFile file(path);
		if (QFileInfo(path).isSymLink() || !file.open(QFile::ReadOnly)) {
			return false;
		}
		QCryptographicHash hash(QCryptographicHash::Sha256);
		while (!file.atEnd()) {
			if (cancelled(cancel)) {
				return false;
			}
			const QByteArray bytes = file.read(ChunkSize);
			if (file.error() != QFileDevice::NoError || bytes.isEmpty()) {
				return false;
			}
			hash.addData(bytes);
		}
		return QString::fromLatin1(hash.result().toHex()) == sha256;
	}
}


QStringList LibraryImporter::imageFiles(const QStringList &roots, const std::shared_ptr<std::atomic_bool> &cancel)
{
	QSet<QString> extensions;
	for (const auto &format : QImageReader::supportedImageFormats()) {
		extensions.insert(QString::fromLatin1(format).toLower());
	}
	QStringList result;
	QSet<QString> seen;
	std::function<void(const QFileInfo &)> visit = [&](const QFileInfo &info) {
		if (cancelled(cancel) || !info.exists() || info.isSymLink()) {
			return;
		}
		const QString path = info.canonicalFilePath();
		#ifdef Q_OS_WIN
			const QString key = path.toCaseFolded();
		#else
			const QString key = path;
		#endif
		if (seen.contains(key)) {
			return;
		}
		seen.insert(key);
		if (info.isDir()) {
			for (const auto &child : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name)) {
				visit(child);
				if (cancelled(cancel)) {
					break;
				}
			}
		} else if (info.isFile() && extensions.contains(info.suffix().toLower())) {
			result.append(path);
		}
	};
	for (const QString &root : roots) {
		visit(QFileInfo(root));
		if (cancelled(cancel)) {
			break;
		}
	}
	return result;
}


LibraryImportData LibraryImporter::inspect(const QString &path, const QString &managedDirectory, const std::shared_ptr<std::atomic_bool> &cancel)
{
	LibraryImportData data;
	const QFileInfo original(path);
	data.path = original.absoluteFilePath();
	data.title = original.completeBaseName();
	const qint64 originalSize = original.size();
	const QDateTime originalModified = original.lastModified();
	if (cancelled(cancel)) {
		data.error = "Import cancelled.";
		return data;
	}
	QFile file(data.path);
	if (!original.isFile() || !file.open(QFile::ReadOnly)) {
		data.error = "Cannot read image: " + (original.isFile() ? file.errorString() : QString("file is missing or not a regular file."));
		return data;
	}
	QCryptographicHash sha256(QCryptographicHash::Sha256), md5(QCryptographicHash::Md5);
	while (!file.atEnd()) {
		const QByteArray bytes = file.read(ChunkSize);
		if (file.error() != QFileDevice::NoError || bytes.isEmpty() || cancelled(cancel)) {
			data.error = cancelled(cancel) ? "Import cancelled." : "Cannot read image bytes.";
			return data;
		}
		sha256.addData(bytes);
		md5.addData(bytes);
	}
	data.sha256 = QString::fromLatin1(sha256.result().toHex());
	data.md5 = QString::fromLatin1(md5.result().toHex());
	QImageReader reader(data.path);
	reader.setAutoTransform(true);
	data.size = reader.size();
	// ponytail: bound decoded-image allocation; tiled decoders are needed for images above 40 million pixels.
	if (!data.size.isValid() || qint64(data.size.width()) * data.size.height() > 40000000 || data.size.width() > 32768 || data.size.height() > 32768) {
		data.error = "Image dimensions are invalid or exceed the 40 million pixel preview limit.";
		return data;
	}
	if (data.size.width() > 512 || data.size.height() > 512) {
		reader.setScaledSize(data.size.scaled(QSize(512, 512), Qt::KeepAspectRatio));
	}
	if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90)) {
		data.size.transpose();
	}
	// Some image plugins clear their detected format after read().
	const QByteArray format = reader.format();
	QJsonArray evidence;
	QJsonObject text;
	qint64 textLength = 0;
	for (const QString &key : reader.textKeys()) {
		const QString value = reader.text(key);
		textLength += value.size();
		if (textLength > MetadataLimit || text.size() >= 256) {
			break;
		}
		text.insert(key, value);
	}
	if (!text.isEmpty()) {
		addEvidence(data, evidence, "embedded-text", data.path, text);
	}
	QImage image = reader.read();
	if (image.isNull()) {
		data.error = "Cannot decode image: " + reader.errorString();
		return data;
	}
	if (image.width() > 512 || image.height() > 512) {
		image = image.scaled(QSize(512, 512), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	data.visualHash = visualHash(image);
	QBuffer buffer(&data.thumbnail);
	buffer.open(QIODevice::WriteOnly);
	if (!image.save(&buffer, "PNG")) {
		data.error = "Cannot create image preview.";
		return data;
	}
	if (md5Value(original.completeBaseName())) {
		data.sourceMd5 = original.completeBaseName().toLower();
		addEvidence(data, evidence, "filename-md5", data.path, QJsonObject {{ "source_md5", data.sourceMd5 }});
	}
	QSet<QString> sidecars;
	for (const QString &suffix : { QString("json"), QString("xmp"), QString("txt") }) {
		for (const QString &sidecar : { data.path + "." + suffix, original.dir().filePath(original.completeBaseName() + "." + suffix) }) {
			if (sidecars.contains(sidecar) || !QFileInfo(sidecar).isFile()) {
				continue;
			}
			sidecars.insert(sidecar);
			if (cancelled(cancel)) {
				data.error = "Import cancelled.";
				return data;
			}
			QString error;
			const QByteArray raw = readBounded(sidecar, error);
			QJsonValue values;
			if (error.isEmpty() && suffix == "json") {
				QJsonParseError parseError;
				const auto document = QJsonDocument::fromJson(raw, &parseError);
				if (parseError.error != QJsonParseError::NoError) {
					error = parseError.errorString();
				} else {
					values = document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array());
				}
			} else if (error.isEmpty() && suffix == "xmp") {
				values = xmlFields(raw, error);
			} else if (error.isEmpty()) {
				values = QJsonObject {{ "tags", QString::fromUtf8(raw) }};
				addUrls(data, QString::fromUtf8(raw));
			}
			addEvidence(data, evidence, "sidecar-" + suffix, sidecar, values, suffix == "json" && error.isEmpty() ? QString() : QString::fromUtf8(raw), error);
		}
	}
	#ifdef Q_OS_WIN
		QString zoneError;
		const QByteArray zone = readBounded(data.path + ":Zone.Identifier", zoneError);
		if (zoneError.isEmpty() && !zone.isEmpty()) {
			QJsonObject zoneFields;
			for (const QByteArray &line : zone.split('\n')) {
				const int split = line.indexOf('=');
				if (split > 0) {
					zoneFields.insert(QString::fromUtf8(line.left(split)).trimmed(), QString::fromUtf8(line.mid(split + 1)).trimmed());
				}
			}
			addEvidence(data, evidence, "download-origin", data.path, zoneFields);
		}
	#endif
	readExiftool(data, evidence, cancel);
	data.metadata = {{ "version", 1 }, { "evidence", evidence }};
	if (cancelled(cancel) || !unchanged(original, originalSize, originalModified)) {
		data.error = cancelled(cancel) ? "Import cancelled." : "Image changed during import; retry it.";
		return data;
	}
	if (managedDirectory.isEmpty()) {
		return data;
	}
	if (format.isEmpty()) {
		data.error = "Cannot determine the image format for a managed copy.";
		return data;
	}
	QDir directory(managedDirectory);
	if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
		data.error = "Cannot create the managed image directory.";
		return data;
	}
	const QString destination = directory.absoluteFilePath(data.sha256 + "." + QString::fromLatin1(format).toLower());
	if (QFileInfo::exists(destination)) {
		if (!matchesHash(destination, data.sha256, cancel)) {
			data.error = cancelled(cancel) ? "Import cancelled." : "An unmatched file already occupies the managed image path.";
			return data;
		}
	} else {
		const QString stagedPath = destination + ".import-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
		QSaveFile copy(stagedPath);
		copy.setDirectWriteFallback(false);
		if (!file.seek(0) || !copy.open(QIODevice::WriteOnly)) {
			data.error = "Cannot prepare the managed image copy.";
			return data;
		}
		QCryptographicHash copiedHash(QCryptographicHash::Sha256);
		while (!file.atEnd()) {
			const QByteArray bytes = file.read(ChunkSize);
			if (cancelled(cancel) || file.error() != QFileDevice::NoError || bytes.isEmpty() || copy.write(bytes) != bytes.size()) {
				copy.cancelWriting();
				data.error = cancelled(cancel) ? "Import cancelled." : "Cannot copy image bytes.";
				return data;
			}
			copiedHash.addData(bytes);
		}
		if (cancelled(cancel) || !unchanged(original, originalSize, originalModified) || QString::fromLatin1(copiedHash.result().toHex()) != data.sha256) {
			copy.cancelWriting();
			data.error = cancelled(cancel) ? "Import cancelled." : "Image changed while being copied; retry it.";
			return data;
		}
		if (!copy.commit()) {
			data.error = "Cannot commit the managed image copy.";
			return data;
		}
		if (!QFile::rename(stagedPath, destination)) {
			const bool duplicate = matchesHash(destination, data.sha256, cancel);
			QFile::remove(stagedPath);
			if (!duplicate) {
				data.error = "Cannot install the managed image copy without replacing an existing file.";
				return data;
			}
		}
	}
	data.copied = QFileInfo(destination).absoluteFilePath() != data.path;
	data.path = destination;
	return data;
}


QString LibraryImporter::visualHash(const QImage &image)
{
	if (image.isNull()) {
		return {};
	}
	const QImage small = image.scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
	quint64 hash = 0;
	for (int row = 0; row < 8; ++row) {
		const uchar *pixels = small.constScanLine(row);
		for (int column = 0; column < 8; ++column) {
			hash = (hash << 1) | (pixels[column] > pixels[column + 1] ? 1 : 0);
		}
	}
	return QString::number(hash, 16).rightJustified(16, '0');
}


int LibraryImporter::visualDistance(const QString &left, const QString &right)
{
	static const QRegularExpression expression("^[0-9a-fA-F]{16}$");
	if (!expression.match(left).hasMatch() || !expression.match(right).hasMatch()) {
		return -1;
	}
	quint64 different = left.toULongLong(nullptr, 16) ^ right.toULongLong(nullptr, 16);
	int distance = 0;
	while (different != 0) {
		different &= different - 1;
		++distance;
	}
	return distance;
}
