/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_runtime.h"

#include <algorithm>
#include <vector>

namespace Plugins {
namespace {

// m3eGram: a plugin must not hang the UI thread - every synchronous
// dispatch (hook, eval, callback) is capped in time.
constexpr auto kMaxDispatchMs = crl::time(2000);
constexpr auto kMemoryLimitBytes = 32 * 1024 * 1024;
constexpr auto kMaxStackSizeBytes = 512 * 1024;
constexpr auto kMaxValueDepth = 8;
constexpr auto kMaxArrayItems = 4096;
constexpr auto kMaxObjectKeys = 256;

[[nodiscard]] QByteArray Utf8(const QString &value) {
	return value.toUtf8();
}

} // namespace

Runtime::Runtime() {
	_rt = JS_NewRuntime();
	if (!_rt) {
		return;
	}
	JS_SetMemoryLimit(_rt, kMemoryLimitBytes);
	JS_SetMaxStackSize(_rt, kMaxStackSizeBytes);
	JS_SetInterruptHandler(
		_rt,
		&Runtime::_interruptHandler,
		static_cast<void *>(this));
	_ctx = JS_NewContext(_rt);
	if (!_ctx) {
		reset();
	}
}

Runtime::~Runtime() {
	reset();
}

void Runtime::reset() {
	if (_ctx) {
		JS_FreeContext(_ctx);
		_ctx = nullptr;
	}
	if (_rt) {
		JS_FreeRuntime(_rt);
		_rt = nullptr;
	}
	_lastError = QString();
}

bool Runtime::valid() const {
	return (_rt != nullptr) && (_ctx != nullptr);
}

JSContext *Runtime::context() const {
	return _ctx;
}

JSValue Runtime::_interruptHandler(JSContext *, void *opaque) {
	const auto runtime = static_cast<Runtime *>(opaque);
	if (!runtime || !runtime->_dispatching) {
		// Not our dispatch (e.g. a promise job after the guard ended) -
		// don't interrupt anything.
		return JS_TRUE;
	}
	if (runtime->_dispatchTimer.isValid()
		&& runtime->_dispatchTimer.elapsed() > kMaxDispatchMs) {
		runtime->_interrupted = true;
		return JS_FALSE;
	}
	return JS_TRUE;
}

bool Runtime::eval(const QByteArray &code, const QString &name) {
	if (!_ctx) {
		return false;
	}
	const auto nameUtf = Utf8(name);
	const auto result = JS_Eval(
		_ctx,
		code.constData(),
		code.size(),
		nameUtf.constData(),
		JS_EVAL_TYPE_GLOBAL);
	const auto ok = !JS_IsException(result);
	JS_FreeValue(_ctx, result);
	if (!ok) {
		captureException();
	}
	return ok;
}

QVariant Runtime::valueToVariant(JSValue value, int depth) {
	if (depth > kMaxValueDepth) {
		return QVariant();
	}
	switch (JS_VALUE_GET_NORM_TAG(value)) {
	case JS_TAG_BOOL:
		return QVariant(JS_ToBool(_ctx, value) == 1);
	case JS_TAG_INT:
		return QVariant(static_cast<double>(JS_VALUE_GET_INT(value)));
	case JS_TAG_FLOAT64:
		return QVariant(JS_VALUE_GET_FLOAT64(value));
	case JS_TAG_STRING: {
		auto size = 0;
		const auto raw = JS_ToCStringLen(_ctx, &size, value);
		if (!raw) {
			return QVariant();
		}
		const auto result = QString::fromUtf8(raw, size);
		JS_FreeCString(_ctx, raw);
		return result;
	}
	case JS_TAG_OBJECT: {
		if (JS_IsArray(_ctx, value) == 1) {
			const auto lengthValue = JS_GetPropertyStr(_ctx, value, "length");
			auto length = 0;
			JS_ToInt32(_ctx, &length, lengthValue);
			JS_FreeValue(_ctx, lengthValue);
			auto list = QVariantList();
			list.reserve(std::min(length, int(kMaxArrayItems)));
			for (auto i = 0; i != length && i < kMaxArrayItems; ++i) {
				const auto item = JS_GetPropertyUint32(_ctx, value, i);
				list.push_back(valueToVariant(item, depth + 1));
				JS_FreeValue(_ctx, item);
			}
			return list;
		}
		auto object = QJsonObject();
		auto tab = static_cast<JSPropertyEnum *>(nullptr);
		auto count = uint32_t(0);
		if (JS_GetOwnPropertyNames(
				_ctx,
				&tab,
				&count,
				value,
				JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
			JS_GetException(_ctx); // Drop the error, return what we have.
			return object;
		}
		for (uint32_t i = 0; i != count && i < kMaxObjectKeys; ++i) {
			const auto keyString = JS_AtomToString(_ctx, tab[i].atom);
			auto keySize = 0;
			const auto keyRaw = JS_IsString(keyString)
				? JS_ToCStringLen(_ctx, &keySize, keyString)
				: nullptr;
			if (keyRaw) {
				const auto item = JS_GetProperty(_ctx, value, tab[i].atom);
				object.insert(
					QString::fromUtf8(keyRaw, keySize),
					valueToVariant(item, depth + 1));
				JS_FreeValue(_ctx, item);
				JS_FreeCString(_ctx, keyRaw);
			}
			JS_FreeValue(_ctx, keyString);
			JS_FreeAtom(_ctx, tab[i].atom);
		}
		js_free(_ctx, tab);
		return object;
	}
	default:
		return QVariant();
	}
}

JSValue Runtime::variantToValue(const QVariant &value) {
	switch (value.typeId()) {
	case QMetaType::Bool:
		return JS_NewBool(_ctx, value.toBool());
	case QMetaType::Int:
		return JS_NewInt32(_ctx, value.toInt());
	case QMetaType::LongLong:
		return JS_NewInt64(_ctx, value.toLongLong());
	case QMetaType::Double:
	case QMetaType::Float:
		return JS_NewFloat64(_ctx, value.toDouble());
	case QMetaType::QString: {
		const auto utf = Utf8(value.toString());
		return JS_NewStringLen(_ctx, utf.constData(), utf.size());
	}
	case QMetaType::QVariantList: {
		auto array = JS_NewArray(_ctx);
		auto index = uint32_t(0);
		for (const auto &item : value.toList()) {
			JS_SetPropertyUint32(_ctx, array, index++, variantToValue(item));
		}
		return array;
	}
	case QMetaType::QJsonObject: {
		auto object = JS_NewObject(_ctx);
		const auto json = value.toJsonObject();
		for (auto it = json.begin(); it != json.end(); ++it) {
			const auto key = Utf8(it.key());
			JS_SetPropertyStr(
				_ctx,
				object,
				key.constData(),
				variantToValue(it.value().toVariant()));
		}
		return object;
	}
	case QMetaType::QVariantMap: {
		auto object = JS_NewObject(_ctx);
		const auto map = value.toMap();
		for (auto it = map.begin(); it != map.end(); ++it) {
			const auto key = Utf8(it.key());
			JS_SetPropertyStr(
				_ctx,
				object,
				key.constData(),
				variantToValue(it.value()));
		}
		return object;
	}
	default:
		return JS_UNDEFINED;
	}
}

QVariant Runtime::callGlobal(
		const QString &dottedName,
		const QVariantList &args,
		bool *ok) {
	_lastError = QString();
	if (!_ctx) {
		if (ok) *ok = false;
		return QVariant();
	}
	auto object = JS_GetGlobalObject(_ctx);
	auto current = object;
	auto resolved = JS_UNDEFINED;
	for (const auto &part : dottedName.split('.')) {
		resolved = JS_GetPropertyStr(_ctx, current, Utf8(part).constData());
		JS_FreeValue(_ctx, current);
		current = resolved;
		if (JS_IsUndefined(resolved) || JS_IsException(resolved)) {
			break;
		}
	}
	const auto callable = current;
	const auto callableOk = JS_IsFunction(_ctx, callable) > 0;
	auto result = JS_UNDEFINED;
	if (callableOk) {
		auto argv = std::vector<JSValue>();
		argv.reserve(args.size());
		for (const auto &arg : args) {
			argv.push_back(variantToValue(arg));
		}
		result = JS_Call(
			_ctx,
			callable,
			JS_UNDEFINED,
			int(argv.size()),
			argv.data());
		for (const auto &arg : argv) {
			JS_FreeValue(_ctx, arg);
		}
	}
	JS_FreeValue(_ctx, callable);
	JS_FreeValue(_ctx, object);
	auto variant = QVariant();
	const auto success = callableOk && !JS_IsException(result);
	if (success) {
		variant = valueToVariant(result);
	} else if (!callableOk) {
		_lastError = u"Function not found: %1"_q.arg(dottedName);
	}
	if (JS_IsException(result)) {
		captureException();
	} else {
		JS_FreeValue(_ctx, result);
	}
	if (ok) *ok = success;
	return variant;
}

void Runtime::pumpJobs() {
	if (!_rt) {
		return;
	}
	JSContext *pending = nullptr;
	while (JS_IsJobPending(_rt)) {
		const auto status = JS_ExecutePendingJob(_rt, &pending);
		if (status < 0) {
			captureException();
			break;
		}
		if (_interrupted) {
			break;
		}
	}
}

void Runtime::beginDispatch() {
	_dispatchTimer.start();
	_dispatching = true;
	_interrupted = false;
}

void Runtime::endDispatch() {
	_dispatching = false;
}

bool Runtime::dispatchInterrupted() const {
	return _interrupted;
}

QString Runtime::lastError() const {
	return _lastError;
}

void Runtime::captureException() {
	const auto exception = JS_GetException(_ctx);
	auto size = 0;
	const auto raw = JS_ToCStringLen(_ctx, &size, exception);
	if (raw) {
		_lastError = QString::fromUtf8(raw, size);
		JS_FreeCString(_ctx, raw);
	} else {
		_lastError = u"Unknown JS error"_q;
	}
	// Attach the stack if the exception is an object with .stack.
	if (JS_IsObject(exception)) {
		const auto stack = JS_GetPropertyStr(_ctx, exception, "stack");
		if (JS_IsString(stack)) {
			auto stackSize = 0;
			const auto stackRaw = JS_ToCStringLen(_ctx, &stackSize, stack);
			if (stackRaw) {
				_lastError += u"\n"_q + QString::fromUtf8(stackRaw, stackSize);
				JS_FreeCString(_ctx, stackRaw);
			}
		}
		JS_FreeValue(_ctx, stack);
	}
	JS_FreeValue(_ctx, exception);
}

DispatchGuard::DispatchGuard(Runtime *runtime) : _runtime(runtime) {
	if (_runtime) {
		_runtime->beginDispatch();
	}
}

DispatchGuard::~DispatchGuard() {
	if (_runtime) {
		_runtime->endDispatch();
		_runtime->pumpJobs();
	}
}

bool DispatchGuard::interrupted() const {
	return _runtime ? _runtime->dispatchInterrupted() : false;
}

} // namespace Plugins
