#ifndef LIBRARY_IMAGE_ENCODER_H
#define LIBRARY_IMAGE_ENCODER_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>

class LibraryImageEncoder
{
	public:
		static QString modelId();
		static QString modelUrl();
		static QString modelSha256();
		static qint64 modelSize();
		static QString modelPath(const QString &profileDirectory);
		static QVector<float> preprocess(const QByteArray &thumbnail, QString *error = nullptr);

		explicit LibraryImageEncoder(const QString &runtimeDirectory = {});
		~LibraryImageEncoder();
		bool open(const QString &modelPath, QString *error = nullptr);
		QVector<float> encode(const QByteArray &thumbnail, QString *error = nullptr);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
};

#endif // LIBRARY_IMAGE_ENCODER_H
