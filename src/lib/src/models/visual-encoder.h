#ifndef VISUAL_ENCODER_H
#define VISUAL_ENCODER_H

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>
#include <memory>


/**
 * Encodes previews with the local visual model on a background thread, one at a time.
 * Requests are dropped once the owner is destroyed, so closing never waits for a long queue.
 */
class VisualEncoder : public QObject
{
	Q_OBJECT

	public:
		explicit VisualEncoder(const QString &modelPath, const QString &runtimeDirectory = {}, QObject *parent = nullptr);
		~VisualEncoder() override;
		void request(const QString &key, const QImage &image);
		int pending() const;

	signals:
		void encoded(const QString &key, const QVector<float> &vector);
		void failed(const QString &key, const QString &error);

	private:
		QThread m_thread;
		QObject *m_worker;
		std::shared_ptr<std::atomic_bool> m_stopping;
		int m_pending = 0;
};

#endif // VISUAL_ENCODER_H
