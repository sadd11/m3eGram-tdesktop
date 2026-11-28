/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_bridge.h"

#include "plugins/plugins_manager.h"

#include <vector>

namespace Plugins {
namespace {

// m3eGram: the plugin API surface. Everything the plugin can reach is
// defined here - the lib_quickjs build has no std/os modules, so a plugin
// can not touch files or the network directly, only through this bridge.
const char kBootstrap[] = R"JS("use strict";
const HookStrategy = { Default: 0, Cancel: 1, Modify: 2, ModifyFinal: 3 };
const AppEvent = { Start: "start", Stop: "stop", Pause: "pause", Resume: "resume" };
function HookResult(result) {
	result = result || {};
	return {
		strategy: result.strategy === undefined ? HookStrategy.Default : result.strategy,
		params: result.params === undefined ? null : result.params,
	};
}
const Plugin = {
	_meta: null,
	_instance: null,
	_callbacks: {},
	register(meta, instance) {
		Plugin._meta = meta || {};
		Plugin._instance = instance || null;
	},
	Base: class {
		addOnSendMessageHook() { __hostCall("__addSendHook", null); }
		onLoad() {}
		onUnload() {}
		onAppEvent(event) {}
		onMessageReceived(message) {}
		onSendMessageHook(params) { return HookResult(); }
		createSettings() { return []; }
		getSetting(key, def) {
			const r = __hostCall("settings.get", { key: String(key), def: def === undefined ? null : def });
			return (r === null || r === undefined) ? def : r;
		}
		setSetting(key, value) { __hostCall("settings.set", { key: String(key), value: value === undefined ? null : value }); }
	},
	_hostCallSync(method, args) {
		const raw = __hostCall(method, args === undefined ? null : args);
		if (raw && raw.__error !== undefined) throw new Error(raw.__error);
		return raw;
	},
	_makePromise(id) {
		return new Promise((resolve, reject) => {
			__hostCall("__registerPromise", { id: id, resolve: resolve, reject: reject });
		});
	},
	_async(method, args) {
		const raw = Plugin._hostCallSync(method, args);
		if (raw && raw.__async !== undefined) return Plugin._makePromise(raw.__async);
		if (raw && raw.__error !== undefined) throw new Error(raw.__error);
		return (raw === null || raw === undefined) ? null : raw;
	},
	_callInstance(method, args) {
		if (!Plugin._instance) return null;
		const fn = Plugin._instance[method];
		if (typeof fn !== "function") return null;
		return (args === null || args === undefined)
			? fn.call(Plugin._instance)
			: fn.call(Plugin._instance, args);
	},
	_settings() {
		const rows = Plugin._callInstance("createSettings", null);
		if (!Array.isArray(rows)) return [];
		Plugin._callbacks = {};
		let n = 0;
		const map = (row) => {
			if (!row || typeof row !== "object") return row;
			if (Array.isArray(row)) return row.map(map);
			const out = {};
			for (const k in row) {
				const v = row[k];
				if (typeof v === "function") {
					const id = "cb" + (n++);
					Plugin._callbacks[id] = v;
					out[k] = { __callback: id };
				} else if (v && typeof v === "object") {
					out[k] = map(v);
				} else {
					out[k] = v;
				}
			}
			return out;
		};
		return rows.map(map);
	},
	_fireCallback(id, value) {
		const f = Plugin._callbacks[id];
		if (typeof f === "function") f(value);
	},
	_getRegistered() {
		return { meta: Plugin._meta, hasInstance: !!Plugin._instance };
	},
};
const ui = {
	showToast(text) { __hostCall("ui.showToast", { text: String(text) }); },
	bulletin(text) { __hostCall("ui.showToast", { text: String(text) }); },
	showAlert(args) {
		__hostCall("ui.showAlert", {
			title: args && args.title ? String(args.title) : "",
			text: args && args.text ? String(args.text) : "",
		});
	},
	showConfirm(args) {
		return Plugin._async("ui.showConfirm", {
			title: args && args.title ? String(args.title) : "",
			text: args && args.text ? String(args.text) : "",
			confirmText: args && args.confirmText ? String(args.confirmText) : "",
			cancelText: args && args.cancelText ? String(args.cancelText) : "",
		});
	},
};
const client = {
	account() {
		const r = __hostCall("client.account", null);
		return (r === null || r === undefined) ? 0 : r;
	},
	sendToCurrentChat(text) { __hostCall("client.sendToCurrentChat", { text: String(text) }); },
	sendText(peerId, text) { __hostCall("client.sendText", { peerId: peerId, text: String(text) }); },
};
const http = {
	get(url, options) {
		return Plugin._async("http.get", {
			url: String(url),
			headers: (options && options.headers) || {},
		});
	},
	post(url, options) {
		return Plugin._async("http.post", {
			url: String(url),
			body: (options && options.body !== undefined && options.body !== null) ? String(options.body) : null,
			json: (options && options.json) || null,
			headers: (options && options.headers) || {},
		});
	},
};
const storage = {
	get(key, def) {
		const r = __hostCall("storage.get", { key: String(key), def: def === undefined ? null : def });
		return (r === null || r === undefined) ? def : r;
	},
	set(key, value) {
		__hostCall("storage.set", { key: String(key), value: value === undefined ? null : value });
	},
};
const console = {
	log(...a) { __hostCall("console.log", { level: "info", text: a.map((x) => String(x)).join(" ") }); },
	warn(...a) { __hostCall("console.log", { level: "warn", text: a.map((x) => String(x)).join(" ") }); },
	error(...a) { __hostCall("console.log", { level: "error", text: a.map((x) => String(x)).join(" ") }); },
};
const logger = {
	info: console.log,
	debug: console.log,
	warning: console.warn,
	error: console.error,
};
// Top-level const/class are global lexical bindings, not properties of the
// global object - Runtime::callGlobal() resolves "Plugin._x" through the
// global object, so expose Plugin there explicitly.
globalThis.Plugin = Plugin;
Plugin._bootstrapReady = true;
)JS";

} // namespace

Bridge::Bridge(Runtime *runtime, QString pluginId)
: _runtime(runtime)
, _pluginId(std::move(pluginId)) {
}

Bridge::~Bridge() {
	if (_runtime && _runtime->context()) {
		const auto ctx = _runtime->context();
		for (const auto &[id, pair] : _pending.asKeyValueRange()) {
			JS_FreeValue(ctx, pair.first);
			JS_FreeValue(ctx, pair.second);
		}
		_pending.clear();
	}
}

void Bridge::setState(QJsonObject settings, QJsonObject storage) {
	_settings = std::move(settings);
	_storage = std::move(storage);
}

void Bridge::install() {
	const auto ctx = _runtime->context();
	if (!ctx) {
		return;
	}
	JS_SetContextOpaque(ctx, static_cast<void *>(this));
	const auto fn = JS_NewCFunction(ctx, &Bridge::_fnHostCall, "__hostCall", 2);
	const auto global = JS_GetGlobalObject(ctx);
	JS_SetPropertyStr(ctx, global, "__hostCall", fn);
	JS_FreeValue(ctx, global);
	_runtime->eval(kBootstrap, u"plugins:bootstrap"_q);
}

QVariant Bridge::registeredInfo() const {
	bool ok = false;
	return _runtime->callGlobal("Plugin._getRegistered", {}, &ok);
}

const QString &Bridge::pluginId() const {
	return _pluginId;
}

bool Bridge::sendHookEnabled() const {
	return _sendHook;
}

QVariant Bridge::settingsGet(const QString &key, const QVariant &fallback) const {
	const auto it = _settings.constFind(key);
	return (it != _settings.constEnd()) ? it->toVariant() : fallback;
}

void Bridge::settingsSet(const QString &key, const QVariant &value) {
	_settings.insert(key, QJsonValue::fromVariant(value));
}

QJsonObject Bridge::settingsJson() const {
	return _settings;
}

QVariant Bridge::storageGet(const QString &key, const QVariant &fallback) const {
	const auto it = _storage.constFind(key);
	return (it != _storage.constEnd()) ? it->toVariant() : fallback;
}

void Bridge::storageSet(const QString &key, const QVariant &value) {
	_storage.insert(key, QJsonValue::fromVariant(value));
}

QJsonObject Bridge::storageJson() const {
	return _storage;
}

void Bridge::resolveAsync(const QString &id, const QVariant &result) {
	const auto it = _pending.find(id);
	if (it == _pending.end() || !_runtime || !_runtime->context()) {
		return;
	}
	const auto ctx = _runtime->context();
	auto arg = _runtime->variantToValue(result);
	const auto res = JS_Call(ctx, it->first, JS_UNDEFINED, 1, &arg);
	JS_FreeValue(ctx, arg);
	if (JS_IsException(res)) {
		_runtime->captureException();
	}
	JS_FreeValue(ctx, res);
	_pending.erase(it);
	_runtime->pumpJobs();
}

void Bridge::rejectAsync(const QString &id, const QString &error) {
	const auto it = _pending.find(id);
	if (it == _pending.end() || !_runtime || !_runtime->context()) {
		return;
	}
	const auto ctx = _runtime->context();
	auto arg = JS_NewStringLen(ctx, error.toUtf8().constData(), error.toUtf8().size());
	const auto res = JS_Call(ctx, it->second, JS_UNDEFINED, 1, &arg);
	JS_FreeValue(ctx, arg);
	if (JS_IsException(res)) {
		_runtime->captureException();
	}
	JS_FreeValue(ctx, res);
	_pending.erase(it);
	_runtime->pumpJobs();
}

JSValue Bridge::_fnHostCall(
		JSContext *ctx,
		JSValueConst,
		int argc,
		JSValueConst *argv) {
	const auto self = static_cast<Bridge *>(JS_GetContextOpaque(ctx));
	if (!self || argc < 1) {
		return JS_UNDEFINED;
	}
	auto size = size_t(0);
	const auto methodRaw = JS_ToCStringLen(ctx, &size, argv[0]);
	if (!methodRaw) {
		return JS_UNDEFINED;
	}
	const auto method = QString::fromUtf8(methodRaw, size);
	JS_FreeCString(ctx, methodRaw);

	if (method == u"__registerPromise") {
		if (argc < 2 || !JS_IsObject(argv[1])) {
			return JS_UNDEFINED;
		}
		const auto idValue = JS_GetPropertyStr(ctx, argv[1], "id");
		auto idSize = size_t(0);
		const auto idRaw = JS_ToCStringLen(ctx, &idSize, idValue);
		JS_FreeValue(ctx, idValue);
		if (!idRaw) {
			return JS_UNDEFINED;
		}
		const auto id = QString::fromUtf8(idRaw, idSize);
		JS_FreeCString(ctx, idRaw);
		const auto resolve = JS_GetPropertyStr(ctx, argv[1], "resolve");
		const auto reject = JS_GetPropertyStr(ctx, argv[1], "reject");
		self->_pending.insert(id, { resolve, reject });
		return JS_UNDEFINED;
	}

	QVariantMap args;
	if (argc >= 2 && JS_IsObject(argv[1])) {
		const auto converted = self->_runtime->valueToVariant(argv[1]);
		if (converted.typeId() == QMetaType::QJsonObject) {
			args = converted.toJsonObject().toVariantMap();
		}
	}
	auto error = QString();
	const auto result = self->hostCall(method, args, &error);
	if (!error.isEmpty()) {
		auto object = JS_NewObject(ctx);
		const auto utf = error.toUtf8();
		JS_SetPropertyStr(
			ctx,
			object,
			"__error",
			JS_NewStringLen(ctx, utf.constData(), utf.size()));
		return object;
	}
	return self->_runtime->variantToValue(result);
}

QVariant Bridge::hostCall(
		const QString &method,
		const QVariantMap &args,
		QString *error) {
	if (method == u"settings.get") {
		return settingsGet(args.value(u"key"_q).toString(), args.value(u"def"_q));
	} else if (method == u"settings.set") {
		settingsSet(args.value(u"key"_q).toString(), args.value(u"value"_q));
		return QVariant();
	} else if (method == u"storage.get") {
		return storageGet(args.value(u"key"_q).toString(), args.value(u"def"_q));
	} else if (method == u"storage.set") {
		storageSet(args.value(u"key"_q).toString(), args.value(u"value"_q));
		return QVariant();
	} else if (method == u"__addSendHook") {
		_sendHook = true;
		return QVariant();
	}
	return Manager::Instance().hostCallFromPlugin(_pluginId, method, args, error);
}

} // namespace Plugins
