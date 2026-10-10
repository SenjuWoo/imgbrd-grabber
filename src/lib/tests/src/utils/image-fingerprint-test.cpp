#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <cmath>
#include "utils/image-fingerprint.h"
#include "catch.h"

namespace
{
	QImage scene(int width, int height, QColor sky = QColor(70, 120, 200))
	{
		QImage image(width, height, QImage::Format_RGB32);
		QPainter painter(&image);
		painter.fillRect(image.rect(), sky);
		painter.setBrush(QColor(40, 140, 60));
		painter.setPen(Qt::NoPen);
		painter.drawEllipse(QRectF(width * 0.1, height * 0.5, width * 0.5, height * 0.6));
		painter.setBrush(QColor(240, 220, 120));
		painter.drawEllipse(QRectF(width * 0.65, height * 0.1, width * 0.2, width * 0.2));
		painter.fillRect(QRectF(width * 0.55, height * 0.55, width * 0.3, height * 0.35), QColor(120, 60, 40));
		return image;
	}

	QImage jpeg(const QImage &image, int quality)
	{
		QByteArray bytes;
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "JPG", quality);
		return QImage::fromData(bytes, "JPG");
	}
}

TEST_CASE("Fingerprints match resized and recompressed copies of one picture", "[merge]")
{
	const auto original = ImageFingerprint::fromImage(scene(300, 420));
	const auto thumbnail = ImageFingerprint::fromImage(jpeg(scene(300, 420).scaled(150, 210, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 70));
	REQUIRE(original.valid);
	CAPTURE(original.structureDistance(thumbnail), original.colorDistance(thumbnail), original.aspect, thumbnail.aspect);
	REQUIRE(original.sameImage(thumbnail));
	REQUIRE(thumbnail.sameImage(original));
}

TEST_CASE("Fingerprints keep recolored, cropped and different pictures separate", "[merge]")
{
	const auto original = ImageFingerprint::fromImage(scene(300, 420));
	REQUIRE_FALSE(original.sameImage(ImageFingerprint::fromImage(scene(300, 420, QColor(200, 80, 140)))));
	REQUIRE_FALSE(original.sameImage(ImageFingerprint::fromImage(scene(420, 300))));
	REQUIRE_FALSE(original.sameImage(ImageFingerprint::fromImage(scene(300, 420).copy(0, 0, 300, 300))));
	QImage edited = scene(300, 420);
	QPainter painter(&edited);
	painter.fillRect(QRect(20, 20, 140, 120), Qt::black);
	painter.end();
	REQUIRE_FALSE(original.sameImage(ImageFingerprint::fromImage(edited)));
	REQUIRE_FALSE(original.sameImage(ImageFingerprint()));
	REQUIRE_FALSE(ImageFingerprint::fromImage(QImage()).valid);
}

TEST_CASE("Fingerprints use the full picture size when previews are padded", "[merge]")
{
	const auto preview = ImageFingerprint::fromImage(scene(150, 210), QSize(1500, 2100));
	REQUIRE(std::abs(preview.aspect - 1500.0 / 2100.0) < 1e-9);
	REQUIRE_FALSE(preview.sameImage(ImageFingerprint::fromImage(scene(150, 210), QSize(2100, 1500))));
}
