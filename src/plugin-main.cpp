/*
OBS Aitum Live Gate
Copyright (C) 2026 trickeri

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
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
	if (haystack.contains(QStringLiteral("trovo")))
		return QStringLiteral("trovo");
	if (haystack.contains(QStringLiteral("facebook")) || haystack.contains(QStringLiteral("fbcdn")) ||
	    haystack.contains(QStringLiteral("fb.")))
		return QStringLiteral("facebook");
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
	const QString broadcastId = configString(config, QStringLiteral("broadcastId"));
	if (accessToken.isEmpty() || broadcastId.isEmpty())
		return {true, false, QStringLiteral("YouTube adapter needs accessToken and broadcastId.")};

	QUrl listUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
	QUrlQuery listQuery;
	listQuery.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,status,contentDetails"));
	listQuery.addQueryItem(QStringLiteral("id"), broadcastId);
	listUrl.setQuery(listQuery);

	const Headers authHeaders{{"Authorization", "Bearer " + accessToken.toUtf8()}, {"Accept", "application/json"}};
	const auto listResponse = sendRequest("GET", listUrl, authHeaders);
	if (listResponse.statusCode != 200)
		return {true, false,
			QStringLiteral("YouTube broadcast lookup failed (%1): %2")
				.arg(listResponse.statusCode)
				.arg(QString::fromUtf8(listResponse.body))};

	const auto root = jsonObjectFromResponse(listResponse);
	const auto items = root.value(QStringLiteral("items")).toArray();
	if (items.isEmpty())
		return {true, false, QStringLiteral("YouTube broadcastId was not found.")};

	QJsonObject broadcast = items.first().toObject();
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

TitlePublishResult updateFacebookTitle(const QJsonObject &config, const QString &title)
{
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	const QString liveVideoId = configString(config, QStringLiteral("liveVideoId"));
	if (accessToken.isEmpty() || liveVideoId.isEmpty())
		return {true, false, QStringLiteral("Facebook adapter needs accessToken and liveVideoId.")};

	QUrl url(QStringLiteral("https://graph.facebook.com/v25.0/%1").arg(liveVideoId));
	QUrlQuery form;
	form.addQueryItem(QStringLiteral("access_token"), accessToken);
	form.addQueryItem(QStringLiteral("title"), title);
	if (config.value(QStringLiteral("setDescriptionToo")).toBool(false))
		form.addQueryItem(QStringLiteral("description"), title);

	const auto response = sendRequest(
		"POST", url,
		{{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}},
		form.query(QUrl::FullyEncoded).toUtf8());

	if (response.statusCode >= 200 && response.statusCode < 300)
		return {true, true, QStringLiteral("Facebook LiveVideo title update requested.")};
	return {true, false,
		QStringLiteral("Facebook title update failed (%1): %2").arg(response.statusCode).arg(QString::fromUtf8(response.body))};
}

TitlePublishResult updateTrovoTitle(const QJsonObject &config, const QString &title)
{
	const QString clientId = configString(config, QStringLiteral("clientId"));
	const QString accessToken = configString(config, QStringLiteral("accessToken"));
	const QString channelId = configString(config, QStringLiteral("channelId"));
	if (clientId.isEmpty() || accessToken.isEmpty() || channelId.isEmpty())
		return {true, false, QStringLiteral("Trovo adapter needs clientId, accessToken, and channelId.")};

	const QJsonObject payload{{QStringLiteral("command"), QStringLiteral("settitle %1").arg(title)},
				  {QStringLiteral("channel_id"), channelId.toLongLong()}};
	const auto response = sendRequest(
		"POST", QUrl(QStringLiteral("https://open-api.trovo.live/openplatform/channels/command")),
		{{"Authorization", "OAuth " + accessToken.toUtf8()},
		 {"Client-ID", clientId.toUtf8()},
		 {"Accept", "application/json"},
		 {"Content-Type", "application/json"}},
		QJsonDocument(payload).toJson(QJsonDocument::Compact));

	if (response.statusCode >= 200 && response.statusCode < 300)
		return {true, true, QStringLiteral("Trovo settitle command sent.")};
	return {true, false,
		QStringLiteral("Trovo title update failed (%1): %2").arg(response.statusCode).arg(QString::fromUtf8(response.body))};
}

TitlePublishResult publishPlatformTitle(const QString &platform, const QJsonObject &adapterConfig, const QString &title)
{
	if (platform == QStringLiteral("twitch"))
		return updateTwitchTitle(adapterConfig, title);
	if (platform == QStringLiteral("youtube"))
		return updateYouTubeTitle(adapterConfig, title);
	if (platform == QStringLiteral("kick"))
		return updateKickTitle(adapterConfig, title);
	if (platform == QStringLiteral("trovo"))
		return updateTrovoTitle(adapterConfig, title);
	if (platform == QStringLiteral("facebook"))
		return updateFacebookTitle(adapterConfig, title);
	if (platform == QStringLiteral("tiktok"))
		return {false, true, QStringLiteral("TikTok title updates are not implemented because no normal public LIVE title API was found.")};
	return {false, true, QStringLiteral("No title adapter for platform '%1'.").arg(platform)};
}

class GoLiveDialog final : public QDialog {
public:
	explicit GoLiveDialog(std::vector<PlatformRow> rows, QWidget *parent = nullptr) : QDialog(parent), rows_(std::move(rows))
	{
		setWindowTitle(text("LiveGate.Title"));
		setModal(true);
		resize(720, 240);

		auto *root = new QVBoxLayout(this);
		auto *intro = new QLabel(text("LiveGate.Intro"), this);
		intro->setWordWrap(true);
		root->addWidget(intro);

		auto *grid = new QGridLayout;
		grid->setColumnStretch(2, 1);
		grid->addWidget(new QLabel(text("LiveGate.Enabled"), this), 0, 0);
		grid->addWidget(new QLabel(text("LiveGate.Platform"), this), 0, 1);
		grid->addWidget(new QLabel(text("LiveGate.StreamTitle"), this), 0, 2);

		int rowNumber = 1;
		for (auto &row : rows_) {
			auto *enabled = new QCheckBox(this);
			enabled->setChecked(row.enabled);
			enabledBoxes_.push_back(enabled);
			grid->addWidget(enabled, rowNumber, 0, Qt::AlignHCenter);

			QString label = row.name;
			const QString endpoint = displayEndpoint(row.endpoint);
			if (!endpoint.isEmpty())
				label += QStringLiteral(" (%1)").arg(endpoint);
			grid->addWidget(new QLabel(label, this), rowNumber, 1);

			auto *title = new QLineEdit(row.title, this);
			title->setPlaceholderText(QStringLiteral("Tonight's stream title"));
			titleEdits_.push_back(title);
			grid->addWidget(title, rowNumber, 2);
			rowNumber++;
		}

		root->addLayout(grid);

		if (rows_.size() <= 1) {
			auto *hint = new QLabel(text("LiveGate.NoAitumOutputs"), this);
			hint->setWordWrap(true);
			root->addWidget(hint);
		}

		auto *buttons = new QDialogButtonBox(this);
		buttons->addButton(text("LiveGate.GoLive"), QDialogButtonBox::AcceptRole);
		buttons->addButton(text("LiveGate.Cancel"), QDialogButtonBox::RejectRole);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		root->addWidget(buttons);
	}

	std::vector<PlatformRow> rows() const
	{
		auto updated = rows_;
		for (size_t i = 0; i < updated.size(); i++) {
			updated[i].enabled = enabledBoxes_[i]->isChecked();
			updated[i].title = titleEdits_[i]->text().trimmed();
		}
		return updated;
	}

private:
	std::vector<PlatformRow> rows_;
	std::vector<QCheckBox *> enabledBoxes_;
	std::vector<QLineEdit *> titleEdits_;
};

class LiveGateController final : public QObject {
public:
	explicit LiveGateController(QObject *parent = nullptr) : QObject(parent) {}

	void install()
	{
		if (qApp)
			qApp->installEventFilter(this);
		obs_frontend_add_event_callback(&LiveGateController::frontendEvent, this);
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
			QTimer::singleShot(1200, self, [self] { self->applyAitumSelections(); });
			self->programmaticStart_ = false;
		} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED || event == OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
			self->pendingAitumIds_.clear();
			self->programmaticStart_ = false;
		}
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
		if (dialog.exec() != QDialog::Accepted)
			return;

		lastAcceptedRows_ = dialog.rows();
		saveRows(lastAcceptedRows_);

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
		if (!titleWarnings.isEmpty()) {
			QMessageBox::warning(mainWindow, text("LiveGate.TitleUpdateWarningTitle"),
					     text("LiveGate.TitleUpdateWarningText") + QStringLiteral("\n\n") +
						     titleWarnings.join(QStringLiteral("\n")));
		}
		if (!titleUpdatesOk) {
			obs_log(LOG_WARNING, "Live Gate aborted stream because strict title updates failed");
			return;
		}

		programmaticStart_ = true;
		obs_frontend_streaming_start();
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
		if (object.contains(QStringLiteral("enabled")))
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

	bool publishTitles(QStringList &warnings) const
	{
		const QJsonObject root = readJsonObject(settingsPath());
		const QJsonObject adapters = root.value(QStringLiteral("titleAdapters")).toObject();
		const bool strict = adapters.value(QStringLiteral("strictTitleUpdates")).toBool(false);
		bool ok = true;

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

			const QJsonObject adapterConfig = adapters.value(row.platform).toObject();
			if (!adapterConfig.value(QStringLiteral("enabled")).toBool(false)) {
				const QString message =
					QStringLiteral("%1: %2 adapter is not enabled; title not updated.").arg(row.name, row.platform);
				warnings.push_back(message);
				obs_log(LOG_WARNING, "%s", message.toUtf8().constData());
				if (strict)
					ok = false;
				continue;
			}

			const auto result = publishPlatformTitle(row.platform, adapterConfig, row.title);
			obs_log(result.success ? LOG_INFO : LOG_WARNING, "%s", result.message.toUtf8().constData());
			if (!result.success) {
				warnings.push_back(QStringLiteral("%1: %2").arg(row.name, result.message));
				if (strict)
					ok = false;
			}
		}

		return ok;
	}

	void applyAitumSelections()
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
			if (selected && !button->isChecked()) {
				obs_log(LOG_INFO, "Live Gate starting Aitum output '%s'", outputName.toUtf8().constData());
				button->click();
			}
		}
	}

	bool showingDialog_ = false;
	bool programmaticStart_ = false;
	std::vector<QString> pendingAitumIds_;
	std::vector<PlatformRow> lastAcceptedRows_;
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

