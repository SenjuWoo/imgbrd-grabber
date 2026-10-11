#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <QElapsedTimer>
#include <iostream>
#include "models/library-importer.h"
#include "models/library-store.h"
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

TEST_CASE("Fingerprints recognize variants of one picture without merging different ones", "[merge]")
{
	const auto original = ImageFingerprint::fromImage(scene(300, 420));
	REQUIRE(original.similarImage(ImageFingerprint::fromImage(jpeg(scene(300, 420).scaled(150, 210, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 70))));
	QImage marked = scene(300, 420);
	QPainter painter(&marked);
	painter.fillRect(QRect(200, 380, 60, 24), Qt::white); // A signature or caption.
	painter.end();
	const auto variant = ImageFingerprint::fromImage(marked);
	CAPTURE(original.structureDistance(variant), original.colorDistance(variant));
	REQUIRE(original.similarImage(variant));
	REQUIRE_FALSE(original.similarImage(ImageFingerprint::fromImage(scene(420, 300))));
	REQUIRE_FALSE(original.similarImage(ImageFingerprint::fromImage(scene(300, 420).copy(0, 0, 300, 300))));
	REQUIRE_FALSE(original.similarImage(ImageFingerprint::fromImage(scene(300, 420).mirrored(true, false))));
	REQUIRE_FALSE(original.similarImage(ImageFingerprint()));
}

TEST_CASE("Fingerprints use the full picture size when previews are padded", "[merge]")
{
	const auto preview = ImageFingerprint::fromImage(scene(150, 210), QSize(1500, 2100));
	REQUIRE(std::abs(preview.aspect - 1500.0 / 2100.0) < 1e-9);
	REQUIRE_FALSE(preview.sameImage(ImageFingerprint::fromImage(scene(150, 210), QSize(2100, 1500))));
}

// Developer aid: GRABBER_DIAG_PROFILE=<copy of a profile folder> lib-tests "[fpdiag]" compares Library pictures that look alike.
TEST_CASE("Fingerprint diagnostics for a profile copy", "[.][fpdiag]")
{
	const QString directory = qEnvironmentVariable("GRABBER_DIAG_PROFILE");
	REQUIRE(!directory.isEmpty());
	LibraryStore store(directory + "/library.sqlite");
	QElapsedTimer timer;
	timer.start();
	const auto entries = store.entries();
	const qint64 loaded = timer.restart();
	QList<ImageFingerprint> prints;
	for (const auto &entry : entries) {
		prints.append(ImageFingerprint::fromImage(QImage::fromData(entry.thumbnail)));
	}
	std::cout << entries.size() << " entries, load " << loaded << " ms, fingerprints " << timer.elapsed() << " ms\n";
	for (int i = 0; i < entries.size(); ++i) {
		for (int j = i + 1; j < entries.size(); ++j) {
			const int hash = LibraryImporter::visualDistance(entries[i].image.value("visual_hash").toString(), entries[j].image.value("visual_hash").toString());
			const bool md5 = entries[i].image.value("md5").toString() == entries[j].image.value("md5").toString();
			int shared = 0;
			for (int k = 0; k < 4; ++k) {
				shared += qPopulationCount(prints[i].confident[k] & prints[j].confident[k]);
			}
			const double ratio = shared > 0 ? double(prints[i].structureDistance(prints[j])) / shared : 1;
			if (hash < 0 || (hash > 10 && !md5 && ratio > 0.2)) {
				continue;
			}
			std::cout	<< (prints[i].sameImage(prints[j]) ? "SAME " : "     ") << (md5 ? "md5 " : "    ") << "dhash " << hash << " struct " << prints[i].structureDistance(prints[j]) << "/" << shared
						<< " color " << prints[i].colorDistance(prints[j]) << " aspect " << std::abs(std::log(prints[i].aspect / prints[j].aspect)) << "  "
						<< entries[i].image.value("website").toString().toStdString() << " " << entries[i].image.value("id").toString().toStdString() << " | "
						<< entries[j].image.value("website").toString().toStdString() << " " << entries[j].image.value("id").toString().toStdString() << "\n";
		}
	}
}
