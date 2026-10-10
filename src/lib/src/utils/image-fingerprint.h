#ifndef IMAGE_FINGERPRINT_H
#define IMAGE_FINGERPRINT_H

#include <QImage>
#include <QSize>
#include <array>

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
	bool valid = false;

	static ImageFingerprint fromImage(const QImage &image, const QSize &fullSize = {});
	int structureDistance(const ImageFingerprint &other) const;
	int colorDistance(const ImageFingerprint &other) const;
	bool sameImage(const ImageFingerprint &other) const;
};

#endif // IMAGE_FINGERPRINT_H
