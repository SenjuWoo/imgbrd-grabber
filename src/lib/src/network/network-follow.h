#ifndef NETWORK_FOLLOW_H
#define NETWORK_FOLLOW_H

#include <QNetworkReply>
#include <QSet>
#include <QString>
#include <QUrl>


/**
 * Shared policy for manual HTTP redirects and 429/503/509 retries.
 *
 * PageApi / Image / ImagePreview follow redirects themselves (sites use
 * ManualRedirectPolicy). Without a hop cap or login-URL rejection, a site
 * that bounces search results through OIDC (Sankaku) or 503s in a loop
 * (Zerochan) will keep creating replies until Qt6Core access-violates.
 */
class NetworkFollow
{
	public:
		static const int MaxRedirects = 8;
		static const int MaxRetries = 2;

		enum class Action
		{
			Follow,
			Stop
		};

		static bool isAuthenticationUrl(const QUrl &url);
		static Action takeRedirect(const QUrl &from, const QUrl &to, QSet<QString> *seen, int *hops, QString *reason);
		static Action takeRetry(int *retries, QString *reason);

		/** Dropped connections, timeouts and server errors, which usually work on a later try. */
		static bool isTransient(QNetworkReply::NetworkError error, int statusCode);
};

#endif // NETWORK_FOLLOW_H
