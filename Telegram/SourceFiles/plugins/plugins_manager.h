/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "plugins/plugins_bridge.h"
#include "rpl/rpl.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

class QNetworkAccessManager;

class HistoryItem;

namespace Main {
class Session;
class Account;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Api {
struct MessageToSend;
} // namespace Api

namespace Plugins {

struct PluginState {
	bool enabled = false;
	QJsonObject settings;
	QJsonObject storage;
};

struct PluginDescriptor {
	QString fileName;
	PluginMeta meta;
	PluginState state;
	bool loadFailed = false;
	QString error;
	bool hasInstance = false;
};

// m3eGram: user plugin system (a QuickJS-based port of the exteraGram
// plugin concept). Plugins live in tdata/plugins/*.js, are scanned at
// startup, and can hook outgoing/incoming messages, define settings pages
// and reach a small sandboxed API (ui / client / http / storage / console).
class Manager final {
public:
	static Manager &Instance();

	// Called once from Core::Application::run().
	void init();

	[[nodiscard]] const std::vector<PluginDescriptor> &descriptors() const {
		return _descriptors;
	}
	[[nodiscard]] QString pluginsFolder() const;

	void setEnabled(const QString &fileName, bool enabled);
	void reload();

	// Settings UI: list state changed (enabled / errors / set of plugins).
	[[nodiscard]] rpl::producer<> changed() const;

	// Persisted plugin settings access for the settings section.
	[[nodiscard]] QVariant settingValue(
		const QString &fileName,
		const QString &key,
		const QVariant &fallback) const;
	void settingChanged(
		const QString &fileName,
		const QString &key,
		const QVariant &value);
	void fireCallback(
		const QString &fileName,
		const QString &callbackId,
		const QVariant &value);
	[[nodiscard]] bool isLoaded(const QString &fileName) const;

	// The createSettings() rows of a plugin, for the settings section.
	// Uses the live runtime when loaded (so the row callbacks reference
	// the right context), a temporary one otherwise.
	[[nodiscard]] QVariantList settingsRows(const QString &fileName);

	// Bridge dispatch (ui.* / client.* / http.* / console.*).
	QVariant hostCallFromPlugin(
		const QString &pluginId,
		const QString &method,
		const QVariantMap &args,
		QString *error);

	// Outgoing message hook, called from ApiWrap::sendMessage. Returns
	// false when a plugin cancelled the send, possibly updates the text.
	[[nodiscard]] bool processOutgoingMessage(
		not_null<Main::Session*> session,
		Api::MessageToSend &message);

private:
	struct Loaded {
		QString fileName;
		QString id;
		std::unique_ptr<Runtime> runtime;
		std::unique_ptr<Bridge> bridge;
	};
	struct AccountSub {
		rpl::lifetime outer;
		rpl::lifetime inner;
	};

	Manager();

	[[nodiscard]] PluginDescriptor *findDescriptor(const QString &fileName);
	[[nodiscard]] Loaded *findLoaded(const QString &pluginId);
	void ensureFolder();
	void writeExample();
	void loadState();
	void saveState();
	void scan();
	void loadOne(PluginDescriptor &descriptor);
	void unloadOne(const QString &fileName);
	void unloadAll();
	void loadEnabled();
	void subscribeAccounts();
	void subscribeAccount(int index, not_null<Main::Account*> account);
	void onNewMessage(
		not_null<Main::Session*> session,
		int accountIndex,
		not_null<HistoryItem*> item);
	[[nodiscard]] int indexOfAccount(not_null<Main::Account*> account) const;
	void recordError(const QString &fileName, const QString &error);
	void resolveForPlugin(
		const QString &pluginId,
		const QString &asyncId,
		const QVariant &result);
	void rejectForPlugin(
		const QString &pluginId,
		const QString &asyncId,
		const QString &error);
	[[nodiscard]] bool beginAsyncForPlugin(
		const QString &pluginId,
		QString &asyncId);

	std::vector<PluginDescriptor> _descriptors;
	std::vector<Loaded> _loaded;
	std::map<QString, PluginState> _state;
	std::vector<QString> _logLines;
	QString _folder;
	rpl::event_stream<> _changed;
	std::unordered_map<Main::Account*, std::shared_ptr<AccountSub>> _accountSubs;
	rpl::lifetime _lifetime;
	std::unique_ptr<QNetworkAccessManager> _nam;
	int _asyncCounter = 0;
	bool _inited = false;

};

// Entry point for ApiWrap::sendMessage (see plugins_manager.cpp).
[[nodiscard]] bool ProcessOutgoingMessage(
	not_null<Main::Session*> session,
	Api::MessageToSend &message);

// Called once from Core::Application::run().
void Init();

// Opens the .py -> .js conversion dialog (exteraGram plugin converter).
void RunConverter(not_null<Window::SessionController*> controller);

} // namespace Plugins
