/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "qjs_shim.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>

namespace Plugins {

// m3eGram: QuickJS sandbox for a single user plugin. Each plugin runs in its
// own runtime (memory + interrupt limits), the only API it can reach is the
// bridge injected by Plugins::Bridge - there is no std/os module support
// compiled into lib_quickjs, so plugins are safe by default.
class Runtime final {
public:
	Runtime();
	~Runtime();

	Runtime(const Runtime &other) = delete;
	Runtime &operator=(const Runtime &other) = delete;

	[[nodiscard]] bool valid() const;
	[[nodiscard]] JSContext *context() const;

	// Evaluate a script (module-like global code), returns false on error.
	[[nodiscard]] bool eval(const QByteArray &code, const QString &name);

	// Resolve a dotted global path ("Plugin._invoke") and call it.
	[[nodiscard]] QVariant callGlobal(
		const QString &dottedName,
		const QVariantList &args,
		bool *ok = nullptr);

	// Run pending promise jobs after any call that may have created them.
	void pumpJobs();

	// QVariant <-> JSValue conversion (used by the bridge and the manager).
	[[nodiscard]] QVariant valueToVariant(JSValue value, int depth = 0);
	[[nodiscard]] JSValue variantToValue(const QVariant &value);

	// Limit a single synchronous dispatch (hook / eval / callback) in time.
	void beginDispatch();
	void endDispatch();
	[[nodiscard]] bool dispatchInterrupted() const;

	// Take the pending exception (if any) into lastError().
	void captureException();

	[[nodiscard]] QString lastError() const;

private:
	static JSValue _interruptHandler(JSContext *ctx, void *opaque);

	void reset();

	JSRuntime *_rt = nullptr;
	JSContext *_ctx = nullptr;
	QString _lastError;
	QElapsedTimer _dispatchTimer; // m3eGram: for the interrupt handler.
	bool _dispatching = false;
	bool _interrupted = false;

};

// Convenience: synchronous dispatch guard (begin/end + jobs pump).
class DispatchGuard final {
public:
	explicit DispatchGuard(Runtime *runtime);
	~DispatchGuard();

	[[nodiscard]] bool interrupted() const;

private:
	Runtime *_runtime = nullptr;

};

} // namespace Plugins
