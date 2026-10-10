#include "network-follow.h"
#include <QString>


bool NetworkFollow::isAuthenticationUrl(const QUrl &url)
{
	const QString host = url.host().toLower();
	const QString path = url.path().toLower();

	if (host.startsWith(QLatin1String("login.")) || host.contains(QLatin1String(".login."))) {
		return true;
	}

	return path.contains(QLatin1String("/oidc"))
		|| path.contains(QLatin1String("/oauth"))
		|| path.contains(QLatin1String("/sso/"))
		|| path.contains(QLatin1String("/signin"))
		|| path.endsWith(QLatin1String("/login"))
		|| path.contains(QLatin1String("/login/"));
}

NetworkFollow::Action NetworkFollow::takeRedirect(const QUrl &from, const QUrl &to, QSet<QString> *seen, int *hops, QString *reason)
{
	if (to.isEmpty()) {
		if (reason != nullptr) {
			*reason = QStringLiteral("empty redirect");
		}
		return Action::Stop;
	}

	if (isAuthenticationUrl(to)) {
		if (reason != nullptr) {
			*reason = QStringLiteral("redirected to login `%1`").arg(to.toString());
		}
		return Action::Stop;
	}

	const QString fromKey = from.toString(QUrl::FullyEncoded);
	const QString toKey = to.toString(QUrl::FullyEncoded);
	if (seen != nullptr) {
		if (seen->contains(toKey) || (!fromKey.isEmpty() && fromKey == toKey)) {
			if (reason != nullptr) {
				*reason = QStringLiteral("redirect loop at `%1`").arg(to.toString());
			}
			return Action::Stop;
		}
		if (!fromKey.isEmpty()) {
			seen->insert(fromKey);
		}
		seen->insert(toKey);
	}

	if (hops != nullptr) {
		*hops += 1;
		if (*hops > MaxRedirects) {
			if (reason != nullptr) {
				*reason = QStringLiteral("too many redirects (%1)").arg(*hops);
			}
			return Action::Stop;
		}
	}

	return Action::Follow;
}

bool NetworkFollow::isTransient(QNetworkReply::NetworkError error, int statusCode)
{
	return statusCode >= 500 || statusCode == 429 || error == QNetworkReply::TimeoutError || error == QNetworkReply::RemoteHostClosedError
		|| error == QNetworkReply::TemporaryNetworkFailureError || error == QNetworkReply::NetworkSessionFailedError
		|| error == QNetworkReply::UnknownNetworkError || error == QNetworkReply::ProxyTimeoutError;
}

NetworkFollow::Action NetworkFollow::takeRetry(int *retries, QString *reason)
{
	if (retries == nullptr) {
		if (reason != nullptr) {
			*reason = QStringLiteral("retry counter missing");
		}
		return Action::Stop;
	}

	*retries += 1;
	if (*retries > MaxRetries) {
		if (reason != nullptr) {
			*reason = QStringLiteral("too many rate-limit retries (%1)").arg(*retries);
		}
		return Action::Stop;
	}

	return Action::Follow;
}
