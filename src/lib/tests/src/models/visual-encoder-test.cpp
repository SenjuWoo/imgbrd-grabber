#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <cmath>
#include "models/library-image-encoder.h"
#include "models/visual-encoder.h"
#include "catch.h"

TEST_CASE("Background visual encoder reports failures without a model and stops quickly", "[ai]")
{
	VisualEncoder encoder("missing-model.onnx", QCoreApplication::applicationDirPath());
	QSignalSpy failed(&encoder, &VisualEncoder::failed);
	QImage image(64, 64, QImage::Format_RGB32);
	image.fill(Qt::red);
	encoder.request("a", image);
	REQUIRE(encoder.pending() == 1);
	REQUIRE(failed.wait(10000));
	REQUIRE(failed.first().at(0).toString() == "a");
	REQUIRE(encoder.pending() == 0);
}

TEST_CASE("Background visual encoder embeds pictures with the real local model", "[ai][model]")
{
	// Opt-in: needs the 89 MB model and the ONNX runtime, e.g. from a portable install.
	const QString model = qEnvironmentVariable("GRABBER_TEST_AI_MODEL");
	const QString runtime = qEnvironmentVariable("GRABBER_TEST_AI_RUNTIME");
	if (model.isEmpty() || !QFileInfo(model).isFile()) {
		SUCCEED("GRABBER_TEST_AI_MODEL not set");
		return;
	}
	VisualEncoder encoder(model, runtime);
	QSignalSpy encoded(&encoder, &VisualEncoder::encoded);
	QSignalSpy failed(&encoder, &VisualEncoder::failed);
	QImage red(160, 200, QImage::Format_RGB32), blue(160, 200, QImage::Format_RGB32);
	red.fill(QColor(220, 40, 40));
	blue.fill(QColor(30, 60, 220));
	{
		QPainter painter(&red);
		painter.setBrush(Qt::white);
		painter.drawEllipse(QRect(40, 50, 80, 100));
		QPainter other(&blue);
		other.setBrush(Qt::yellow);
		other.drawRect(QRect(10, 20, 60, 160));
	}
	const QImage redder = red.scaled(80, 100, Qt::IgnoreAspectRatio, Qt::SmoothTransformation); // Same picture, smaller copy.
	encoder.request("red", red);
	encoder.request("redder", redder);
	encoder.request("blue", blue);
	for (int i = 0; i < 300 && encoded.count() + failed.count() < 3; ++i) {
		encoded.wait(200);
	}
	INFO((failed.isEmpty() ? QString() : failed.first().at(1).toString()).toStdString());
	REQUIRE(failed.isEmpty());
	REQUIRE(encoded.count() == 3);
	QHash<QString, QVector<float>> vectors;
	for (const auto &call : encoded) {
		vectors.insert(call.at(0).toString(), call.at(1).value<QVector<float>>());
	}
	auto cosine = [](const QVector<float> &a, const QVector<float> &b) {
		double dot = 0;
		for (int i = 0; i < a.size(); ++i) {
			dot += double(a[i]) * b[i];
		}
		return dot;
	};
	REQUIRE(vectors["red"].size() == 512);
	REQUIRE(cosine(vectors["red"], vectors["redder"]) > 0.9);
	REQUIRE(cosine(vectors["red"], vectors["redder"]) > cosine(vectors["red"], vectors["blue"]));
}
