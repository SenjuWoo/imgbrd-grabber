#include <QEventLoop>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include "catch.h"
#include "custom-network-access-manager.h"
#include "functions.h"
#include "network/network-reply.h"

static void drainEventLoop(int ms = 50)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

/**
 * Regression tests for the NetworkReply state machine.
 *
 * These never dispatch a request, so they need no network and run by default -
 * unlike the "[.][network]" NetworkManager tests, which Catch2 skips and which
 * depend on httpbin.org, and so never caught any of this.
 */
TEST_CASE("NetworkReply", "[network-reply]")
{
	CustomNetworkAccessManager manager;
	const QNetworkRequest request(QUrl("http://localhost/never-dispatched"));

	SECTION("Aborting before dispatch emits nothing")
	{
		NetworkReply reply(request, &manager);

		int finishedCount = 0;
		QObject::connect(&reply, &NetworkReply::finished, [&]() { finishedCount++; });

		reply.abort();
		drainEventLoop();

		// Callers that abort() deliberately do not want a completion callback, and a
		// synchronous emit would re-enter them mid-teardown
		// (PageApi::parse -> setReply -> abort -> parse).
		REQUIRE(finishedCount == 0);
	}

	SECTION("An aborted reply never reports itself as running")
	{
		NetworkReply reply(request, &manager);
		REQUIRE(reply.isRunning());

		reply.abort();
		REQUIRE_FALSE(reply.isRunning());

		// The throttling manager can still call start() on a reply that was aborted
		// while queued. This used to clear the aborted flag and return without
		// starting, leaving a reply that reported isRunning() forever with no request
		// in flight, no timer pending, and no signal it would ever emit - so its
		// concurrency slot was never released and downloads hung until restart.
		reply.start(0);
		drainEventLoop();

		REQUIRE_FALSE(reply.isRunning());
	}

	SECTION("Reading an aborted reply yields no data instead of a closed-device read")
	{
		NetworkReply reply(request, &manager);
		reply.abort();
		drainEventLoop();

		REQUIRE(reply.readAll().isEmpty());
	}

	SECTION("Aborting after start() with a 0ms delay never dispatches")
	{
		NetworkReply reply(request, &manager);
		int finishedCount = 0;
		QObject::connect(&reply, &NetworkReply::finished, [&]() { finishedCount++; });

		reply.start(0);
		reply.abort();
		drainEventLoop();

		REQUIRE_FALSE(reply.isRunning());
		REQUIRE(finishedCount == 0);
		REQUIRE(reply.networkReply() == nullptr);
	}
}

TEST_CASE("A stalled host ends with an error instead of holding its request forever", "[network-reply]")
{
	QTcpServer server;
	REQUIRE(server.listen(QHostAddress::LocalHost));
	QList<QTcpSocket*> silent; // Accepted, never answered.
	QObject::connect(&server, &QTcpServer::newConnection, [&]() {
		while (server.hasPendingConnections()) {
			silent.append(server.nextPendingConnection());
		}
	});
	const bool testMode = isTestModeEnabled();
	setTestModeEnabled(false);
	auto restore = qScopeGuard([testMode]() { setTestModeEnabled(testMode); });

	CustomNetworkAccessManager manager;
	REQUIRE(manager.transferTimeout() > 0);
	manager.setTransferTimeout(300);
	NetworkReply reply(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/stall").arg(server.serverPort()))), &manager);
	QSignalSpy finished(&reply, &NetworkReply::finished);
	reply.start(0);
	REQUIRE(finished.wait(5000));
	REQUIRE(reply.error() == NetworkReply::NetworkError::TimeoutError);
}
