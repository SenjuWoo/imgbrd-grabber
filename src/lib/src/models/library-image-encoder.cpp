#include "library-image-encoder.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLibrary>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include "vendor/onnxruntime/onnxruntime_c_api.h"

namespace
{
	constexpr int ImageSize = 224;
	constexpr int EmbeddingSize = 512;
	constexpr int CoefficientBits = 22;
	constexpr float Mean[] = {0.48145466f, 0.4578275f, 0.40821073f};
	constexpr float Deviation[] = {0.26862954f, 0.26130258f, 0.27577711f};

	void fail(QString *error, const QString &message)
	{
		if (error) {
			*error = message;
		}
	}

	double cubic(double value)
	{
		value = std::abs(value);
		if (value < 1.0) {
			return ((1.5 * value - 2.5) * value) * value + 1.0;
		}
		return value < 2.0 ? ((-0.5 * value + 2.5) * value - 4.0) * value + 2.0 : 0.0;
	}

	struct Filter
	{
		int start;
		QVector<qint32> weights;
	};

	QVector<Filter> filters(int sourceSize, int destinationSize, int cropStart)
	{
		// Match Pillow's bicubic antialiasing and 22-bit byte-rounding convention.
		const double scale = double(sourceSize) / destinationSize;
		const double filterScale = std::max(1.0, scale);
		const double support = 2.0 * filterScale;
		QVector<Filter> result;
		result.reserve(ImageSize);
		for (int offset = 0; offset < ImageSize; ++offset) {
			const double center = (double(cropStart) + offset + 0.5) * scale;
			const int start = std::max(0, int(center - support + 0.5));
			const int end = std::min(sourceSize, int(center + support + 0.5));
			QVector<double> coefficients;
			double total = 0.0;
			for (int pixel = start; pixel < end; ++pixel) {
				const double weight = cubic((pixel - center + 0.5) / filterScale);
				coefficients.append(weight);
				total += weight;
			}
			Filter filter{start, {}};
			filter.weights.reserve(coefficients.size());
			for (const double coefficient : coefficients) {
				const double weight = coefficient / total * (1 << CoefficientBits);
				filter.weights.append(qint32(weight + (weight < 0.0 ? -0.5 : 0.5)));
			}
			result.append(std::move(filter));
		}
		return result;
	}

	quint8 filteredByte(qint64 sum)
	{
		return quint8(std::clamp<qint64>(sum >> CoefficientBits, 0, 255));
	}
}

struct LibraryImageEncoder::Impl
{
	QLibrary runtime;
	const OrtApi *api = nullptr;
	OrtEnv *environment = nullptr;
	OrtSession *session = nullptr;
	OrtMemoryInfo *memory = nullptr;

	explicit Impl(const QString &directory)
	{
		#if defined(Q_OS_WIN)
			const QString name = QStringLiteral("onnxruntime.dll");
		#elif defined(Q_OS_MACOS)
			const QString name = QStringLiteral("libonnxruntime.dylib");
		#else
			const QString name = QStringLiteral("libonnxruntime.so");
		#endif
		runtime.setFileName(QDir(directory.isEmpty() ? QCoreApplication::applicationDirPath() : directory).absoluteFilePath(name));
		runtime.setLoadHints(QLibrary::ResolveAllSymbolsHint);
	}

	~Impl()
	{
		close();
	}

	void close()
	{
		if (api) {
			if (session) {
				api->ReleaseSession(session);
			}
			if (memory) {
				api->ReleaseMemoryInfo(memory);
			}
			if (environment) {
				api->ReleaseEnv(environment);
			}
		}
		session = nullptr;
		memory = nullptr;
		environment = nullptr;
	}

	bool check(OrtStatus *status, QString *error) const
	{
		if (!status) {
			return true;
		}
		fail(error, QString::fromUtf8(api->GetErrorMessage(status)));
		api->ReleaseStatus(status);
		return false;
	}

	bool validate(bool input, QString *error) const
	{
		OrtAllocator *allocator = nullptr;
		OrtTypeInfo *type = nullptr;
		char *name = nullptr;
		auto cleanup = qScopeGuard([&] {
			if (name && allocator) {
				allocator->Free(allocator, name);
			}
			if (type) {
				api->ReleaseTypeInfo(type);
			}
		});
		size_t count = 0;
		if (!check(input ? api->SessionGetInputCount(session, &count) : api->SessionGetOutputCount(session, &count), error)
			|| !check(api->GetAllocatorWithDefaultOptions(&allocator), error)) {
			return false;
		}
		if (count != 1) {
			fail(error, QStringLiteral("The local AI model has an unexpected tensor count."));
			return false;
		}
		if (!check(input ? api->SessionGetInputName(session, 0, allocator, &name) : api->SessionGetOutputName(session, 0, allocator, &name), error)
			|| !check(input ? api->SessionGetInputTypeInfo(session, 0, &type) : api->SessionGetOutputTypeInfo(session, 0, &type), error)) {
			return false;
		}
		const OrtTensorTypeAndShapeInfo *tensor = nullptr;
		ONNXTensorElementDataType elementType;
		size_t dimensions = 0;
		if (!check(api->CastTypeInfoToTensorInfo(type, &tensor), error) || !tensor
			|| !check(api->GetTensorElementType(tensor, &elementType), error)
			|| !check(api->GetDimensionsCount(tensor, &dimensions), error)) {
			if (!tensor) {
				fail(error, QStringLiteral("The local AI model tensor type is invalid."));
			}
			return false;
		}
		const size_t expectedRank = input ? 4 : 2;
		if (QByteArray(name) != (input ? "pixel_values" : "image_embeds")
			|| elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dimensions != expectedRank) {
			fail(error, QStringLiteral("The local AI model has an unexpected tensor name, type or shape."));
			return false;
		}
		std::array<int64_t, 4> shape{};
		if (!check(api->GetDimensions(tensor, shape.data(), dimensions), error)) {
			return false;
		}
		const std::array<int64_t, 4> expected = input ? std::array<int64_t, 4>{1, 3, ImageSize, ImageSize} : std::array<int64_t, 4>{1, EmbeddingSize, 0, 0};
		for (size_t index = 0; index < dimensions; ++index) {
			if (shape[index] != -1 && shape[index] != expected[index]) {
				fail(error, QStringLiteral("The local AI model has an incompatible tensor dimension."));
				return false;
			}
		}
		return true;
	}
};

QString LibraryImageEncoder::modelId()
{
	return QStringLiteral("clip-vit-base-patch32-uint8-v1");
}

QString LibraryImageEncoder::modelUrl()
{
	return QStringLiteral("https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/d15189d7028b43f1d3e65039190477f6af591c2a/onnx/vision_model_uint8.onnx");
}

QString LibraryImageEncoder::modelSha256()
{
	return QStringLiteral("b95448754a6ae56964ec80c570c80bb9863787ee85f51b664abed8c0e5f22a7a");
}

qint64 LibraryImageEncoder::modelSize()
{
	return 88648915;
}

QString LibraryImageEncoder::modelPath(const QString &profileDirectory)
{
	return QDir(profileDirectory).absoluteFilePath(QStringLiteral("models/clip-vit-base-patch32-uint8.onnx"));
}

LibraryImageEncoder::LibraryImageEncoder(const QString &runtimeDirectory)
	: m_impl(std::make_unique<Impl>(runtimeDirectory))
{}

LibraryImageEncoder::~LibraryImageEncoder() = default;

bool LibraryImageEncoder::open(const QString &path, QString *error)
{
	if (error) {
		error->clear();
	}
	m_impl->close();
	if (!QFileInfo(m_impl->runtime.fileName()).isFile()) {
		fail(error, QStringLiteral("The local AI runtime is missing from the application folder."));
		return false;
	}
	QFile model(path);
	if (!model.open(QIODevice::ReadOnly)) {
		fail(error, QStringLiteral("The local AI model could not be opened: %1").arg(model.errorString()));
		return false;
	}
	if (model.size() != modelSize()) {
		fail(error, QStringLiteral("The local AI model has an incorrect file size. Download it again."));
		return false;
	}
	// Read a bounded snapshot; the session consumes these verified bytes rather than a replaceable path.
	const QByteArray bytes = model.read(modelSize() + 1);
	if (bytes.size() != modelSize()) {
		fail(error, QStringLiteral("The local AI model changed size or could not be read completely."));
		return false;
	}
	if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != modelSha256()) {
		fail(error, QStringLiteral("The local AI model checksum does not match. Download it again."));
		return false;
	}
	if (!m_impl->runtime.load()) {
		fail(error, QStringLiteral("The local AI runtime could not be loaded: %1").arg(m_impl->runtime.errorString()));
		return false;
	}
	using GetApiBase = const OrtApiBase * (ORT_API_CALL *)();
	const auto getApiBase = reinterpret_cast<GetApiBase>(m_impl->runtime.resolve("OrtGetApiBase"));
	const OrtApiBase *base = getApiBase ? getApiBase() : nullptr;
	m_impl->api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
	if (!m_impl->api) {
		fail(error, QStringLiteral("The local AI runtime does not support ONNX Runtime API 30."));
		return false;
	}
	const auto api = m_impl->api;
	OrtSessionOptions *options = nullptr;
	auto cleanup = qScopeGuard([&] {
		if (options) {
			api->ReleaseSessionOptions(options);
		}
	});
	auto failedOpen = qScopeGuard([&] { m_impl->close(); });
	if (!m_impl->check(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "GrabberLocalAI", &m_impl->environment), error)
		|| !m_impl->check(api->DisableTelemetryEvents(m_impl->environment), error)
		|| !m_impl->check(api->CreateSessionOptions(&options), error)
		|| !m_impl->check(api->SetIntraOpNumThreads(options, std::clamp(QThread::idealThreadCount(), 1, 4)), error)
		|| !m_impl->check(api->SetInterOpNumThreads(options, 1), error)
		|| !m_impl->check(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL), error)
		|| !m_impl->check(api->CreateSessionFromArray(m_impl->environment, bytes.constData(), size_t(bytes.size()), options, &m_impl->session), error)
		|| !m_impl->validate(true, error) || !m_impl->validate(false, error)
		|| !m_impl->check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_impl->memory), error)) {
		return false;
	}
	failedOpen.dismiss();
	return true;
}

QVector<float> LibraryImageEncoder::preprocess(const QByteArray &thumbnail, QString *error)
{
	if (error) {
		error->clear();
	}
	if (thumbnail.isEmpty() || thumbnail.size() > 16 * 1024 * 1024) {
		fail(error, QStringLiteral("The cached preview is missing or too large for local AI."));
		return {};
	}
	QBuffer buffer;
	buffer.setData(thumbnail);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer);
	const QSize dimensions = reader.size();
	if (!dimensions.isValid() || dimensions.width() > 8192 || dimensions.height() > 8192
		|| qint64(dimensions.width()) * dimensions.height() > 16 * 1024 * 1024) {
		fail(error, QStringLiteral("The cached preview has invalid or oversized dimensions."));
		return {};
	}
	QImage image = reader.read();
	if (image.isNull() || image.size() != dimensions) {
		fail(error, QStringLiteral("The cached preview could not be decoded: %1").arg(reader.errorString()));
		return {};
	}
	// Straight RGBA retains RGB even for transparent pixels, like PIL convert('RGB').
	image = image.convertToFormat(QImage::Format_RGBA8888);
	if (image.isNull()) {
		fail(error, QStringLiteral("The cached preview could not be converted to RGB."));
		return {};
	}
	const int shortEdge = std::min(image.width(), image.height());
	const int resizedWidth = int(qint64(image.width()) * ImageSize / shortEdge);
	const int resizedHeight = int(qint64(image.height()) * ImageSize / shortEdge);
	const auto horizontal = filters(image.width(), resizedWidth, (resizedWidth - ImageSize) / 2);
	const auto vertical = filters(image.height(), resizedHeight, (resizedHeight - ImageSize) / 2);
	QByteArray intermediate(image.height() * ImageSize * 3, Qt::Uninitialized);
	for (int row = 0; row < image.height(); ++row) {
		const auto source = image.constScanLine(row);
		for (int column = 0; column < ImageSize; ++column) {
			const auto &filter = horizontal[column];
			for (int channel = 0; channel < 3; ++channel) {
				qint64 total = 1 << (CoefficientBits - 1);
				for (int offset = 0; offset < filter.weights.size(); ++offset) {
					total += qint64(source[(filter.start + offset) * 4 + channel]) * filter.weights[offset];
				}
				intermediate[(row * ImageSize + column) * 3 + channel] = char(filteredByte(total));
			}
		}
	}
	QVector<float> values(ImageSize * ImageSize * 3);
	for (int row = 0; row < ImageSize; ++row) {
		const auto &filter = vertical[row];
		for (int column = 0; column < ImageSize; ++column) {
			for (int channel = 0; channel < 3; ++channel) {
				qint64 total = 1 << (CoefficientBits - 1);
				for (int offset = 0; offset < filter.weights.size(); ++offset) {
					total += qint64(quint8(intermediate[((filter.start + offset) * ImageSize + column) * 3 + channel])) * filter.weights[offset];
				}
				const float rescaled = float(filteredByte(total) * (1.0 / 255.0));
				values[channel * ImageSize * ImageSize + row * ImageSize + column] = (rescaled - Mean[channel]) / Deviation[channel];
			}
		}
	}
	return values;
}

QVector<float> LibraryImageEncoder::encode(const QByteArray &thumbnail, QString *error)
{
	if (error) {
		error->clear();
	}
	if (!m_impl->session) {
		fail(error, QStringLiteral("The local AI model is not open."));
		return {};
	}
	auto pixels = preprocess(thumbnail, error);
	if (pixels.isEmpty()) {
		return {};
	}
	const auto api = m_impl->api;
	OrtValue *input = nullptr;
	OrtValue *output = nullptr;
	OrtTensorTypeAndShapeInfo *tensor = nullptr;
	auto cleanup = qScopeGuard([&] {
		if (tensor) {
			api->ReleaseTensorTypeAndShapeInfo(tensor);
		}
		if (output) {
			api->ReleaseValue(output);
		}
		if (input) {
			api->ReleaseValue(input);
		}
	});
	const int64_t inputShape[] = {1, 3, ImageSize, ImageSize};
	const char *inputNames[] = {"pixel_values"};
	const char *outputNames[] = {"image_embeds"};
	if (!m_impl->check(api->CreateTensorWithDataAsOrtValue(m_impl->memory, pixels.data(), size_t(pixels.size()) * sizeof(float), inputShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input), error)
		|| !m_impl->check(api->Run(m_impl->session, nullptr, inputNames, &input, 1, outputNames, 1, &output), error)
		|| !m_impl->check(api->GetTensorTypeAndShape(output, &tensor), error)) {
		return {};
	}
	size_t rank = 0;
	size_t elements = 0;
	ONNXTensorElementDataType type;
	if (!m_impl->check(api->GetDimensionsCount(tensor, &rank), error)
		|| !m_impl->check(api->GetTensorElementType(tensor, &type), error)
		|| !m_impl->check(api->GetTensorShapeElementCount(tensor, &elements), error)) {
		return {};
	}
	std::array<int64_t, 2> outputShape{};
	if (rank != 2 || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || elements != EmbeddingSize) {
		fail(error, QStringLiteral("The local AI runtime returned an invalid embedding tensor."));
		return {};
	}
	if (!m_impl->check(api->GetDimensions(tensor, outputShape.data(), outputShape.size()), error)) {
		return {};
	}
	void *data = nullptr;
	if (outputShape[0] != 1 || outputShape[1] != EmbeddingSize || !m_impl->check(api->GetTensorMutableData(output, &data), error) || !data) {
		fail(error, QStringLiteral("The local AI runtime returned an invalid embedding shape."));
		return {};
	}
	const auto floats = static_cast<const float *>(data);
	double squaredNorm = 0.0;
	for (int index = 0; index < EmbeddingSize; ++index) {
		if (!std::isfinite(floats[index])) {
			fail(error, QStringLiteral("The local AI runtime returned a non-finite embedding."));
			return {};
		}
		squaredNorm += double(floats[index]) * floats[index];
	}
	if (!std::isfinite(squaredNorm) || squaredNorm <= 1e-20) {
		fail(error, QStringLiteral("The local AI runtime returned an empty embedding."));
		return {};
	}
	const double norm = std::sqrt(squaredNorm);
	QVector<float> result(EmbeddingSize);
	for (int index = 0; index < EmbeddingSize; ++index) {
		result[index] = float(floats[index] / norm);
	}
	return result;
}
