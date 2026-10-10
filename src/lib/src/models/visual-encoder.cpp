#include "models/visual-encoder.h"
#include <QBuffer>
#include "models/library-image-encoder.h"


namespace
{
	class Worker : public QObject
	{
		public:
			Worker(QString modelPath, QString runtimeDirectory, std::shared_ptr<std::atomic_bool> stopping)
				: m_modelPath(std::move(modelPath)), m_runtimeDirectory(std::move(runtimeDirectory)), m_stopping(std::move(stopping))
			{}

			// Returns an empty vector and sets the error when the picture cannot be encoded.
			QVector<float> encode(const QByteArray &bytes, QString *error)
			{
				if (m_stopping->load()) {
					*error = QStringLiteral("Cancelled");
					return {};
				}
				if (!m_encoder) {
					m_encoder = std::make_unique<LibraryImageEncoder>(m_runtimeDirectory);
					m_opened = m_encoder->open(m_modelPath, &m_openError);
				}
				if (!m_opened) {
					*error = m_openError;
					return {};
				}
				return m_encoder->encode(bytes, error);
			}

		private:
			QString m_modelPath;
			QString m_runtimeDirectory;
			std::shared_ptr<std::atomic_bool> m_stopping;
			std::unique_ptr<LibraryImageEncoder> m_encoder;
			bool m_opened = false;
			QString m_openError;
	};
}

VisualEncoder::VisualEncoder(const QString &modelPath, const QString &runtimeDirectory, QObject *parent)
	: QObject(parent), m_stopping(std::make_shared<std::atomic_bool>(false))
{
	qRegisterMetaType<QVector<float>>();
	m_worker = new Worker(modelPath, runtimeDirectory, m_stopping);
	m_worker->moveToThread(&m_thread);
	connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
	m_thread.setObjectName("VisualEncoder");
	m_thread.start(QThread::LowPriority);
}

VisualEncoder::~VisualEncoder()
{
	m_stopping->store(true);
	m_thread.quit();
	m_thread.wait();
}

int VisualEncoder::pending() const
{
	return m_pending;
}

void VisualEncoder::request(const QString &key, const QImage &image)
{
	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	// Previews are small; PNG keeps the encoder's decoding path identical to Library thumbnails.
	image.scaled(384, 384, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
	++m_pending;
	auto *worker = static_cast<Worker *>(m_worker);
	QMetaObject::invokeMethod(m_worker, [this, worker, key, bytes, stopping = m_stopping]() {
		QString error;
		const auto vector = worker->encode(bytes, &error);
		if (stopping->load()) {
			return;
		}
		QMetaObject::invokeMethod(this, [this, key, vector, error]() {
			--m_pending;
			if (vector.isEmpty()) {
				emit failed(key, error);
			} else {
				emit encoded(key, vector);
			}
		}, Qt::QueuedConnection);
	}, Qt::QueuedConnection);
}
