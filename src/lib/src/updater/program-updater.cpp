#include "updater/program-updater.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <utility>
#include "logger.h"
#include "network/network-manager.h"
#include "network/network-reply.h"


ProgramUpdater::ProgramUpdater()
	: ProgramUpdater(QStringLiteral("https://api.github.com/repos/SenjuWoo/imgbrd-grabber"))
{}

ProgramUpdater::ProgramUpdater(QString baseUrl)
	: m_baseUrl(std::move(baseUrl)), m_downloadReply(nullptr)
{
	#ifdef NIGHTLY
		m_nightly = true;
		m_commit = QString(NIGHTLY_COMMIT);
	#endif
}

void ProgramUpdater::setNightly(bool nightly, const QString &commit)
{
	m_nightly = nightly;
	m_commit = commit;
}

void ProgramUpdater::checkForUpdates() const
{
	request(m_nightly ? QStringLiteral("/releases/tags/nightly") : QStringLiteral("/releases/latest"));
}

void ProgramUpdater::request(const QString &path) const
{
	const QUrl url(m_baseUrl + path);
	QNetworkRequest request(url);
	request.setTransferTimeout(30000);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

	auto *reply = m_networkAccessManager->get(request);
	connect(reply, &NetworkReply::finished, this, &ProgramUpdater::checkForUpdatesDone);
}

void ProgramUpdater::checkForUpdatesDone()
{
	auto *reply = dynamic_cast<NetworkReply*>(sender());
	// A repository that publishes tagged releases only has no "nightly" release and answers 404. A build
	// made from a branch then compares itself with the latest tagged release, instead of failing every
	// check and never hearing about an update.
	if (m_nightly && reply->error() == NetworkReply::NetworkError::ContentNotFoundError && reply->url().path().endsWith(QStringLiteral("/releases/tags/nightly"))) {
		reply->deleteLater();
		request(QStringLiteral("/releases/latest"));
		return;
	}
	const QByteArray response = reply->readAll();
	QJsonParseError parseError;
	const QJsonDocument json = QJsonDocument::fromJson(response, &parseError);
	const QJsonObject lastRelease = json.object();
	const QUrl releaseUrl(lastRelease["html_url"].toString());
	if (reply->error() != NetworkReply::NetworkError::NoError || parseError.error != QJsonParseError::NoError
		|| lastRelease["tag_name"].toString().isEmpty() || releaseUrl.scheme() != "https" || releaseUrl.host() != "github.com"
		|| !releaseUrl.path().startsWith("/SenjuWoo/imgbrd-grabber/releases/")) {
		m_source.clear();
		log(QStringLiteral("Unable to check for updates: %1").arg(reply->errorString()), Logger::Warning);
		emit failed(QStringLiteral("Unable to check for updates."));
		reply->deleteLater();
		return;
	}
	m_source = response;

	QString latest, changelog;
	bool isNew;
	if (m_nightly && lastRelease["tag_name"].toString() == QLatin1String("nightly")) {
		static const QRegularExpression regexHead(R"(Head:\**\s*([a-f0-9]{40}))");
		const auto match = regexHead.match(lastRelease["body"].toString());
		latest = match.hasMatch() ? match.captured(1) : lastRelease["target_commitish"].toString();
		isNew = !m_commit.isEmpty() && latest != m_commit;
		latest = latest.left(8);
	} else {
		latest = lastRelease["tag_name"].toString();
		if (latest.startsWith('v')) {
			latest.remove(0, 1);
		}
		isNew = compareVersions(latest, QString(VERSION)) > 0;
		changelog = lastRelease["body"].toString();
	}

	m_newVersion = latest;
	emit finished(latest, isNew, changelog);
	reply->deleteLater();
}


QUrl ProgramUpdater::latestUrl() const
{
	QJsonDocument json = QJsonDocument::fromJson(m_source);
	QJsonObject lastRelease = json.object();
	return QUrl(lastRelease["html_url"].toString());
}

void ProgramUpdater::downloadUpdate()
{
	QJsonDocument json = QJsonDocument::fromJson(m_source);
	QJsonObject lastRelease = json.object();
	QJsonArray lastAssets = lastRelease["assets"].toArray();
	QJsonObject lastAsset;
	for (int i = 0; i < lastAssets.size(); ++i) {
		const QJsonObject obj = lastAssets[i].toObject();
		const QString name = obj["name"].toString();
		if (name.endsWith(".exe") && name.contains(VERSION_PLATFORM)) {
			lastAsset = obj;
		}
	}
	if (lastAsset.isEmpty()) {
		log("No compatible release installer found for updating", Logger::Error);
		return;
	}

	QUrl url(lastAsset["browser_download_url"].toString());
	m_updateFilename = url.fileName();
	QNetworkRequest request(url);
	request.setTransferTimeout(30000);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	log(QStringLiteral("Downloading installer from \"%1\".").arg(url.toString()));

	m_downloadReply = m_networkAccessManager->get(request);
	connect(m_downloadReply, &NetworkReply::downloadProgress, this, &ProgramUpdater::downloadProgress);
	connect(m_downloadReply, &NetworkReply::finished, this, &ProgramUpdater::downloadDone);
}

void ProgramUpdater::downloadDone()
{
	QUrl redirection = m_downloadReply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!redirection.isEmpty()) {
		log(QStringLiteral("Installer download redirected to \"%1\".").arg(redirection.toString()));
		const QNetworkRequest request(redirection);
		m_downloadReply = m_networkAccessManager->get(request);
		connect(m_downloadReply, &NetworkReply::downloadProgress, this, &ProgramUpdater::downloadProgress);
		connect(m_downloadReply, &NetworkReply::finished, this, &ProgramUpdater::downloadDone);
		return;
	}

	QFile file(QDir::tempPath() + QDir::separator() + m_updateFilename);
	if (!file.open(QFile::WriteOnly | QFile::Truncate)) {
		log(QStringLiteral("Error opening installer file \"%1\".").arg(file.fileName()));
		return;
	}

	file.write(m_downloadReply->readAll());
	file.close();
	log(QStringLiteral("Installer file written to \"%1\"").arg(file.fileName()));

	emit downloadFinished(file.fileName());
}
