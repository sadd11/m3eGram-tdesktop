/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_manager.h"

#include "apiwrap.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Plugins {
namespace {

constexpr auto kExampleFileName = "example.js";
constexpr auto kStateFileName = "state.json";
constexpr auto kLogLinesLimit = 500;

// m3eGram: bot-api style signed peer id -> tdesktop PeerId
// (>= 0: user, <= -1000000000000: channel, otherwise: chat).
[[nodiscard]] PeerId PeerIdFromBotApi(qint64 id) {
	if (id >= 0) {
		return peerFromUser(BareId(id));
	} else if (id <= -1000000000000LL) {
		return peerFromChannel(BareId(-id - 1000000000000LL));
	}
	return peerFromChat(BareId(-id));
}

[[nodiscard]] QString PeerTypeOf(not_null<PeerData*> peer) {
	return peer->isUser()
		? u"user"_q
		: peer->isChannel()
		? u"channel"_q
		: u"chat"_q;
}

} // namespace

Manager::Manager() = default;

Manager &Manager::Instance() {
	static auto instance = Manager();
	return instance;
}

QString Manager::pluginsFolder() const {
	return _folder;
}

void Manager::init() {
	if (_inited) {
		return;
	}
	_inited = true;
	_nam = std::make_unique<QNetworkAccessManager>();
	ensureFolder();
	writeExample();
	loadState();
	scan();
	loadEnabled();
	subscribeAccounts();
}

void Manager::ensureFolder() {
	_folder = cWorkingDir() + u"tdata/plugins"_q;
	QDir().mkpath(_folder);
}

void Manager::writeExample() {
	const QDir dir(_folder);
	if (!dir.entryList(QStringList(u"*.js"_q), QDir::Files).isEmpty()) {
		return;
	}
	QFile file(dir.filePath(kExampleFileName));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	file.write(R"JS(
// m3eGram example plugin: a port of the exteraGram "Hello World" plugin.
// Rewrites ".hello <name>" into a friendly message.
"use strict";

const DEFAULT_TEMPLATE = "Hello, {name}! 👋";

class HelloWorldPlugin extends Plugin.Base {
	onLoad() {
		this.addOnSendMessageHook();
		console.log("Hello World plugin loaded");
	}

	onSendMessageHook(params) {
		const text = (params.text || "").trim();
		if (!text.startsWith(".hello")) {
			return HookResult();
		}
		let name = text.split(" ").slice(1).join(" ").trim();
		const template = String(this.getSetting("template", DEFAULT_TEMPLATE));
		if (!name) {
			return HookResult({
				strategy: HookStrategy.Modify,
				params: { text: "Usage: .hello <name>" },
			});
		}
		let result = "";
		try {
			result = template.replace("{name}", name);
		} catch (e) {
			result = "Template must contain a {name} placeholder.";
		}
		return HookResult({
			strategy: HookStrategy.Modify,
			params: { text: result },
		});
	}

	createSettings() {
		return [
			{ type: "header", text: "Hello World" },
			{
				type: "input",
				key: "template",
				text: "Message template",
				default: DEFAULT_TEMPLATE,
				subtext: "Use {name} for the name",
			},
		];
	}
}

Plugin.register({
	id: "hello_world",
	name: "Hello World",
	description: "Rewrites .hello commands into a friendly message",
	author: "m3eGram",
	version: "1.0.0",
}, new HelloWorldPlugin());
)JS");
}

void Manager::loadState() {
	QFile file(QDir(_folder).filePath(kStateFileName));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	const auto root = doc.object();
	for (auto it = root.begin(); it != root.end(); ++it) {
		const auto value = it->toObject();
		auto state = PluginState();
		state.enabled = value.value(u"enabled"_q).toBool();
		state.settings = value.value(u"settings"_q).toObject();
		state.storage = value.value(u"storage"_q).toObject();
		_state.insert(it.key(), std::move(state));
	}
}

void Manager::saveState() {
	auto root = QJsonObject();
	for (const auto &descriptor : _descriptors) {
		auto value = QJsonObject();
		value.insert(u"enabled"_q, descriptor.state.enabled);
		value.insert(u"settings"_q, descriptor.state.settings);
		value.insert(u"storage"_q, descriptor.state.storage);
		root.insert(descriptor.fileName, value);
	}
	QFile file(QDir(_folder).filePath(kStateFileName));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}

void Manager::scan() {
	unloadAll();
	_descriptors.clear();
	const QDir dir(_folder);
	const auto files = dir.entryList(
		QStringList(u"*.js"_q),
		QDir::Files,
		QDir::Name);
	for (const auto &fileName : files) {
		auto descriptor = PluginDescriptor();
		descriptor.fileName = fileName;
		if (const auto it = _state.find(fileName); it != _state.end()) {
			descriptor.state = it->second;
		} else if (fileName == kExampleFileName) {
			// The bundled example starts enabled on the very first run so
			// the feature is visible; it can be disabled any time.
			descriptor.state.enabled = true;
		}

		auto runtime = std::make_unique<Runtime>();
		auto bridge = std::make_unique<Bridge>(runtime.get(), u"scan"_q);
		bridge->setState(descriptor.state.settings, descriptor.state.storage);
		bridge->install();
		QFile file(dir.filePath(fileName));
		if (file.open(QIODevice::ReadOnly)) {
			const auto code = file.readAll();
			const DispatchGuard guard(runtime.get());
			if (!runtime->eval(code, fileName)) {
				descriptor.loadFailed = true;
				descriptor.error = runtime->lastError();
			}
		} else {
			descriptor.loadFailed = true;
			descriptor.error = u"Cannot read file"_q;
		}
		if (!descriptor.loadFailed) {
			auto info = bridge->registeredInfo();
			if (info.typeId() == QMetaType::QJsonObject) {
				info = info.toJsonObject().toVariantMap();
			}
			const auto map = info.toMap();
			const auto meta = map.value(u"meta"_q).toMap();
			descriptor.meta.id = meta.value(u"id"_q).toString();
			descriptor.meta.name = meta.value(u"name"_q).toString();
			descriptor.meta.description = meta.value(u"description"_q).toString();
			descriptor.meta.author = meta.value(u"author"_q).toString();
			descriptor.meta.version = meta.value(u"version"_q).toString();
			descriptor.meta.icon = meta.value(u"icon"_q).toString();
			if (descriptor.meta.id.isEmpty()) {
				descriptor.meta.id = fileName.chopped(3);
			}
			if (descriptor.meta.name.isEmpty()) {
				descriptor.meta.name = descriptor.meta.id;
			}
			descriptor.hasInstance = map.value(u"hasInstance"_q).toBool();
		}
		_descriptors.push_back(std::move(descriptor));
	}
}

void Manager::loadOne(PluginDescriptor &descriptor) {
	unloadOne(descriptor.fileName);
	if (!descriptor.hasInstance) {
		// Nothing to load (registration only, e.g. metadata-only files).
		return;
	}
	auto runtime = std::make_unique<Runtime>();
	auto bridge = std::make_unique<Bridge>(
		runtime.get(),
		descriptor.meta.id);
	bridge->setState(descriptor.state.settings, descriptor.state.storage);
	bridge->install();
	QFile file(QDir(_folder).filePath(descriptor.fileName));
	auto failed = false;
	if (file.open(QIODevice::ReadOnly)) {
		const auto code = file.readAll();
		const DispatchGuard guard(runtime.get());
		failed = !runtime->eval(code, descriptor.fileName);
		if (!failed) {
			runtime->callGlobal(
				u"Plugin._callInstance"_q,
				{ u"onLoad"_q, QVariant() });
			if (!runtime->lastError().isEmpty()) {
				failed = true;
			}
		}
	} else {
		failed = true;
	}
	if (failed) {
		descriptor.loadFailed = true;
		descriptor.error = runtime->lastError();
		recordError(descriptor.fileName, descriptor.error);
		return;
	}
	descriptor.loadFailed = false;
	descriptor.error = QString();
	{
		const DispatchGuard guard(runtime.get());
		runtime->callGlobal(
			u"Plugin._callInstance"_q,
			{ u"onAppEvent"_q, u"start"_q });
		if (!runtime->lastError().isEmpty()) {
			recordError(descriptor.fileName, runtime->lastError());
		}
	}
	_loaded.push_back(Loaded{
		descriptor.fileName,
		descriptor.meta.id,
		std::move(runtime),
		std::move(bridge) });
}

void Manager::unloadOne(const QString &fileName) {
	auto it = _loaded.begin();
	while (it != _loaded.end()) {
		if (it->fileName == fileName) {
			it = _loaded.erase(it);
		} else {
			++it;
		}
	}
	if (const auto descriptor = findDescriptor(fileName)) {
		descriptor->loadFailed = false;
		descriptor->error = QString();
	}
}

void Manager::unloadAll() {
	_loaded.clear();
}

void Manager::loadEnabled() {
	for (auto &descriptor : _descriptors) {
		if (descriptor.state.enabled) {
			loadOne(descriptor);
		}
	}
}

void Manager::setEnabled(const QString &fileName, bool enabled) {
	const auto descriptor = findDescriptor(fileName);
	if (!descriptor || descriptor->state.enabled == enabled) {
		return;
	}
	descriptor->state.enabled = enabled;
	saveState();
	if (enabled) {
		loadOne(*descriptor);
	} else {
		unloadOne(fileName);
	}
	_changed.fire({});
}

void Manager::reload() {
	unloadAll();
	loadState();
	scan();
	saveState();
	loadEnabled();
	_changed.fire({});
}

rpl::producer<> Manager::changed() const {
	return _changed.events();
}

QVariant Manager::settingValue(
		const QString &fileName,
		const QString &key,
		const QVariant &fallback) const {
	for (const auto &descriptor : _descriptors) {
		if (descriptor.fileName == fileName) {
			const auto it = descriptor.state.settings.constFind(key);
			return (it != descriptor.state.settings.constEnd())
				? it->toVariant()
				: fallback;
		}
	}
	return fallback;
}

void Manager::settingChanged(
		const QString &fileName,
		const QString &key,
		const QVariant &value) {
	const auto descriptor = findDescriptor(fileName);
	if (!descriptor) {
		return;
	}
	descriptor->state.settings.insert(key, QJsonValue::fromVariant(value));
	saveState();
	for (auto &loaded : _loaded) {
		if (loaded.fileName == fileName) {
			loaded.bridge->settingsSet(key, value);
			break;
		}
	}
}

void Manager::fireCallback(
		const QString &fileName,
		const QString &callbackId,
		const QVariant &value) {
	for (auto &loaded : _loaded) {
		if (loaded.fileName != fileName) {
			continue;
		}
		const DispatchGuard guard(loaded.runtime.get());
		bool ok = false;
		loaded.runtime->callGlobal(
			u"Plugin._fireCallback"_q,
			{ callbackId, value },
			&ok);
		if (!ok) {
			recordError(loaded.fileName, loaded.runtime->lastError());
		}
		break;
	}
}

bool Manager::isLoaded(const QString &fileName) const {
	for (const auto &loaded : _loaded) {
		if (loaded.fileName == fileName) {
			return true;
		}
	}
	return false;
}

QVariantList Manager::settingsRows(const QString &fileName) {
	const auto descriptor = findDescriptor(fileName);
	if (!descriptor || !descriptor->hasInstance) {
		return {};
	}
	// Use the live runtime when loaded, so that the callback ids of the
	// rows reference the same context the plugin instance lives in.
	for (auto &loaded : _loaded) {
		if (loaded.fileName != fileName) {
			continue;
		}
		const DispatchGuard guard(loaded.runtime.get());
		bool ok = false;
		const auto rows = loaded.runtime->callGlobal(
			u"Plugin._settings"_q,
			{},
			&ok);
		return (ok && rows.typeId() == QMetaType::QVariantList)
			? rows.toList()
			: QVariantList();
	}
	auto runtime = std::make_unique<Runtime>();
	auto bridge = std::make_unique<Bridge>(
		runtime.get(),
		descriptor->meta.id);
	bridge->setState(descriptor->state.settings, descriptor->state.storage);
	bridge->install();
	QFile file(QDir(_folder).filePath(fileName));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const DispatchGuard guard(runtime.get());
	if (!runtime->eval(file.readAll(), fileName)) {
		return {};
	}
	bool ok = false;
	const auto rows = runtime->callGlobal(u"Plugin._settings"_q, {}, &ok);
	return (ok && rows.typeId() == QMetaType::QVariantList)
		? rows.toList()
		: QVariantList();
}

PluginDescriptor *Manager::findDescriptor(const QString &fileName) {
	for (auto &descriptor : _descriptors) {
		if (descriptor.fileName == fileName) {
			return &descriptor;
		}
	}
	return nullptr;
}

Manager::Loaded *Manager::findLoaded(const QString &pluginId) {
	for (auto &loaded : _loaded) {
		if (loaded.id == pluginId) {
			return &loaded;
		}
	}
	return nullptr;
}

void Manager::recordError(const QString &fileName, const QString &error) {
	qInfo() << "[plugins]" << fileName << ":" << error;
	if (const auto descriptor = findDescriptor(fileName)) {
		descriptor->loadFailed = true;
		descriptor->error = error;
	}
}

void Manager::subscribeAccounts() {
	const auto domain = &Core::App().domain();
	for (const auto &withIndex : domain->accounts()) {
		subscribeAccount(withIndex.index, withIndex.account.get());
	}
	domain->accountsChanges(
	) | rpl::start_with_next([=] {
		for (const auto &withIndex : domain->accounts()) {
			subscribeAccount(withIndex.index, withIndex.account.get());
		}
	}, _lifetime);
}

void Manager::subscribeAccount(int index, not_null<Main::Account*> account) {
	if (_accountSubs.contains(account.get())) {
		return;
	}
	// Kept alive by the shared_ptr captured into the subscription itself.
	const auto sub = std::make_shared<AccountSub>();
	account->sessionValue(
	) | rpl::start_with_next([=](Main::Session *session) {
		sub->inner.destroy();
		if (!session) {
			return;
		}
		session->changes().messageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::start_with_next([=](const Data::MessageUpdate &update) {
			onNewMessage(session, index, update.item);
		}, sub->inner);
	}, sub->outer);
	_accountSubs.emplace(account.get(), sub);
}

void Manager::onNewMessage(
		not_null<Main::Session*> session,
		int accountIndex,
		not_null<HistoryItem*> item) {
	if (!_inited || _loaded.empty()) {
		return;
	}
	if (item->isService()) {
		return;
	}
	const auto peer = item->history()->peer;
	const auto sender = item->from();
	const auto params = QVariantMap{
		{ u"account"_q, accountIndex },
		{ u"msgId"_q, double(item->id.bare) },
		{ u"peerId"_q, double(peer->id.value) },
		{ u"peerType"_q, PeerTypeOf(peer) },
		{ u"senderId"_q, double(sender->id.value) },
		{ u"text"_q, item->originalText().text },
		{ u"date"_q, double(item->date()) },
		{ u"out"_q, item->out() },
	};
	for (auto &loaded : _loaded) {
		const DispatchGuard guard(loaded.runtime.get());
		bool ok = false;
		loaded.runtime->callGlobal(
			u"Plugin._callInstance"_q,
			{ u"onMessageReceived"_q, params },
			&ok);
		if (!ok) {
			recordError(loaded.fileName, loaded.runtime->lastError());
		} else if (guard.interrupted()) {
			recordError(loaded.fileName, u"onMessageReceived timed out"_q);
		}
	}
}

int Manager::indexOfAccount(not_null<Main::Account*> account) const {
	for (const auto &withIndex : Core::App().domain().accounts()) {
		if (withIndex.account.get() == account.get()) {
			return withIndex.index;
		}
	}
	return 0;
}

void Manager::resolveForPlugin(
		const QString &pluginId,
		const QString &asyncId,
		const QVariant &result) {
	if (const auto loaded = findLoaded(pluginId)) {
		loaded->bridge->resolveAsync(asyncId, result);
	}
}

void Manager::rejectForPlugin(
		const QString &pluginId,
		const QString &asyncId,
		const QString &error) {
	if (const auto loaded = findLoaded(pluginId)) {
		loaded->bridge->rejectAsync(asyncId, error);
	}
}

bool Manager::beginAsyncForPlugin(
		const QString &pluginId,
		QString &asyncId) {
	if (!findLoaded(pluginId)) {
		return false;
	}
	asyncId = u"a%1"_q.arg(++_asyncCounter);
	return true;
}

QVariant Manager::hostCallFromPlugin(
		const QString &pluginId,
		const QString &method,
		const QVariantMap &args,
		QString *error) {
	const auto fail = [&](const QString &message) {
		if (error) {
			*error = message;
		}
		return QVariant();
	};
	if (method == u"ui.showToast") {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return fail(u"No active window"_q);
		}
		controller->showToast(args.value(u"text"_q).toString());
		return QVariant();
	} else if (method == u"ui.showAlert") {
		const auto window = Core::App().activeWindow();
		if (!window) {
			return fail(u"No active window"_q);
		}
		window->show(Ui::MakeInformBox({
			.text = args.value(u"text"_q).toString(),
			.title = args.value(u"title"_q).toString(),
		}));
		return QVariant();
	} else if (method == u"ui.showConfirm") {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return fail(u"No active window"_q);
		}
		auto asyncId = QString();
		if (!beginAsyncForPlugin(pluginId, asyncId)) {
			return fail(u"Plugin is not loaded"_q);
		}
		const auto weak = base::make_weak(controller);
		const auto confirmText = args.value(u"confirmText"_q).toString();
		const auto cancelText = args.value(u"cancelText"_q).toString();
		controller->show(Ui::MakeConfirmBox({
			.text = args.value(u"text"_q).toString(),
			.title = args.value(u"title"_q).toString(),
			.confirmText = confirmText,
			.cancelText = cancelText,
			.confirmed = [=](Fn<void()> close) {
				close();
				resolveForPlugin(pluginId, asyncId, true);
			},
			.cancelled = [=] {
				resolveForPlugin(pluginId, asyncId, false);
			},
		}));
		return QVariantMap{ { u"__async"_q, asyncId } };
	} else if (method == u"client.account") {
		const auto &domain = Core::App().domain();
		return indexOfAccount(domain.active());
	} else if (method == u"client.sendToCurrentChat") {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return fail(u"No active window"_q);
		}
		const auto history = controller->activeChatCurrent().history();
		if (!history) {
			return fail(u"No active chat"_q);
		}
		auto to = Api::MessageToSend(Api::SendAction(history));
		to.textWithTags = TextWithTags{
			args.value(u"text"_q).toString(),
			TextWithTags::Tags()
		};
		to.action.clearDraft = false;
		history->session().api().sendMessage(std::move(to));
		return QVariant();
	} else if (method == u"client.sendText") {
		const auto &domain = Core::App().domain();
		const auto session = domain.active().maybeSession();
		if (!session) {
			return fail(u"No session"_q);
		}
		const auto peerId = PeerIdFromBotApi(
			args.value(u"peerId"_q).toLongLong());
		const auto peer = session->data().peerLoaded(peerId);
		if (!peer) {
			return fail(u"Peer not found"_q);
		}
		const auto history = peer->history();
		auto to = Api::MessageToSend(Api::SendAction(history));
		to.textWithTags = TextWithTags{
			args.value(u"text"_q).toString(),
			TextWithTags::Tags()
		};
		to.action.clearDraft = false;
		session->api().sendMessage(std::move(to));
		return QVariant();
	} else if (method == u"http.get" || method == u"http.post") {
		auto asyncId = QString();
		if (!beginAsyncForPlugin(pluginId, asyncId)) {
			return fail(u"Plugin is not loaded"_q);
		}
		const auto url = args.value(u"url"_q).toString();
		auto request = QNetworkRequest(QUrl(url));
		const auto headers = args.value(u"headers"_q).toMap();
		for (auto it = headers.begin(); it != headers.end(); ++it) {
			request.setRawHeader(
				it.key().toUtf8(),
				it.value().toString().toUtf8());
		}
		auto *reply = (method == u"http.get")
			? _nam->get(request)
			: [&] {
				const auto json = args.value(u"json"_q);
				auto body = QByteArray();
				if (!json.isNull() && !QJsonObject::fromVariant(json).isEmpty()) {
					request.setHeader(
						QNetworkRequest::ContentTypeHeader,
						u"application/json"_q);
					body = QJsonDocument(
						QJsonObject::fromVariant(json)).toJson(
							QJsonDocument::Compact);
				} else {
					request.setHeader(
						QNetworkRequest::ContentTypeHeader,
						u"text/plain; charset=utf-8"_q);
					body = args.value(u"body"_q).toString().toUtf8();
				}
				return _nam->post(request, body);
			}();
		QObject::connect(
			reply,
			&QNetworkReply::finished,
			[=, this] {
				const auto status = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				const auto body = QString::fromUtf8(reply->readAll());
				const auto err = reply->error();
				const auto errText = reply->errorString();
				reply->deleteLater();
				if (status == 0 && err != QNetworkReply::NoError) {
					rejectForPlugin(pluginId, asyncId, errText);
					return;
				}
				resolveForPlugin(pluginId, asyncId, QVariantMap{
					{ u"status"_q, status },
					{ u"body"_q, body },
				});
			});
		return QVariantMap{ { u"__async"_q, asyncId } };
	} else if (method == u"console.log") {
		const auto level = args.value(u"level"_q).toString();
		const auto text = args.value(u"text"_q).toString();
		_logLines.push_back(u"[%1] %2"_q.arg(pluginId, text));
		if (_logLines.size() > kLogLinesLimit) {
			_logLines.erase(_logLines.begin());
		}
		if (level == u"error") {
			qWarning() << "[plugins]" << pluginId << ":" << text;
		} else {
			qInfo() << "[plugins]" << pluginId << ":" << text;
		}
		return QVariant();
	}
	return fail(u"Unknown method: "_q + method);
}

bool Manager::processOutgoingMessage(
		not_null<Main::Session*> session,
		Api::MessageToSend &message) {
	if (!_inited || _loaded.empty()) {
		return true;
	}
	const auto accountIndex = indexOfAccount(session->account());
	const auto peer = message.action.history->peer;
	auto text = message.textWithTags.text;
	auto cancelled = false;
	auto modified = false;
	for (auto &loaded : _loaded) {
		if (!loaded.bridge->sendHookEnabled()) {
			continue;
		}
		const auto params = QVariantMap{
			{ u"account"_q, accountIndex },
			{ u"text"_q, text },
			{ u"peerId"_q, double(peer->id.value) },
			{ u"peerType"_q, PeerTypeOf(peer) },
			{ u"replyTo"_q, double(
				message.action.replyTo.messageId
					? message.action.replyTo.messageId.bare
					: 0) },
		};
		const DispatchGuard guard(loaded.runtime.get());
		bool ok = false;
		const auto result = loaded.runtime->callGlobal(
			u"Plugin._callInstance"_q,
			{ u"onSendMessageHook"_q, params },
			&ok);
		if (!ok) {
			recordError(loaded.fileName, loaded.runtime->lastError());
			continue;
		}
		if (guard.interrupted()) {
			recordError(loaded.fileName, u"onSendMessageHook timed out"_q);
			continue;
		}
		const auto map = result.typeId() == QMetaType::QJsonObject
			? result.toJsonObject().toVariantMap()
			: result.toMap();
		const auto strategy = map.value(u"strategy"_q).toInt();
		if (strategy == 1 /* HookStrategy.Cancel */) {
			cancelled = true;
			break;
		} else if (strategy == 2 || strategy == 3 /* Modify / Final */) {
			const auto newText = map
				.value(u"params"_q)
				.toMap()
				.value(u"text"_q)
				.toString();
			if (!newText.isEmpty() && newText != text) {
				text = newText;
				modified = true;
			}
			if (strategy == 3) {
				break;
			}
		}
	}
	if (cancelled) {
		return false;
	}
	if (modified) {
		message.textWithTags.text = text;
	}
	return true;
}

bool ProcessOutgoingMessage(
		not_null<Main::Session*> session,
		Api::MessageToSend &message) {
	return Manager::Instance().processOutgoingMessage(session, message);
}

void Init() {
	Manager::Instance().init();
}

} // namespace Plugins
