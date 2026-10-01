#include <QFile>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include "custom-network-access-manager.h"
#include "downloader/file-downloader.h"
#include "network/network-manager.h"
#include "catch.h"


QString fileMd5(const QString &path)
{
	QCryptographicHash hash(QCryptographicHash::Md5);

	QFile f(path);
	if (!f.open(QFile::ReadOnly)) {
		return QString();
	}

	hash.addData(&f);
	f.close();
	return hash.result().toHex();
}


TEST_CASE("FileDownloader")
{
	const QString successUrl = "https://raw.githubusercontent.com/Bionus/imgbrd-grabber/master/gui/resources/images/icon.png";
	const QString successMd5 = "30d4506c7747c244219b8428a5fbff7f";
	NetworkManager accessManager;

	SECTION("Success")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("../gui/resources/images/icon.png");

		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl(successUrl)));
		QString dest = "single.png";

		FileDownloader downloader(false);
		QSignalSpy spy(&downloader, SIGNAL(success()));
		REQUIRE(downloader.start(reply, dest));
		REQUIRE(spy.wait());

		REQUIRE(fileMd5(dest) == successMd5);
		QFile::remove(dest);
	}

	SECTION("NetworkError")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("404");

		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testNetworkError")));
		QString dest = "single.png";

		FileDownloader downloader(false);
		qRegisterMetaType<NetworkReply::NetworkError>("NetworkReply::NetworkError");
		QSignalSpy spy(&downloader, SIGNAL(networkError(NetworkReply::NetworkError, QString)));
		REQUIRE(downloader.start(reply, dest));
		REQUIRE(spy.wait());

		QList<QVariant> arguments = spy.takeFirst();
		auto code = arguments[0].value<NetworkReply::NetworkError>();

		REQUIRE(code == NetworkReply::NetworkError::ContentNotFoundError);
		REQUIRE(!QFile::exists(dest));
	}

	SECTION("Large response is fully written and flushed")
	{
		QTemporaryFile input;
		REQUIRE(input.open());
		const QByteArray data(600 * 1024 + 17, 'x');
		REQUIRE(input.write(data) == data.size());
		input.close();
		CustomNetworkAccessManager::NextFiles.enqueue(input.fileName());
		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testLargeResponse")));
		const QString dest = "large-response.bin";
		FileDownloader downloader(false);
		QSignalSpy success(&downloader, SIGNAL(success()));
		QSignalSpy failure(&downloader, SIGNAL(writeError()));
		REQUIRE(downloader.start(reply, dest));
		REQUIRE(success.wait());
		REQUIRE(failure.isEmpty());
		QFile written(dest);
		REQUIRE(written.open(QFile::ReadOnly));
		REQUIRE(written.readAll() == data);
		written.close();
		REQUIRE(QFile::remove(dest));
	}

	#ifdef Q_OS_UNIX
		SECTION("Disk write failure cannot report success")
		{
			if (QFile::exists("/dev/full")) {
				CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/image_1x1.png");
				NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testWriteFailure")));
				FileDownloader downloader(false);
				QSignalSpy failure(&downloader, SIGNAL(writeError()));
				QSignalSpy success(&downloader, SIGNAL(success()));
				QTemporaryDir directory;
				REQUIRE(directory.isValid());
				const QString destination = directory.filePath("full-device");
				REQUIRE(QFile::link("/dev/full", destination));
				REQUIRE(downloader.start(reply, destination));
				REQUIRE(failure.wait());
				REQUIRE(success.isEmpty());
			}
		}
	#endif

	SECTION("FailedStart")
	{
		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testFailedStart")));
		QString dest = "////////";

		FileDownloader downloader(false);
		REQUIRE(!downloader.start(reply, dest));

		accessManager.clear();
	}

	SECTION("Large HTML response is rejected after its first buffer was streamed")
	{
		QTemporaryFile input;
		REQUIRE(input.open());
		const QByteArray data = "<!DOCTYPE html>" + QByteArray(600 * 1024, 'x');
		REQUIRE(input.write(data) == data.size());
		input.close();
		CustomNetworkAccessManager::NextFiles.enqueue(input.fileName());
		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testLargeHtml")));
		const QString dest = "large-html.bin";
		FileDownloader downloader(false);
		qRegisterMetaType<NetworkReply::NetworkError>("NetworkReply::NetworkError");
		QSignalSpy failure(&downloader, SIGNAL(networkError(NetworkReply::NetworkError, QString)));
		QSignalSpy success(&downloader, SIGNAL(success()));
		REQUIRE(downloader.start(reply, dest));
		REQUIRE(failure.wait());
		REQUIRE(failure.takeFirst()[0].value<NetworkReply::NetworkError>() == NetworkReply::NetworkError::ContentNotFoundError);
		REQUIRE(success.isEmpty());
		REQUIRE(!QFile::exists(dest));
	}

	SECTION("InvalidHtml")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/homepage.html");

		NetworkReply *reply = accessManager.get(QNetworkRequest(QUrl("testInvalidHtml")));
		QString dest = "test.html";

		FileDownloader downloader(false);
		qRegisterMetaType<NetworkReply::NetworkError>("NetworkReply::NetworkError");
		QSignalSpy spy(&downloader, SIGNAL(networkError(NetworkReply::NetworkError, QString)));
		REQUIRE(downloader.start(reply, dest));
		REQUIRE(spy.wait());

		QList<QVariant> arguments = spy.takeFirst();
		auto code = arguments[0].value<NetworkReply::NetworkError>();
		QString error = arguments[1].toString();

		REQUIRE(code == NetworkReply::NetworkError::ContentNotFoundError);
		REQUIRE(error == QString("Invalid HTML content returned"));
		REQUIRE(!QFile::exists(dest));
	}
}
