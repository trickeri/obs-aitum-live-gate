/*
OBS Aitum Live Gate
Copyright (c) 2026 trickeri

Released under the MIT License. See the accompanying LICENSE file for the full
license text.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

struct PlatformRow {
	QString id;
	QString source;
	QString platform;
	QString name;
	QString endpoint;
	QString title;
	bool enabled = false;
	bool obsMain = false;
};

struct HttpResponse {
	int statusCode = 0;
	QByteArray body;
	QString error;
};

struct TitlePublishResult {
	bool attempted = false;
	bool success = true;
	QString message;
};

using Headers = std::vector<std::pair<QByteArray, QByteArray>>;

QString text(const char *key)
{
	const char *value = obs_module_text(key);
	return QString::fromUtf8(value ? value : key);
}

QString currentProfileName()
{
	char *profile = obs_frontend_get_current_profile();
	QString value = QString::fromUtf8(profile ? profile : "default");
	bfree(profile);
	return value;
}

QString moduleConfigFile(const QString &fileName)
{
	char *rawPath = obs_module_config_path(fileName.toUtf8().constData());
	if (!rawPath)
		return {};
	QString path = QString::fromUtf8(rawPath);
	bfree(rawPath);
	return QDir::fromNativeSeparators(path);
}

QJsonObject readJsonObject(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QJsonParseError parseError{};
	const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		return {};
	return doc.object();
}

bool writeJsonObject(const QString &path, const QJsonObject &object)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	return true;
}

QString platformId(const QString &source, const QString &name)
{
	QString id = source + QStringLiteral(":") + name;
	id.replace(QStringLiteral("/"), QStringLiteral("_"));
	id.replace(QStringLiteral("\\"), QStringLiteral("_"));
	return id;
}

QString displayEndpoint(const QString &endpoint)
{
	if (endpoint.isEmpty())
		return {};
	QString trimmed = endpoint;
	trimmed.remove(QStringLiteral("rtmp://"), Qt::CaseInsensitive);
	trimmed.remove(QStringLiteral("rtmps://"), Qt::CaseInsensitive);
	return trimmed.section('/', 0, 0);
}

constexpr int kIconSize = 18;

QString platformIconPath(const QString &platform)
{
	// Reuse the platform icons that the Aitum multistream plugin compiles into
	// its own binary as Qt resources. Both plugins live in the same OBS process
	// and share one Qt resource registry, so these paths resolve as long as the
	// Aitum plugin is loaded (it always is by the time this dialog can open).
	if (platform == QStringLiteral("twitch"))
		return QStringLiteral(":/aitum/media/twitch.png");
	if (platform == QStringLiteral("youtube"))
		return QStringLiteral(":/aitum/media/youtube.png");
	if (platform == QStringLiteral("kick"))
		return QStringLiteral(":/aitum/media/kick.png");
	if (platform == QStringLiteral("x"))
		return QStringLiteral(":/aitum/media/twitter.png");
	if (platform == QStringLiteral("tiktok"))
		return QStringLiteral(":/aitum/media/tiktok.png");
	return {};
}

QString inferPlatform(const QString &name, const QString &endpoint)
{
	const QString haystack = (name + QStringLiteral(" ") + endpoint).toLower();
	if (haystack.contains(QStringLiteral("twitch")))
		return QStringLiteral("twitch");
	if (haystack.contains(QStringLiteral("youtube")) || haystack.contains(QStringLiteral("youtu.be")) ||
	    haystack.contains(QStringLiteral("ytimg")))
		return QStringLiteral("youtube");
	if (haystack.contains(QStringLiteral("kick")))
		return QStringLiteral("kick");
	if (haystack.contains(QStringLiteral("twitter")) || haystack.contains(QStringLiteral("x.com")) ||
	    haystack.contains(QStringLiteral("pscp.tv")) || haystack.contains(QStringLiteral("periscope")))
		return QStringLiteral("x");
	if (haystack.contains(QStringLiteral("tiktok")) || haystack.contains(QStringLiteral("byteoversea")) ||
	    haystack.contains(QStringLiteral("muscdn")))
		return QStringLiteral("tiktok");
	return {};
}

QNetworkRequest requestWithHeaders(const QUrl &url, const Headers &headers)
{
	QNetworkRequest request(url);
	for (const auto &header : headers)
		request.setRawHeader(header.first, header.second);
	return request;
}

HttpResponse sendRequest(const QByteArray &method, const QUrl &url, const Headers &headers, const QByteArray &body = {})
{
	HttpResponse response;
	QNetworkAccessManager manager;
	QNetworkRequest request = requestWithHeaders(url, headers);
	QNetworkReply *reply = manager.sendCustomRequest(request, method, body);

	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	timeout.start(15000);
	loop.exec();

	if (timeout.isActive()) {
		timeout.stop();
		response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		response.body = reply->readAll();
		if (reply->error() != QNetworkReply::NoError)
			response.error = reply->errorString();
	} else {
		reply->abort();
		response.error = QStringLiteral("request timed out");
	}

	reply->deleteLater();
	return response;
}

QJsonObject jsonObjectFromResponse(const HttpResponse &response)
{
	QJsonParseError parseError{};
	const auto doc = QJsonDocument::fromJson(response.body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		return {};
	return doc.object();
}

QString configString(const QJsonObject &config, const QString &key)
{
	return config.value(key).toString().trimmed();
}

TitlePublishResult updateTwitchTitle(const QJsonObject &config, const QString &title)
{
	const QString clientId = configString(config, QStringLiteral("clientId"));
	const QString broadcasterId = configString(config, QStringLiteral("broadcasterId"));
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	if (clientId.isEmpty() || broadcasterId.isEmpty() || accessToken.isEmpty())
		return {true, false, QStringLiteral("Twitch adapter needs clientId, broadcasterId, and accessToken.")};

	QUrl url(QStringLiteral("https://api.twitch.tv/helix/channels"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId);
	url.setQuery(query);

	const QJsonObject payload{{QStringLiteral("title"), title}};
	const auto response = sendRequest(
		"PATCH", url,
		{{"Authorization", "Bearer " + accessToken.toUtf8()},
		 {"Client-Id", clientId.toUtf8()},
		 {"Content-Type", "application/json"}},
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	if (response.statusCode == 204)
		return {true, true, QStringLiteral("Twitch title updated.")};
	return {true, false,
		QStringLiteral("Twitch title update failed (%1): %2").arg(response.statusCode).arg(QString::fromUtf8(response.body))};
}

TitlePublishResult updateKickTitle(const QJsonObject &config, const QString &title)
{
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	if (accessToken.isEmpty())
		return {true, false, QStringLiteral("Kick adapter needs accessToken with channel:write scope.")};

	const QJsonObject payload{{QStringLiteral("stream_title"), title}};
	const auto response = sendRequest(
		"PATCH", QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")),
		{{"Authorization", "Bearer " + accessToken.toUtf8()}, {"Content-Type", "application/json"}},
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	if (response.statusCode == 204)
		return {true, true, QStringLiteral("Kick title updated.")};
	return {true, false,
		QStringLiteral("Kick title update failed (%1): %2").arg(response.statusCode).arg(QString::fromUtf8(response.body))};
}

TitlePublishResult updateYouTubeTitle(const QJsonObject &config, const QString &title)
{
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	if (accessToken.isEmpty())
		return {true, false, QStringLiteral("YouTube adapter needs an access token (configure a refreshToken).")};

	const Headers authHeaders{{"Authorization", "Bearer " + accessToken.toUtf8()}, {"Accept", "application/json"}};

	// Decide which broadcast to retitle. YouTube creates a fresh broadcast per
	// stream, so unless a fixed broadcastId is pinned in settings we auto-select
	// the currently live broadcast, falling back to the next scheduled one.
	const QString pinnedId = configString(config, QStringLiteral("broadcastId"));

	QJsonObject broadcast;
	if (!pinnedId.isEmpty()) {
		QUrl listUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery listQuery;
		listQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
		listQuery.addQueryItem(QStringLiteral("id"), pinnedId);
		listUrl.setQuery(listQuery);

		const auto listResponse = sendRequest("GET", listUrl, authHeaders);
		if (listResponse.statusCode != 200)
			return {true, false,
				QStringLiteral("YouTube broadcast lookup failed (%1): %2")
					.arg(listResponse.statusCode)
					.arg(QString::fromUtf8(listResponse.body))};

		const auto items = jsonObjectFromResponse(listResponse).value(QStringLiteral("items")).toArray();
		if (!items.isEmpty())
			broadcast = items.first().toObject();
	} else {
		// Auto-select the broadcast for this session. We query *all* broadcasts
		// (not just active/upcoming) because a stream sent to YouTube's reusable
		// "Default stream" key with monitor/testing enabled sits in the
		// `ready`/`testing` lifecycle for a while after ingest begins, before
		// YouTube auto-transitions it to `live`. The old active+upcoming-only
		// lookup raced that window and gave up (logged "No live or upcoming
		// broadcast found"). Rank candidates by how far along they are and skip
		// finished/revoked ones; the furthest-along live session wins.
		QUrl listUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery listQuery;
		listQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
		listQuery.addQueryItem(QStringLiteral("broadcastStatus"), QStringLiteral("all"));
		listQuery.addQueryItem(QStringLiteral("broadcastType"), QStringLiteral("all"));
		listQuery.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
		listUrl.setQuery(listQuery);

		const auto listResponse = sendRequest("GET", listUrl, authHeaders);
		if (listResponse.statusCode != 200)
			return {true, false,
				QStringLiteral("YouTube broadcast lookup failed (%1): %2")
					.arg(listResponse.statusCode)
					.arg(QString::fromUtf8(listResponse.body))};

		const auto items = jsonObjectFromResponse(listResponse).value(QStringLiteral("items")).toArray();
		int bestRank = 0;
		for (const auto &item : items) {
			const QJsonObject obj = item.toObject();
			const QString life = obj.value(QStringLiteral("status"))
						     .toObject()
						     .value(QStringLiteral("lifeCycleStatus"))
						     .toString();
			int rank = 0;
			if (life == QStringLiteral("live") || life == QStringLiteral("liveStarting"))
				rank = 4; // already public-live
			else if (life == QStringLiteral("testing"))
				rank = 3; // ingest bound, in monitor/preview
			else if (life == QStringLiteral("ready"))
				rank = 2; // bound or scheduled, not yet testing
			else if (life == QStringLiteral("created"))
				rank = 1; // exists but unbound
			else
				continue; // complete, completed, revoked, …
			if (rank > bestRank) {
				bestRank = rank;
				broadcast = obj;
			}
		}
	}

	if (broadcast.isEmpty())
		return {true, false,
			QStringLiteral("No active or pending YouTube broadcast found. Start or schedule your stream first.")};

	QJsonObject snippet = broadcast.value(QStringLiteral("snippet")).toObject();
	snippet.insert(QStringLiteral("title"), title);
	broadcast.insert(QStringLiteral("snippet"), snippet);

	QUrl updateUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
	QUrlQuery updateQuery;
	updateQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
	updateUrl.setQuery(updateQuery);

	const auto updateResponse = sendRequest(
		"PUT", updateUrl,
		{{"Authorization", "Bearer " + accessToken.toUtf8()},
		 {"Accept", "application/json"},
		 {"Content-Type", "application/json"}},
		QJsonDocument(broadcast).toJson(QJsonDocument::Compact));

	if (updateResponse.statusCode >= 200 && updateResponse.statusCode < 300)
		return {true, true, QStringLiteral("YouTube title updated.")};
	return {true, false,
		QStringLiteral("YouTube title update failed (%1): %2")
			.arg(updateResponse.statusCode)
			.arg(QString::fromUtf8(updateResponse.body))};
}

struct BroadcastResult {
	bool ok = false;
	QString broadcastId;
	QString message;
};

// Pick the ingest stream a new broadcast should bind to. A pinned `streamId` in
// settings wins; otherwise prefer a configured `streamTitle`, then the reusable
// "Default stream" key, then whatever the account lists first.
QString resolveYouTubeStreamId(const QJsonObject &config, const QByteArray &accessToken)
{
	const QString pinned = configString(config, QStringLiteral("streamId"));
	if (!pinned.isEmpty())
		return pinned;

	QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveStreams"));
	QUrlQuery q;
	q.addQueryItem(QStringLiteral("part"), QStringLiteral("id,snippet"));
	q.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
	q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
	url.setQuery(q);

	const auto resp = sendRequest("GET", url,
				      {{"Authorization", "Bearer " + accessToken}, {"Accept", "application/json"}});
	if (resp.statusCode != 200)
		return {};

	const auto items = jsonObjectFromResponse(resp).value(QStringLiteral("items")).toArray();
	const QString wantTitle = configString(config, QStringLiteral("streamTitle"));
	QString firstId, defaultId;
	for (const auto &it : items) {
		const QJsonObject o = it.toObject();
		const QString id = o.value(QStringLiteral("id")).toString();
		const QString title = o.value(QStringLiteral("snippet")).toObject().value(QStringLiteral("title")).toString();
		if (firstId.isEmpty())
			firstId = id;
		if (!wantTitle.isEmpty() && title.compare(wantTitle, Qt::CaseInsensitive) == 0)
			return id;
		if (title.contains(QStringLiteral("default"), Qt::CaseInsensitive) && defaultId.isEmpty())
			defaultId = id;
	}
	return !defaultId.isEmpty() ? defaultId : firstId;
}

// Create a fresh YouTube broadcast and bind it to an ingest stream, returning the
// new broadcast id. This is what lets Live Gate go live without the broadcaster
// hand-creating a broadcast in YouTube Studio first. It MUST run BEFORE the Aitum
// YouTube output starts ingesting: the broadcast has to already exist, be bound, and
// be `ready` so the off->on ingest edge (the Aitum "go live" button click) trips
// enableAutoStart and YouTube promotes it straight to `live`. monitorStream is left
// OFF so there is no intermediate testing/preview state to get wedged in (a
// Studio-created broadcast forces monitor ON, which is what kept sticking on
// "Preparing stream").
BroadcastResult createYouTubeBroadcast(const QJsonObject &config, const QString &title)
{
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	if (accessToken.isEmpty())
		return {false, {}, QStringLiteral("YouTube adapter needs an access token (configure a refreshToken).")};
	const QByteArray bearer = "Bearer " + accessToken.toUtf8();
	const Headers jsonHeaders{{"Authorization", bearer}, {"Accept", "application/json"}, {"Content-Type", "application/json"}};

	const QString startIso = QDateTime::currentDateTimeUtc().addSecs(30).toString(Qt::ISODate);
	const QJsonObject snippet{{QStringLiteral("title"), title.isEmpty() ? QStringLiteral("Live Stream") : title},
				  {QStringLiteral("scheduledStartTime"), startIso}};
	const QJsonObject status{{QStringLiteral("privacyStatus"), QStringLiteral("public")},
				 {QStringLiteral("selfDeclaredMadeForKids"), false}};
	const QJsonObject contentDetails{{QStringLiteral("enableAutoStart"), true},
					 {QStringLiteral("enableAutoStop"), true},
					 {QStringLiteral("enableDvr"), true},
					 {QStringLiteral("latencyPreference"), QStringLiteral("low")},
					 {QStringLiteral("monitorStream"),
					  QJsonObject{{QStringLiteral("enableMonitorStream"), false}}}};
	const QJsonObject body{{QStringLiteral("snippet"), snippet},
			       {QStringLiteral("status"), status},
			       {QStringLiteral("contentDetails"), contentDetails}};

	QUrl insertUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
	QUrlQuery iq;
	iq.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
	insertUrl.setQuery(iq);
	const auto insResp = sendRequest("POST", insertUrl, jsonHeaders, QJsonDocument(body).toJson(QJsonDocument::Compact));
	if (insResp.statusCode < 200 || insResp.statusCode >= 300)
		return {false, {},
			QStringLiteral("YouTube broadcast insert failed (%1): %2")
				.arg(insResp.statusCode)
				.arg(QString::fromUtf8(insResp.body))};
	const QString broadcastId = jsonObjectFromResponse(insResp).value(QStringLiteral("id")).toString();
	if (broadcastId.isEmpty())
		return {false, {}, QStringLiteral("YouTube broadcast insert returned no id.")};

	const QString streamId = resolveYouTubeStreamId(config, accessToken.toUtf8());
	if (streamId.isEmpty())
		return {false, broadcastId,
			QStringLiteral("Created broadcast %1 but could not resolve a stream to bind.").arg(broadcastId)};

	QUrl bindUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts/bind"));
	QUrlQuery bq;
	bq.addQueryItem(QStringLiteral("id"), broadcastId);
	bq.addQueryItem(QStringLiteral("part"), QStringLiteral("id,contentDetails"));
	bq.addQueryItem(QStringLiteral("streamId"), streamId);
	bindUrl.setQuery(bq);
	const auto bindResp = sendRequest("POST", bindUrl, jsonHeaders);
	if (bindResp.statusCode < 200 || bindResp.statusCode >= 300)
		return {false, broadcastId,
			QStringLiteral("Created broadcast %1 but bind failed (%2): %3")
				.arg(broadcastId)
				.arg(bindResp.statusCode)
				.arg(QString::fromUtf8(bindResp.body))};

	return {true, broadcastId,
		QStringLiteral("Created and bound YouTube broadcast %1 (autostart on, monitor off).").arg(broadcastId)};
}

TitlePublishResult publishPlatformTitle(const QString &platform, const QJsonObject &adapterConfig, const QString &title)
{
	if (platform == QStringLiteral("twitch"))
		return updateTwitchTitle(adapterConfig, title);
	if (platform == QStringLiteral("youtube"))
		return updateYouTubeTitle(adapterConfig, title);
	if (platform == QStringLiteral("kick"))
		return updateKickTitle(adapterConfig, title);
	if (platform == QStringLiteral("x"))
		return {false, true,
			QStringLiteral("X title updates are not implemented because X live Producer/Media Studio does not expose a generally available public title-update API.")};
	return {false, true, QStringLiteral("No title adapter for platform '%1'.").arg(platform)};
}

QString tokenEndpoint(const QString &platform)
{
	if (platform == QStringLiteral("twitch"))
		return QStringLiteral("https://id.twitch.tv/oauth2/token");
	if (platform == QStringLiteral("youtube"))
		return QStringLiteral("https://oauth2.googleapis.com/token");
	if (platform == QStringLiteral("kick"))
		return QStringLiteral("https://id.kick.com/oauth/token");
	return {};
}

struct TokenRefreshResult {
	bool ok = false;
	QString accessToken;
	QString refreshToken; // non-empty only when the platform rotated it
	QString error;
};

// Trade a stored refresh token for a fresh, short-lived access token. This is the
// OAuth refresh_token grant every platform exposes; we run it right before pushing
// a title so the access token is always valid even though it expires in ~1 hour.
TokenRefreshResult refreshAccessToken(const QString &platform, const QJsonObject &config)
{
	const QString endpoint = tokenEndpoint(platform);
	const QString refreshToken = configString(config, QStringLiteral("refreshToken"));
	const QString clientId = configString(config, QStringLiteral("clientId"));
	const QString clientSecret = configString(config, QStringLiteral("clientSecret"));
	if (endpoint.isEmpty())
		return {false, {}, {}, QStringLiteral("no token endpoint for platform '%1'.").arg(platform)};
	if (refreshToken.isEmpty() || clientId.isEmpty() || clientSecret.isEmpty())
		return {false, {}, {}, QStringLiteral("refresh needs clientId, clientSecret, and refreshToken.")};

	const auto enc = [](const QString &value) { return QString::fromUtf8(QUrl::toPercentEncoding(value)); };
	const QString body =
		QStringLiteral("grant_type=refresh_token&refresh_token=%1&client_id=%2&client_secret=%3")
			.arg(enc(refreshToken), enc(clientId), enc(clientSecret));

	const auto response = sendRequest(
		"POST", QUrl(endpoint),
		{{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}}, body.toUtf8());

	if (response.statusCode < 200 || response.statusCode >= 300)
		return {false, {}, {},
			QStringLiteral("token refresh failed (%1): %2")
				.arg(response.statusCode)
				.arg(QString::fromUtf8(response.body))};

	const auto object = jsonObjectFromResponse(response);
	const QString accessToken = object.value(QStringLiteral("access_token")).toString();
	if (accessToken.isEmpty())
		return {false, {}, {}, QStringLiteral("token endpoint returned no access_token.")};

	// Twitch and Kick (OAuth 2.1) rotate the refresh token on each use, so the
	// caller must persist the new one. Google keeps the same refresh token.
	QString rotated = object.value(QStringLiteral("refresh_token")).toString();
	if (rotated == refreshToken)
		rotated.clear();
	return {true, accessToken, rotated, {}};
}

class GoLiveDialog final : public QDialog {
public:
	explicit GoLiveDialog(std::vector<PlatformRow> rows, QWidget *parent = nullptr, bool previewOnly = false)
		: QDialog(parent),
		  rows_(std::move(rows)),
		  previewOnly_(previewOnly)
	{
		setWindowTitle(previewOnly ? text("LiveGate.PreviewTitle") : text("LiveGate.Title"));
		setModal(true);
		resize(720, 240);

		auto *root = new QVBoxLayout(this);
		auto *intro = new QLabel(previewOnly ? text("LiveGate.PreviewIntro") : text("LiveGate.Intro"), this);
		intro->setWordWrap(true);
		root->addWidget(intro);

		auto *grid = new QGridLayout;
		grid->setColumnStretch(2, 1);
		grid->addWidget(new QLabel(text("LiveGate.Enabled"), this), 0, 0);
		grid->addWidget(new QLabel(text("LiveGate.Platform"), this), 0, 1);
		grid->addWidget(new QLabel(text("LiveGate.StreamTitle"), this), 0, 2);

		// Master title bar: typing here sets every platform's title at once, so the
		// common case (same title everywhere) is one field. Per-platform titles below
		// can still be edited afterwards to override/tweak any individual platform.
		auto *masterLabel = new QLabel(QStringLiteral("All Platforms"), this);
		QFont masterFont = masterLabel->font();
		masterFont.setBold(true);
		masterLabel->setFont(masterFont);
		grid->addWidget(masterLabel, 1, 1);

		auto *masterTitle = new QLineEdit(this);
		masterTitle->setPlaceholderText(QStringLiteral("Set title for all platforms…"));
		grid->addWidget(masterTitle, 1, 2);

		int rowNumber = 2;
		for (auto &row : rows_) {
			auto *enabled = new QCheckBox(this);
			enabled->setChecked(row.enabled);
			enabledBoxes_.push_back(enabled);
			grid->addWidget(enabled, rowNumber, 0, Qt::AlignHCenter);

			auto *platformCell = new QWidget(this);
			auto *platformLayout = new QHBoxLayout(platformCell);
			platformLayout->setContentsMargins(0, 0, 0, 0);
			platformLayout->setSpacing(6);

			if (row.obsMain) {
				// The OBS main stream can't be auto-detected from a name/endpoint
				// like the Aitum rows, so let the user pick its platform here
				// instead of hand-editing settings.json. The choice is persisted.
				auto *combo = new QComboBox(platformCell);
				addPlatformItem(combo, QString(), text("LiveGate.PlatformNone"));
				addPlatformItem(combo, QStringLiteral("twitch"), QStringLiteral("Twitch"));
				addPlatformItem(combo, QStringLiteral("youtube"), QStringLiteral("YouTube"));
				addPlatformItem(combo, QStringLiteral("kick"), QStringLiteral("Kick"));
				addPlatformItem(combo, QStringLiteral("x"), QStringLiteral("X"));
				const int selected = combo->findData(row.platform);
				combo->setCurrentIndex(selected >= 0 ? selected : 0);
				mainPlatformCombo_ = combo;
				platformLayout->addWidget(combo);
			} else {
				auto *icon = new QLabel(platformCell);
				icon->setFixedSize(kIconSize, kIconSize);
				const QPixmap pixmap(platformIconPath(row.platform));
				if (!pixmap.isNull())
					icon->setPixmap(pixmap.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio,
								      Qt::SmoothTransformation));
				platformLayout->addWidget(icon);
			}
			platformLayout->addWidget(new QLabel(row.name, platformCell));
			platformLayout->addStretch();
			grid->addWidget(platformCell, rowNumber, 1);

			auto *title = new QLineEdit(row.title, this);
			title->setPlaceholderText(QStringLiteral("Stream Title"));
			titleEdits_.push_back(title);
			grid->addWidget(title, rowNumber, 2);
			rowNumber++;
		}

		// Editing the master field overwrites every per-platform title. We use
		// textEdited (user keystrokes only), so programmatic per-row setText below
		// and any manual per-platform overrides afterwards never feed back into it.
		connect(masterTitle, &QLineEdit::textEdited, this, [this](const QString &t) {
			for (auto *edit : titleEdits_)
				edit->setText(t);
		});

		// If every platform already carries the same non-empty title, surface it in
		// the master field so it reflects the current shared state on open.
		if (!titleEdits_.empty()) {
			const QString first = titleEdits_.front()->text();
			const bool allSame =
				!first.isEmpty() &&
				std::all_of(titleEdits_.begin(), titleEdits_.end(),
					    [&](QLineEdit *e) { return e->text() == first; });
			if (allSame)
				masterTitle->setText(first);
		}

		root->addLayout(grid);

		if (rows_.size() <= 1) {
			auto *hint = new QLabel(text("LiveGate.NoAitumOutputs"), this);
			hint->setWordWrap(true);
			root->addWidget(hint);
		}

		auto *buttons = new QDialogButtonBox(this);
		if (previewOnly) {
			buttons->addButton(text("LiveGate.ClosePreview"), QDialogButtonBox::AcceptRole);
		} else {
			buttons->addButton(text("LiveGate.GoLive"), QDialogButtonBox::AcceptRole);
			buttons->addButton(text("LiveGate.Cancel"), QDialogButtonBox::RejectRole);
		}
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		root->addWidget(buttons);

		// Snapshot the opening state so reject() can tell whether the user
		// actually changed anything and only then warn before discarding.
		for (auto *box : enabledBoxes_)
			initialEnabled_.push_back(box->isChecked());
		for (auto *edit : titleEdits_)
			initialTitles_.push_back(edit->text());
		initialPlatform_ = mainPlatformCombo_ ? mainPlatformCombo_->currentData().toString() : QString();
	}

	std::vector<PlatformRow> rows() const
	{
		auto updated = rows_;
		for (size_t i = 0; i < updated.size(); i++) {
			updated[i].enabled = enabledBoxes_[i]->isChecked();
			updated[i].title = titleEdits_[i]->text().trimmed();
			if (updated[i].obsMain && mainPlatformCombo_)
				updated[i].platform = mainPlatformCombo_->currentData().toString();
		}
		return updated;
	}

	// Guard against losing edits to a stray Cancel/Escape/window-close: if the
	// user changed any title, go-live toggle, or the OBS main platform, confirm
	// before discarding. Preview mode never persists anything, so it just closes.
	void reject() override
	{
		if (!previewOnly_ && isDirty()) {
			const auto choice = QMessageBox::question(this, text("LiveGate.DiscardTitle"),
								  text("LiveGate.DiscardText"),
								  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (choice != QMessageBox::Yes)
				return; // keep the dialog open so the user can keep editing
		}
		QDialog::reject();
	}

private:
	bool isDirty() const
	{
		for (size_t i = 0; i < titleEdits_.size(); i++)
			if (titleEdits_[i]->text() != initialTitles_[i])
				return true;
		for (size_t i = 0; i < enabledBoxes_.size(); i++)
			if (enabledBoxes_[i]->isChecked() != initialEnabled_[i])
				return true;
		if (mainPlatformCombo_ && mainPlatformCombo_->currentData().toString() != initialPlatform_)
			return true;
		return false;
	}

	static void addPlatformItem(QComboBox *combo, const QString &platform, const QString &label)
	{
		const QPixmap pixmap(platformIconPath(platform));
		if (!pixmap.isNull())
			combo->addItem(QIcon(pixmap), label, platform);
		else
			combo->addItem(label, platform);
	}

	std::vector<PlatformRow> rows_;
	bool previewOnly_ = false;
	std::vector<QCheckBox *> enabledBoxes_;
	std::vector<QLineEdit *> titleEdits_;
	QComboBox *mainPlatformCombo_ = nullptr;
	std::vector<bool> initialEnabled_;
	std::vector<QString> initialTitles_;
	QString initialPlatform_;
};

class LiveGateController final : public QObject {
public:
	explicit LiveGateController(QObject *parent = nullptr) : QObject(parent) {}

	void install()
	{
		if (qApp)
			qApp->installEventFilter(this);
		obs_frontend_add_event_callback(&LiveGateController::frontendEvent, this);
		obs_frontend_add_tools_menu_item("Preview Go Live Gate", &LiveGateController::previewMenuClicked, this);
		obs_log(LOG_INFO, "OBS Aitum Live Gate loaded");
	}

	void uninstall()
	{
		obs_frontend_remove_event_callback(&LiveGateController::frontendEvent, this);
		if (qApp)
			qApp->removeEventFilter(this);
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (programmaticStart_ || showingDialog_ || obs_frontend_streaming_active())
			return QObject::eventFilter(watched, event);

		if (event->type() != QEvent::MouseButtonPress)
			return QObject::eventFilter(watched, event);

		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() != Qt::LeftButton)
			return QObject::eventFilter(watched, event);

		auto *button = qobject_cast<QPushButton *>(watched);
		if (!button || !isStartStreamingButton(button))
			return QObject::eventFilter(watched, event);

		showingDialog_ = true;
		QTimer::singleShot(0, this, [this] {
			showGateDialog();
			showingDialog_ = false;
		});
		return true;
	}

private:
	static void frontendEvent(enum obs_frontend_event event, void *data)
	{
		auto *self = static_cast<LiveGateController *>(data);
		if (!self)
			return;

		if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
			// applyAitumSelections starts the other platforms now and schedules
			// YouTube (plus its deferred title push) on a later, staggered pass.
			QTimer::singleShot(1200, self, [self] { self->applyAitumSelections(); });
			self->programmaticStart_ = false;
		} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
			// Stop the Aitum outputs we started before we forget which they were.
			self->setAitumOutputs(false);
			self->pendingYouTubeRows_.clear();
			self->programmaticStart_ = false;
		} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
			self->pendingAitumIds_.clear();
			self->programmaticStart_ = false;
		}
	}

	static void previewMenuClicked(void *data)
	{
		auto *self = static_cast<LiveGateController *>(data);
		if (self)
			QTimer::singleShot(0, self, [self] { self->showPreviewDialog(); });
	}

	bool isStartStreamingButton(const QPushButton *button) const
	{
		QString buttonText = button->text();
		buttonText.remove(QChar('&'));
		buttonText = buttonText.trimmed();

		QString localized = QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.StartStreaming"));
		localized.remove(QChar('&'));
		localized = localized.trimmed();

		return button->isEnabled() && button->isVisible() &&
		       (buttonText.compare(localized, Qt::CaseInsensitive) == 0 ||
			buttonText.compare(QStringLiteral("Start Streaming"), Qt::CaseInsensitive) == 0 ||
			buttonText.contains(QStringLiteral("Start Stream"), Qt::CaseInsensitive));
	}

	void showGateDialog()
	{
		auto rows = loadRows();
		auto *mainWindow = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
		GoLiveDialog dialog(std::move(rows), mainWindow);
		const bool accepted = dialog.exec() == QDialog::Accepted;

		// Cancel/Escape/window-close discards: the dialog already confirmed
		// "Discard changes?" when there were edits, so leave the saved settings
		// untouched and reopen with the last persisted choices next time.
		if (!accepted)
			return;

		// Persist only the choices the user actually committed by going live.
		const auto editedRows = dialog.rows();
		saveRows(editedRows);

		lastAcceptedRows_ = editedRows;

		pendingAitumIds_.clear();
		bool startMainStream = false;
		for (const auto &row : lastAcceptedRows_) {
			if (row.obsMain) {
				startMainStream = row.enabled;
			} else if (row.enabled) {
				pendingAitumIds_.push_back(row.name);
			}
		}

		if (!startMainStream) {
			obs_log(LOG_INFO, "Live Gate cancelled OBS main stream because the main stream row was disabled");
			return;
		}

		QStringList titleWarnings;
		const bool titleUpdatesOk = publishTitles(titleWarnings);
		if (!titleUpdatesOk) {
			QMessageBox::warning(mainWindow, text("LiveGate.TitleUpdateWarningTitle"),
					     text("LiveGate.TitleUpdateWarningText") + QStringLiteral("\n\n") +
						     titleWarnings.join(QStringLiteral("\n")));
			obs_log(LOG_WARNING, "Live Gate aborted stream because strict title updates failed");
			return;
		}
		if (!titleWarnings.isEmpty()) {
			QMessageBox box(mainWindow);
			box.setIcon(QMessageBox::Warning);
			box.setWindowTitle(text("LiveGate.TitleUpdateWarningTitle"));
			box.setText(text("LiveGate.TitleUpdateWarningText") + QStringLiteral("\n\n") +
				    titleWarnings.join(QStringLiteral("\n")));
			QPushButton *goLiveButton = box.addButton(text("LiveGate.GoLiveAnyway"), QMessageBox::AcceptRole);
			box.addButton(text("LiveGate.Cancel"), QMessageBox::RejectRole);
			box.setDefaultButton(goLiveButton);
			box.exec();
			if (box.clickedButton() != goLiveButton) {
				obs_log(LOG_INFO, "Live Gate aborted stream at the title warning by user choice");
				return;
			}
		}

		programmaticStart_ = true;
		obs_frontend_streaming_start();
	}

	void showPreviewDialog()
	{
		auto rows = loadRows();
		auto *mainWindow = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
		GoLiveDialog dialog(std::move(rows), mainWindow, true);
		dialog.exec();
	}

	std::vector<PlatformRow> loadRows() const
	{
		std::vector<PlatformRow> rows;
		const auto saved = readJsonObject(settingsPath()).value(QStringLiteral("platforms")).toObject();

		PlatformRow main;
		main.id = platformId(QStringLiteral("obs"), text("LiveGate.ObsMain"));
		main.source = QStringLiteral("obs");
		main.name = text("LiveGate.ObsMain");
		main.enabled = true;
		main.obsMain = true;
		applySaved(main, saved);
		rows.push_back(main);

		for (auto row : loadAitumRows()) {
			applySaved(row, saved);
			rows.push_back(row);
		}

		return rows;
	}

	void applySaved(PlatformRow &row, const QJsonObject &saved) const
	{
		const auto object = saved.value(row.id).toObject();
		// The OBS main stream row drives whether OBS goes live at all (and is
		// what triggers the Aitum outputs), so always reload it checked rather
		// than letting a previous unchecked session get it stuck off.
		if (!row.obsMain && object.contains(QStringLiteral("enabled")))
			row.enabled = object.value(QStringLiteral("enabled")).toBool(row.enabled);
		row.platform = object.value(QStringLiteral("platform")).toString(row.platform);
		row.title = object.value(QStringLiteral("title")).toString(row.title);
	}

	std::vector<PlatformRow> loadAitumRows() const
	{
		std::vector<PlatformRow> rows;
		const QString aitumPath = aitumConfigPath();
		const auto root = readJsonObject(aitumPath);
		const QString profile = currentProfileName();

		for (const auto profileValue : root.value(QStringLiteral("profiles")).toArray()) {
			const auto profileObject = profileValue.toObject();
			if (profileObject.value(QStringLiteral("name")).toString() != profile)
				continue;

			for (const auto outputValue : profileObject.value(QStringLiteral("outputs")).toArray()) {
				const auto output = outputValue.toObject();
				const QString name = output.value(QStringLiteral("name")).toString().trimmed();
				if (name.isEmpty())
					continue;
				PlatformRow row;
				row.source = QStringLiteral("aitum");
				row.name = name;
				row.endpoint = output.value(QStringLiteral("stream_server")).toString();
				row.platform = inferPlatform(row.name, row.endpoint);
				row.id = platformId(row.source, row.name);
				row.enabled = false;
				rows.push_back(row);
			}
			break;
		}

		return rows;
	}

	QString settingsPath() const { return moduleConfigFile(QStringLiteral("settings.json")); }

	QString aitumConfigPath() const
	{
		const QString ownConfig = moduleConfigFile(QStringLiteral("settings.json"));
		if (ownConfig.isEmpty())
			return {};

		QDir dir(QFileInfo(ownConfig).absoluteDir());
		dir.cdUp();
		return dir.filePath(QStringLiteral("aitum-multistream/config.json"));
	}

	void saveRows(const std::vector<PlatformRow> &rows) const
	{
		QJsonObject root = readJsonObject(settingsPath());
		QJsonObject platforms;
		for (const auto &row : rows) {
			QJsonObject object;
			object.insert(QStringLiteral("source"), row.source);
			object.insert(QStringLiteral("platform"), row.platform);
			object.insert(QStringLiteral("name"), row.name);
			object.insert(QStringLiteral("endpoint"), row.endpoint);
			object.insert(QStringLiteral("enabled"), row.enabled);
			object.insert(QStringLiteral("title"), row.title);
			platforms.insert(row.id, object);
		}
		root.insert(QStringLiteral("profile"), currentProfileName());
		root.insert(QStringLiteral("platforms"), platforms);
		if (!writeJsonObject(settingsPath(), root))
			obs_log(LOG_WARNING, "Live Gate failed to save settings to %s", settingsPath().toUtf8().constData());
	}

	// Publishes one row's title: mints a fresh access token from the stored
	// refresh token when present, pushes the title, and persists a rotated
	// refresh token (Twitch/Kick) back to settings for next time.
	TitlePublishResult publishRowTitle(const PlatformRow &row) const
	{
		QJsonObject root = readJsonObject(settingsPath());
		QJsonObject adapters = root.value(QStringLiteral("titleAdapters")).toObject();
		QJsonObject adapterConfig = adapters.value(row.platform).toObject();
		if (!adapterConfig.value(QStringLiteral("enabled")).toBool(false))
			return {true, false, QStringLiteral("%1 adapter is not enabled; title not updated.").arg(row.platform)};

		if (!configString(adapterConfig, QStringLiteral("refreshToken")).isEmpty()) {
			const auto refreshed = refreshAccessToken(row.platform, adapterConfig);
			if (!refreshed.ok)
				return {true, false, refreshed.error};
			adapterConfig.insert(QStringLiteral("accessToken"), refreshed.accessToken);
			if (!refreshed.refreshToken.isEmpty()) {
				adapterConfig.insert(QStringLiteral("refreshToken"), refreshed.refreshToken);
				adapters.insert(row.platform, adapterConfig);
				root.insert(QStringLiteral("titleAdapters"), adapters);
				if (!writeJsonObject(settingsPath(), root))
					obs_log(LOG_WARNING, "Live Gate failed to persist rotated refresh token to %s",
						settingsPath().toUtf8().constData());
			}
		}

		return publishPlatformTitle(row.platform, adapterConfig, row.title);
	}

	bool publishTitles(QStringList &warnings)
	{
		const QJsonObject adapters = readJsonObject(settingsPath()).value(QStringLiteral("titleAdapters")).toObject();
		const bool strict = adapters.value(QStringLiteral("strictTitleUpdates")).toBool(false);
		bool ok = true;
		pendingYouTubeRows_.clear();

		for (const auto &row : lastAcceptedRows_) {
			if (!row.enabled || row.title.isEmpty())
				continue;

			if (row.platform.isEmpty()) {
				const QString message = QStringLiteral("%1: platform could not be inferred; title not updated.").arg(row.name);
				warnings.push_back(message);
				obs_log(LOG_WARNING, "%s", message.toUtf8().constData());
				if (strict)
					ok = false;
				continue;
			}

			// YouTube only creates the broadcast object once RTMP ingest begins,
			// so defer its title push to just after streaming starts (with retries)
			// rather than failing here before there is anything to update.
			if (row.platform == QStringLiteral("youtube")) {
				pendingYouTubeRows_.push_back(row);
				continue;
			}

			const auto result = publishRowTitle(row);
			obs_log(result.success ? LOG_INFO : LOG_WARNING, "%s", result.message.toUtf8().constData());
			if (!result.success) {
				warnings.push_back(QStringLiteral("%1: %2").arg(row.name, result.message));
				if (strict)
					ok = false;
			}
		}

		return ok;
	}

	// Retried shortly after streaming starts: the YouTube broadcast can take a few
	// seconds to appear once ingest begins, so keep trying until it lands.
	void publishPendingYouTubeTitles(int attempt = 0)
	{
		static constexpr int kMaxAttempts = 8;
		static constexpr int kRetryDelayMs = 5000;
		if (pendingYouTubeRows_.empty())
			return;

		std::vector<PlatformRow> stillPending;
		for (const auto &row : pendingYouTubeRows_) {
			const auto result = publishRowTitle(row);
			obs_log(result.success ? LOG_INFO : LOG_WARNING, "%s", result.message.toUtf8().constData());
			if (!result.success)
				stillPending.push_back(row);
		}
		pendingYouTubeRows_ = std::move(stillPending);

		if (pendingYouTubeRows_.empty())
			return;
		if (attempt + 1 < kMaxAttempts) {
			QTimer::singleShot(kRetryDelayMs, this, [this, attempt] { publishPendingYouTubeTitles(attempt + 1); });
		} else {
			for (const auto &row : pendingYouTubeRows_)
				obs_log(LOG_WARNING, "Live Gate gave up updating the YouTube title for '%s'",
					row.name.toUtf8().constData());
			pendingYouTubeRows_.clear();
		}
	}

	// How long after the other platforms go live before YouTube is started, in ms.
	// Measured from applyAitumSelections (itself ~1.2s after OBS streaming starts).
	static constexpr int kYouTubeStartDelayMs = 8000;

	// Which Aitum outputs a start/stop pass should touch. YouTube is staggered so
	// it never goes live in the same instant as the other platforms.
	enum class AitumPhase { All, NonYouTube, YouTubeOnly };

	bool isYouTubeOutput(const QString &name) const
	{
		for (const auto &row : lastAcceptedRows_) {
			if (row.name == name)
				return row.platform == QStringLiteral("youtube");
		}
		return false;
	}

	bool hasPendingYouTube() const
	{
		for (const auto &name : pendingAitumIds_) {
			if (isYouTubeOutput(name))
				return true;
		}
		return false;
	}

	// Create + bind a fresh YouTube broadcast for the enabled YouTube output, using
	// the title chosen in the go-live dialog. Runs just before the YouTube ingest so
	// autostart can promote the new `ready` broadcast to live on the ingest edge.
	void ensureYouTubeBroadcasts()
	{
		if (!hasPendingYouTube())
			return;

		QJsonObject adapterConfig =
			readJsonObject(settingsPath()).value(QStringLiteral("titleAdapters")).toObject().value(QStringLiteral("youtube")).toObject();
		if (!adapterConfig.value(QStringLiteral("enabled")).toBool(false)) {
			obs_log(LOG_INFO, "Live Gate: YouTube adapter disabled; not creating a broadcast.");
			return;
		}
		if (!configString(adapterConfig, QStringLiteral("refreshToken")).isEmpty()) {
			const auto refreshed = refreshAccessToken(QStringLiteral("youtube"), adapterConfig);
			if (!refreshed.ok) {
				obs_log(LOG_WARNING, "Live Gate: YouTube token refresh failed: %s",
					refreshed.error.toUtf8().constData());
				return;
			}
			adapterConfig.insert(QStringLiteral("accessToken"), refreshed.accessToken);
		}

		QString title;
		for (const auto &r : lastAcceptedRows_) {
			if (r.platform == QStringLiteral("youtube") && r.enabled) {
				title = r.title;
				break;
			}
		}

		const auto result = createYouTubeBroadcast(adapterConfig, title);
		obs_log(result.ok ? LOG_INFO : LOG_WARNING, "Live Gate: %s", result.message.toUtf8().constData());
	}

	void applyAitumSelections()
	{
		// Start every non-YouTube output now, then give YouTube the stage to
		// itself a few seconds later. Clicking the YouTube "go live" button in the
		// same instant as Kick/Twitch races its broadcast lookup: it comes back
		// "No active or pending YouTube broadcast found", the title push gives up,
		// and the broadcast sticks on "Preparing stream" with healthy ingest.
		// Staggering reproduces the known-good "start YouTube on its own" flow.
		setAitumOutputs(true, AitumPhase::NonYouTube);

		if (!hasPendingYouTube())
			return;

		QTimer::singleShot(kYouTubeStartDelayMs, this, [this] {
			// Bail if the stream was stopped during the stagger window so we do
			// not silently bring YouTube up after the user went offline.
			if (!obs_frontend_streaming_active())
				return;
			// Create + bind a fresh broadcast FIRST so it is `ready` and bound
			// before any ingest. Then clicking the Aitum YouTube output (below)
			// is the off->on ingest edge that trips autostart -> live. Doing this
			// in the reverse order is exactly what left the broadcast stuck on
			// "Preparing stream" (the edge passed before the broadcast existed).
			ensureYouTubeBroadcasts();
			setAitumOutputs(true, AitumPhase::YouTubeOnly);
			// The title is already set at broadcast creation; this remains as a
			// fallback for a pinned/pre-existing broadcast and is a no-op otherwise.
			publishPendingYouTubeTitles();
		});
	}

	// Toggle the Aitum output "go live" buttons for the outputs we selected. Used
	// to start them after OBS goes live and to stop them again when OBS stops, so
	// the user does not have to click each Aitum output by hand. The phase lets the
	// start path bring YouTube up on a separate, later pass (see applyAitumSelections).
	void setAitumOutputs(bool live, AitumPhase phase = AitumPhase::All)
	{
		if (pendingAitumIds_.empty())
			return;

		auto *mainWindow = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
		if (!mainWindow)
			return;

		const auto buttons = mainWindow->findChildren<QPushButton *>();
		for (auto *button : buttons) {
			if (!button || button->objectName() != QStringLiteral("canvasStream"))
				continue;

			QString outputName;
			for (QWidget *parent = button->parentWidget(); parent; parent = parent->parentWidget()) {
				if (qobject_cast<QGroupBox *>(parent) && !parent->objectName().isEmpty()) {
					outputName = parent->objectName();
					break;
				}
			}
			if (outputName.isEmpty())
				continue;

			const bool selected = std::find(pendingAitumIds_.begin(), pendingAitumIds_.end(), outputName) != pendingAitumIds_.end();
			if (!selected)
				continue;

			const bool youtube = isYouTubeOutput(outputName);
			if (phase == AitumPhase::NonYouTube && youtube)
				continue;
			if (phase == AitumPhase::YouTubeOnly && !youtube)
				continue;

			if (button->isChecked() != live) {
				obs_log(LOG_INFO, "Live Gate %s Aitum output '%s'", live ? "starting" : "stopping",
					outputName.toUtf8().constData());
				button->click();
			}
		}
	}

	bool showingDialog_ = false;
	bool programmaticStart_ = false;
	std::vector<QString> pendingAitumIds_;
	std::vector<PlatformRow> lastAcceptedRows_;
	std::vector<PlatformRow> pendingYouTubeRows_;
};

std::unique_ptr<LiveGateController> controller;

} // namespace

bool obs_module_load(void)
{
	QTimer::singleShot(0, [] {
		controller = std::make_unique<LiveGateController>();
		controller->install();
	});
	return true;
}

void obs_module_unload(void)
{
	if (controller) {
		controller->uninstall();
		controller.reset();
	}
	obs_log(LOG_INFO, "OBS Aitum Live Gate unloaded");
}

