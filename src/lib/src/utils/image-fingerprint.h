#ifndef IMAGE_FINGERPRINT_H
#define IMAGE_FINGERPRINT_H

#include <QImage>
#include <QSize>
#include <array>
#include <functional>
#include <QString>

/**
 * Compact visual signature used to recognize the same picture served by different sources.
 * It combines a 256-bit gradient hash with a coarse colour grid and the aspect ratio, so resized or
 * recompressed copies match while recoloured, cropped or edited variants stay separate.
 */
struct ImageFingerprint
{
	std::array<quint64, 4> structure {};
	std::array<quint64, 4> confident {};
	std::array<quint8, 48> colors {};
	double aspect = 0;
	// Share of clearly colored pixels, mean colorfulness (0-255) and share of near-white pixels.
	float colored = 0;
	float chroma = 0;
	float white = 0;
	bool valid = false;

	static ImageFingerprint fromImage(const QImage &image, const QSize &fullSize = {});
	int structureDistance(const ImageFingerprint &other) const;
	int colorDistance(const ImageFingerprint &other) const;
	bool sameImage(const ImageFingerprint &other) const;
	/** Black and white or grey pictures, which uploaders often forget to tag as monochrome. */
	bool looksMonochrome() const;
	/** Mostly blank, barely colored pictures: sketches and line art. */
	bool looksLikeSketch() const;
	/** The first tag the picture visibly carries for which `listed` is true, such as a blacklisted "monochrome". */
	QString lookTag(const std::function<bool(const QString &)> &listed) const;
};

#endif // IMAGE_FINGERPRINT_H
