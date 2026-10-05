#include <QBuffer>
#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include "models/library-image-encoder.h"
#include "catch.h"

namespace
{
	QByteArray encodedFixture(const QImage &image)
	{
		QByteArray bytes;
		QBuffer buffer(&bytes);
		REQUIRE(buffer.open(QIODevice::WriteOnly));
		REQUIRE(image.save(&buffer, "PNG"));
		return bytes;
	}

	QString runtimeName()
	{
		#if defined(Q_OS_WIN)
			return QStringLiteral("onnxruntime.dll");
		#elif defined(Q_OS_MACOS)
			return QStringLiteral("libonnxruntime.dylib");
		#else
			return QStringLiteral("libonnxruntime.so");
		#endif
	}

	QByteArray decodedPixels(const QVector<float> &values)
	{
		constexpr float mean[] = {0.48145466f, 0.4578275f, 0.40821073f};
		constexpr float deviation[] = {0.26862954f, 0.26130258f, 0.27577711f};
		QByteArray bytes(224 * 224 * 3, Qt::Uninitialized);
		for (int pixel = 0; pixel < 224 * 224; ++pixel) {
			for (int channel = 0; channel < 3; ++channel) {
				const auto value = std::lround((values[channel * 224 * 224 + pixel] * deviation[channel] + mean[channel]) * 255);
				bytes[pixel * 3 + channel] = char(std::clamp<long>(value, 0, 255));
			}
		}
		return bytes;
	}
}

TEST_CASE("Local image encoder matches independent Pillow bicubic and center crop pixels", "[library][ai][encoder]")
{
	// Independently generated using installed Pillow 12.2: RGB -> BICUBIC resize
	// (w*224/min(w,h), h*224/min(w,h)), then floor-centered 224x224 crop.
	const struct Fixture { int width; int height; const char *sha256; } fixtures[] = {
		{371, 263, "bafe7f686fc989061ec2f7ab90d41e33fd260e6f9a45821be6448116988c8b4f"},
		{263, 371, "eb9b30d659d0f38ac6d6e4845d54f202745be75719dfceba76e8fffb3fad4ae3"},
		{37, 53, "c38db662d5043adbef8a6066d03bd071e39a2ce5a6ac6c0413a0e940af570ba2"},
		{53, 37, "6e3a9f50af3518ae6228daf14b6869d3d2a2ab7f5e3453a21da730338e1f2ca4"},
		{241, 319, "7bf94d65d1a4c81da8bc96f1ddd42f767d07a1b35ac94d21c49a3b2389e36019"},
		{8192, 1, "3bb661d13abc10bbc9e30e2985693ea2ae342faafcd2bb7dbd8d3c654d732695"},
	};
	for (const auto &fixture : fixtures) {
		INFO("fixture " << fixture.width << "x" << fixture.height);
		QImage image(fixture.width, fixture.height, QImage::Format_RGB888);
		for (int row = 0; row < image.height(); ++row) {
			for (int column = 0; column < image.width(); ++column) {
				image.setPixelColor(column, row, QColor((column * 17 + row * 3) % 256, (column * 5 + row * 19) % 256, (column * column + row * 7) % 256));
			}
		}
		QString error = "stale";
		const auto values = LibraryImageEncoder::preprocess(encodedFixture(image), &error);
		REQUIRE(error.isEmpty());
		REQUIRE(values.size() == 3 * 224 * 224);
		REQUIRE(std::all_of(values.cbegin(), values.cend(), [](float value) { return std::isfinite(value); }));
		REQUIRE(QCryptographicHash::hash(decodedPixels(values), QCryptographicHash::Sha256).toHex() == fixture.sha256);
	}
}

TEST_CASE("Local image encoder preserves RGB, normalizes CHW and rejects bad previews", "[library][ai][encoder]")
{
	QImage image(17, 30, QImage::Format_RGBA8888);
	image.fill(QColor(45, 86, 120, 0));
	QString error;
	const auto values = LibraryImageEncoder::preprocess(encodedFixture(image), &error);
	REQUIRE(error.isEmpty());
	REQUIRE(values.size() == 3 * 224 * 224);
	REQUIRE(values[0] == Catch::Approx((float(45.0 / 255.0) - 0.48145466f) / 0.26862954f));
	REQUIRE(values[224 * 224] == Catch::Approx((float(86.0 / 255.0) - 0.4578275f) / 0.26130258f));
	REQUIRE(values[2 * 224 * 224] == Catch::Approx((float(120.0 / 255.0) - 0.40821073f) / 0.27577711f));
	REQUIRE(values.front() == values[224 * 224 - 1]);
	for (const auto &invalid : {QByteArray(), QByteArray("not an image"), QByteArray(16 * 1024 * 1024 + 1, 'x')}) {
		REQUIRE(LibraryImageEncoder::preprocess(invalid, &error).isEmpty());
		REQUIRE(!error.isEmpty());
	}
	QImage oversized(8193, 1, QImage::Format_RGB888);
	oversized.fill(Qt::red);
	REQUIRE(LibraryImageEncoder::preprocess(encodedFixture(oversized), &error).isEmpty());
	REQUIRE(error.contains("dimensions"));
	QByteArray truncated = encodedFixture(image);
	truncated.truncate(40);
	REQUIRE(LibraryImageEncoder::preprocess(truncated, &error).isEmpty());
	REQUIRE(!error.isEmpty());
}

TEST_CASE("Local image encoder reports missing runtime and verifies model bytes before loading", "[library][ai][encoder]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	LibraryImageEncoder encoder(directory.path());
	QString error;
	REQUIRE(!encoder.open(directory.filePath("missing.onnx"), &error));
	REQUIRE(error.contains("runtime"));
	REQUIRE(encoder.encode("invalid", &error).isEmpty());
	REQUIRE(error.contains("not open"));
	// This placeholder is never loaded: every model below fails validation first.
	QFile runtime(directory.filePath(runtimeName()));
	REQUIRE(runtime.open(QIODevice::WriteOnly));
	REQUIRE(runtime.write("not a runtime") == 13);
	runtime.close();
	const QString path = LibraryImageEncoder::modelPath(directory.path());
	REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
	REQUIRE(!encoder.open(path, &error));
	REQUIRE(error.contains("opened"));
	QFile model(path);
	REQUIRE(model.open(QIODevice::WriteOnly));
	REQUIRE(model.write("not a model") == 11);
	model.close();
	REQUIRE(!encoder.open(path, &error));
	REQUIRE(error.contains("size"));
	REQUIRE(model.open(QIODevice::WriteOnly));
	REQUIRE(model.resize(LibraryImageEncoder::modelSize()));
	model.close();
	REQUIRE(!encoder.open(path, &error));
	REQUIRE(error.contains("checksum"));
	REQUIRE(LibraryImageEncoder::modelUrl().contains("d15189d7028b43f1d3e65039190477f6af591c2a"));
	REQUIRE(LibraryImageEncoder::modelSha256().size() == 64);
	REQUIRE(LibraryImageEncoder::modelId() == "clip-vit-base-patch32-uint8-v1");
}
