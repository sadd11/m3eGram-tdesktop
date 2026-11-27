/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "plugins/plugins_runtime.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Plugins {

// m3eGram: best-effort exteraGram (.py) -> QuickJS (.js) source converter.
// Handles the documented exteraGram plugin idioms (BasePlugin classes,
// lifecycle hooks, settings rows, f-strings, basic control flow); anything
// it can not map confidently is passed through with a warning.
[[nodiscard]] QString ConvertExteraPlugin(
	const QString &source,
	const QString &fileName,
	QStringList *warnings);

// Install a plugin package: .jplugin (zip with a .js script),
// .plugin (exteraGram zip with Python sources, run through the converter)
// or a plain .js file. The result is placed into tdata/plugins.
void InstallPluginFromFile(
	not_null<Window::SessionController*> controller,
	const QString &path);

} // namespace Plugins
