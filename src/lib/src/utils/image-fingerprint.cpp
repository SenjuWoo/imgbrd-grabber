#include "utils/image-fingerprint.h"
#include <QtAlgorithms>
#include <algorithm>
#include <cmath>
#include <cstdlib>

ImageFingerprint ImageFingerprint::fromImage(const QImage &image, const QSize &fullSize)
{
	ImageFingerprint result;
	if (image.isNull() || image.width() < 8 || image.height() < 8) {
		return result;
	}
	const QSize reference = fullSize.isValid() && !fullSize.isEmpty() ? fullSize : image.size();
	result.aspect = double(reference.width()) / reference.height();

	const QImage gray = image.convertToFormat(QImage::Format_RGB32).scaled(17, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
	for (int row = 0; row < 16; ++row) {
		const uchar *pixels = gray.constScanLine(row);
		for (int column = 0; column < 16; ++column) {
			const int bit = row * 16 + column;
			const int difference = int(pixels[column]) - int(pixels[column + 1]);
			if (difference > 0) {
				result.structure[bit / 64] |= quint64(1) << (bit % 64);
			}
			// Flat areas flip randomly under recompression; only clear gradients are compared.
			if (std::abs(difference) >= 6) {
				result.confident[bit / 64] |= quint64(1) << (bit % 64);
			}
		}
	}

	const QImage grid = image.convertToFormat(QImage::Format_RGB32).scaled(4, 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	for (int row = 0; row < 4; ++row) {
		const auto *pixels = reinterpret_cast<const QRgb *>(grid.constScanLine(row));
		for (int column = 0; column < 4; ++column) {
			const int cell = (row * 4 + column) * 3;
			result.colors[cell] = quint8(qRed(pixels[column]));
			result.colors[cell + 1] = quint8(qGreen(pixels[column]));
			result.colors[cell + 2] = quint8(qBlue(pixels[column]));
		}
	}
	// Calibrated on Danbooru previews: catches most monochrome and line art, no colored pictures.
	const QImage look = image.convertToFormat(QImage::Format_RGB32).scaled(32, 32, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	int colored = 0, chroma = 0, white = 0;
	for (int row = 0; row < 32; ++row) {
		const auto *pixels = reinterpret_cast<const QRgb *>(look.constScanLine(row));
		for (int column = 0; column < 32; ++column) {
			const int red = qRed(pixels[column]), green = qGreen(pixels[column]), blue = qBlue(pixels[column]);
			const int spread = std::max({red, green, blue}) - std::min({red, green, blue});
			colored += spread > 32 ? 1 : 0;
			chroma += spread;
			white += std::min({red, green, blue}) > 215 ? 1 : 0;
		}
	}
	result.colored = float(colored) / 1024;
	result.chroma = float(chroma) / 1024;
	result.white = float(white) / 1024;
	result.valid = true;
	return result;
}

bool ImageFingerprint::looksMonochrome() const
{
	return valid && colored < 0.01f && chroma < 6;
}

bool ImageFingerprint::looksLikeSketch() const
{
	return valid && colored < 0.10f && white > 0.5f;
}

QString ImageFingerprint::lookTag(const std::function<bool(const QString &)> &listed) const
{
	static const QStringList monochrome {"monochrome", "greyscale", "grayscale", "black_and_white"};
	static const QStringList sketch {"sketch", "rough_sketch", "lineart", "line_art", "draft", "unfinished", "wip", "work_in_progress"};
	for (const QString &tag : monochrome) {
		if (looksMonochrome() && listed(tag)) {
			return tag;
		}
	}
	for (const QString &tag : sketch) {
		if (looksLikeSketch() && listed(tag)) {
			return tag;
		}
	}
	return {};
}

int ImageFingerprint::structureDistance(const ImageFingerprint &other) const
{
	int distance = 0;
	for (int i = 0; i < 4; ++i) {
		distance += qPopulationCount((structure[i] ^ other.structure[i]) & confident[i] & other.confident[i]);
	}
	return distance;
}

namespace
{
	int sharedGradients(const ImageFingerprint &left, const ImageFingerprint &right)
	{
		int shared = 0;
		for (int i = 0; i < 4; ++i) {
			shared += qPopulationCount(left.confident[i] & right.confident[i]);
		}
		return shared;
	}
}

int ImageFingerprint::colorDistance(const ImageFingerprint &other) const
{
	int worst = 0;
	for (size_t i = 0; i < colors.size(); ++i) {
		worst = std::max(worst, std::abs(int(colors[i]) - int(other.colors[i])));
	}
	return worst;
}

bool ImageFingerprint::sameImage(const ImageFingerprint &other) const
{
	if (!valid || !other.valid || aspect <= 0 || other.aspect <= 0) {
		return false;
	}
	// Thresholds are deliberately strict: a missed duplicate is better than hiding an edit.
	// Nearly flat pictures carry too little structure to compare safely.
	const int shared = sharedGradients(*this, other);
	return std::abs(std::log(aspect / other.aspect)) < 0.03 && shared >= 24 && structureDistance(other) * 20 <= shared && colorDistance(other) <= 24;
}
