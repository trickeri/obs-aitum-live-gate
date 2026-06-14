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
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
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
	QString name;
	QString endpoint;
	QString title;
	bool enabled = false;
	bool obsMain = false;
};

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
		} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING) {
			self->publishTitles();
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

	void publishTitles() const
	{
		for (const auto &row : lastAcceptedRows_) {
			if (!row.enabled || row.title.isEmpty())
				continue;
			obs_log(LOG_INFO, "Live Gate title queued for %s '%s': %s", row.source.toUtf8().constData(),
				row.name.toUtf8().constData(), row.title.toUtf8().constData());
		}
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

