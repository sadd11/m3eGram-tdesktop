/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "plugins/plugins_runtime.h"

#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QVariantMap>

namespace Plugins {

struct PluginMeta {
	QString id;
	QString name;
	QString description;
	QString author;
	QString version;
	QString icon;
};

// m3eGram: host-side state of one loaded plugin. Injects the plugin API
// (Plugin / ui / client / http / storage / console, an exteraGram-style
// surface) into its runtime and translates the calls into app actions.
// Plugin settings and kv storage are owned here and persisted by the
// manager as a single state.json.
class Bridge final {
public:
	Bridge(Runtime *runtime, QString pluginId);
	~Bridge();

	// Fill the persisted settings/storage before the plugin file is run.
	void setState(QJsonObject settings, QJsonObject storage);

	// Define the globals; must be called before eval().
	void install();

	// Read back what the plugin registered while evaluating.
	[[nodiscard]] QVariant registeredInfo() const;

	[[nodiscard]] const QString &pluginId() const;
	[[nodiscard]] bool sendHookEnabled() const;

	// Plugin settings (persisted, exposed through getSetting/setSetting).
	[[nodiscard]] QVariant settingsGet(
		const QString &key,
		const QVariant &fallback) const;
	void settingsSet(const QString &key, const QVariant &value);
	[[nodiscard]] QJsonObject settingsJson() const;

	// Arbitrary kv storage (persisted alongside the settings).
	[[nodiscard]] QVariant storageGet(
		const QString &key,
		const QVariant &fallback) const;
	void storageSet(const QString &key, const QVariant &value);
	[[nodiscard]] QJsonObject storageJson() const;

	// Complete a promise started by an async API call (http / dialogs).
	void resolveAsync(const QString &id, const QVariant &result);
	void rejectAsync(const QString &id, const QString &error);

private:
	static JSValue _fnHostCall(
		JSContext *ctx,
		JSValueConst thisVal,
		int argc,
		JSValueConst *argv);

	[[nodiscard]] QVariant hostCall(
		const QString &method,
		const QVariantMap &args,
		QString *error);

	Runtime *_runtime = nullptr;
	QString _pluginId;
	PluginMeta _meta;
	bool _registered = false;
	bool _sendHook = false;
	QJsonObject _settings;
	QJsonObject _storage;
	QMap<QString, std::pair<JSValue, JSValue>> _pending; // id -> resolve/reject

};

} // namespace Plugins
