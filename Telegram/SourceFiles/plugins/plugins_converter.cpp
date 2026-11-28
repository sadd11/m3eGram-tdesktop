/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "plugins/plugins_converter.h"

#include "core/application.h"
#include "plugins/plugins_manager.h"
#include "ui/boxes/confirm_box.h"
#include "base/flat_set.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtWidgets/QFileDialog>
#include <minizip/unzip.h>

namespace Plugins {
namespace {

constexpr auto kReadChunk = 64 * 1024;

void Warn(
		QStringList *warnings,
		const QString &fileName,
		int line,
		const QString &text) {
	if (warnings) {
		*warnings << fileName + u":"_q + QString::number(line + 1) + u": "_q
			+ text;
	}
}

// Strip a python comment (best effort: outside of string literals).
void StripComment(QString &line) {
	auto quote = QChar(0);
	for (auto i = 0; i != line.size(); ++i) {
		const auto ch = line[i];
		if (quote != QChar(0)) {
			if (ch == quote && line[i - 1] != '\\') {
				quote = QChar(0);
			}
		} else if (ch == '"' || ch == '\'') {
			quote = ch;
		} else if (ch == '#') {
			line = line.left(i).trimmed();
			return;
		}
	}
	line = line.trimmed();
}

[[nodiscard]] QString TransformExpression(QString expression) {
	// f-strings -> template literals (single-line, no nested braces specs).
	static const auto fString = QRegularExpression(
		R"re(f"([^"\\]*(?:\\.[^"\\]*)*)")re");
	expression.replace(fString, u"`\\1`"_q);
	static const auto braces = QRegularExpression(
		R"re(\{([^:{}]+)\})re");
	{
		auto changed = false;
		auto result = expression;
		auto it = braces.globalMatch(result);
		auto out = QString();
		auto pos = qsizetype(0);
		while (it.hasNext()) {
			const auto match = it.next();
			out += result.mid(pos, match.capturedStart() - pos);
			out += u"${"_q + match.captured(1) + u"}"_q;
			pos = match.capturedEnd();
			changed = true;
		}
		out += result.mid(pos);
		if (changed) {
			expression = out;
		}
	}

	static const auto replacements = std::vector<std::pair<QString, QString>>{
		{ u"is not"_q, u"!=="_q },
		{ u"is"_q, u"==="_q },
	};
	for (const auto &[from, to] : replacements) {
		QRegularExpression re(u"\\b%1\\b"_q.arg(from));
		expression.replace(re, to);
	}

	// Do the simple word-level ones first.
	static const auto wordMap = std::vector<std::pair<QString, QString>>{
		{ u"True"_q, u"true"_q },
		{ u"False"_q, u"false"_q },
		{ u"None"_q, u"null"_q },
		{ u"self"_q, u"this"_q },
	};
	for (const auto &[from, to] : wordMap) {
		QRegularExpression re(u"\\b%1\\b"_q.arg(from));
		expression.replace(re, to);
	}
	{
		static const auto andRe = QRegularExpression(u"\\band\\b"_q);
		expression.replace(andRe, u"&&"_q);
		static const auto orRe = QRegularExpression(u"\\bor\\b"_q);
		expression.replace(orRe, u"||"_q);
		static const auto notRe = QRegularExpression(u"\\bnot\\b"_q);
		expression.replace(notRe, u"!"_q);
		static const auto printRe = QRegularExpression(u"\\bprint\\("_q);
		expression.replace(printRe, u"console.log("_q);
		static const auto strRe = QRegularExpression(u"\\bstr\\("_q);
		expression.replace(strRe, u"String("_q);
		static const auto startsRe = QRegularExpression(u"\\.startswith\\("_q);
		expression.replace(startsRe, u".startsWith("_q);
		static const auto endsRe = QRegularExpression(u"\\.endswith\\("_q);
		expression.replace(endsRe, u".endsWith("_q);
		static const auto lowerRe = QRegularExpression(u"\\.lower\\(\\)"_q);
		expression.replace(lowerRe, u".toLowerCase()"_q);
		static const auto upperRe = QRegularExpression(u"\\.upper\\(\\)"_q);
		expression.replace(upperRe, u".toUpperCase()"_q);
		static const auto stripRe = QRegularExpression(u"\\.strip\\(\\)"_q);
		expression.replace(stripRe, u".trim()"_q);
	}

	// exteraGram API surface -> our bridge API.
	static const auto apiMap = std::vector<std::pair<QString, QString>>{
		{ u"on_plugin_load"_q, u"onLoad"_q },
		{ u"on_plugin_unload"_q, u"onUnload"_q },
		{ u"on_app_event"_q, u"onAppEvent"_q },
		{ u"on_send_message_hook"_q, u"onSendMessageHook"_q },
		{ u"add_on_send_message_hook"_q, u"addOnSendMessageHook"_q },
		{ u"create_settings"_q, u"createSettings"_q },
		{ u"get_setting"_q, u"getSetting"_q },
		{ u"set_setting"_q, u"setSetting"_q },
		{ u"HookStrategy\\.MODIFY_FINAL"_q, u"HookStrategy.ModifyFinal"_q },
		{ u"HookStrategy\\.MODIFY"_q, u"HookStrategy.Modify"_q },
		{ u"HookStrategy\\.CANCEL"_q, u"HookStrategy.Cancel"_q },
		{ u"HookStrategy\\.DEFAULT"_q, u"HookStrategy.Default"_q },
		{ u"ui\\.show_toast"_q, u"ui.showToast"_q },
		{ u"ui\\.show_bulletin"_q, u"ui.showToast"_q },
		{ u"ui\\.show_alert_dialog"_q, u"ui.showAlert"_q },
		{ u"ui\\.show_alert"_q, u"ui.showAlert"_q },
		{ u"ui\\.show_confirm_dialog"_q, u"ui.showConfirm"_q },
		{ u"logger\\.info"_q, u"console.log"_q },
		{ u"logger\\.debug"_q, u"console.log"_q },
		{ u"logger\\.warning"_q, u"console.warn"_q },
		{ u"logger\\.error"_q, u"console.error"_q },
	};
	for (const auto &[from, to] : apiMap) {
		QRegularExpression re(u"\\b%1\\b"_q.arg(from));
		expression.replace(re, to);
	}
	return expression;
}

[[nodiscard]] QString JsArgsFromKwargs(const QString &call) {
	static const auto kwarg = QRegularExpression(
		R"re((?<![=!<>])(\b\w+\b)\s*=(?!=))re");
	return QString(call).replace(kwarg, u"\\1: "_q);
}

} // namespace

QString ConvertExteraPlugin(
		const QString &source,
		const QString &fileName,
		QStringList *warnings) {
	auto out = QStringList();
	auto metaId = QString();
	auto metaName = QString();
	auto metaDescription = QString();
	auto metaAuthor = QString();
	auto metaVersion = QString();
	auto metaIcon = QString();
	auto className = QString();

	const auto lines = source.split('\n');
	auto stack = QVector<int>();
	const auto warn = [&](int line, const QString &text) {
		Warn(warnings, fileName, line, text);
	};
	auto blankWasEmitted = true;
	for (auto i = 0; i != lines.size(); ++i) {
		auto line = lines[i];
		line.replace(u'\t', u"    ");
		StripComment(line);
		const auto trimmed = line.trimmed();
		const auto indent = [&] {
			auto result = 0;
			for (const auto ch : line) {
				if (ch == ' ') {
					++result;
				} else {
					break;
				}
			}
			return result;
		}();
		const auto pad = QString(indent, ' ');
		const auto emitLine = [&](QString text) {
			out << pad + text;
			blankWasEmitted = false;
		};
		const auto emitBlank = [&] {
			if (!blankWasEmitted) {
				out << QString();
				blankWasEmitted = true;
			}
		};

		if (trimmed.isEmpty()) {
			emitBlank();
			continue;
		}

		// Metadata constants.
		static const auto metaRe = QRegularExpression(
			R"re(^(__\w+__)\s*=\s*"?\s*([^"]*?)\s*"?\s*$)re");
		if (const auto m = metaRe.match(trimmed); m.hasMatch()) {
			const auto key = m.captured(1);
			const auto value = m.captured(2);
			if (key == u"__id__"_q) {
				metaId = value;
			} else if (key == u"__name__"_q) {
				metaName = value;
			} else if (key == u"__description__"_q) {
				metaDescription = value;
			} else if (key == u"__author__"_q) {
				metaAuthor = value;
			} else if (key == u"__version__"_q) {
				metaVersion = value;
			} else if (key == u"__icon__"_q) {
				metaIcon = value;
			}
			out << u"// %1 = %2"_q.arg(key, value);
			blankWasEmitted = false;
			continue;
		}

		// Imports: only exteraGram-internal ones are dropped silently.
		if (trimmed.startsWith(u"from "_q) || trimmed.startsWith(u"import "_q)) {
			if (trimmed.contains(u"base_plugin"_q)
				|| trimmed.contains(u"ui.settings"_q)
				|| trimmed.contains(u"ui"_q)
				|| trimmed.contains(u"android_utils"_q)
				|| trimmed.contains(u"hook_utils"_q)
				|| trimmed.contains(u"client_utils"_q)
				|| trimmed.contains(u"extera_utils"_q)
				|| trimmed.contains(u"file_utils"_q)) {
				continue;
			}
			warn(i, u"import '%1' dropped - check dependencies"_q.arg(trimmed));
			continue;
		}

		// Decorators are not supported.
		if (trimmed.startsWith(u'@')) {
			warn(i, u"decorator dropped: %1"_q.arg(trimmed));
			continue;
		}

		// Class definition.
		static const auto classRe = QRegularExpression(
			R"re(^class\s+(\w+)\s*(?:\(([^)]*)\))?\s*:\s*(.*))re");
		if (const auto m = classRe.match(trimmed); m.hasMatch()) {
			while (!stack.isEmpty()) {
				out << u"}"_q;
				stack.removeLast();
			}
			className = m.captured(1);
			const auto base = m.captured(2).trimmed();
			const auto extends = base.isEmpty()
				? QString()
				: (base.contains(u"BasePlugin"_q)
					? u" extends Plugin.Base"_q
					: (u" extends /* " + base + u" */"_q));
			emitLine(u"class %1%2 {"_q.arg(className, extends));
			stack.append(indent);
			if (!m.captured(3).isEmpty()) {
				warn(i, u"class one-line body moved inside"_q);
				emitLine(TransformExpression(m.captured(3)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}

		// Function definition.
		static const auto defRe = QRegularExpression(
			R"re(^(async\s+)?def\s+(\w+)\s*\((.*)\)\s*:\s*(.*))re");
		if (const auto m = defRe.match(trimmed); m.hasMatch()) {
			while (!stack.isEmpty() && stack.last() >= indent) {
				out << u"}"_q;
				stack.removeLast();
			}
			auto name = m.captured(2);
			if (name == u"__init__"_q) {
				name = u"constructor"_q;
			}
			const auto isAsync = m.captured(1).trimmed().isEmpty()
				? QString()
				: u"async "_q;
			const auto args = m.captured(3);
			// Drop self / defaults / *args / **kwargs.
			auto argList = QStringList();
			auto first = true;
			for (auto arg : args.split(u',')) {
				arg = arg.trimmed();
				if (arg.isEmpty()) {
					continue;
				}
				if (first && (arg == u"self"_q || arg == u"cls"_q)) {
					first = false;
					continue;
				}
				first = false;
				if (arg.startsWith(u'*')) {
					warn(i, u"*args/**kwargs dropped: %1"_q.arg(arg));
					continue;
				}
				const auto eq = arg.indexOf(u'=');
				if (eq > 0) {
					arg = arg.left(eq).trimmed();
				}
				const auto colon = arg.indexOf(u':');
				if (colon > 0) {
					arg = arg.left(colon).trimmed();
				}
				argList << arg;
			}
			emitLine(u"%1%2(%3) {"_q.arg(
				isAsync,
				name,
				argList.join(u", "_q)));
			stack.append(indent);
			if (!m.captured(4).isEmpty()) {
				emitLine(TransformExpression(m.captured(4)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}

		// Block continuations: elif / else / except / finally.
		static const auto contRe = QRegularExpression(
			R"re(^(elif|else|except|finally)\b\s*(.*))re");
		if (const auto m = contRe.match(trimmed); m.hasMatch()) {
			const auto kind = m.captured(1);
			if (stack.isEmpty() || stack.last() != indent) {
				warn(i, u"%1 without matching block indent"_q.arg(kind));
			} else {
				stack.removeLast();
			}
			auto opener = QString();
			if (kind == u"elif"_q) {
				opener = u"} else if (%1) {"_q.arg(
					TransformExpression(m.captured(2)));
			} else if (kind == u"else"_q) {
				opener = u"} else {"_q;
			} else if (kind == u"except"_q) {
				opener = u"} catch (e) {"_q;
			} else {
				opener = u"} finally {"_q;
			}
			emitLine(opener);
			stack.append(indent);
			continue;
		}

		// Block openers.
		static const auto ifRe = QRegularExpression(
			R"re(^if\s+(.+?)\s*:\s*(.*))re");
		static const auto forRe = QRegularExpression(
			R"re(^for\s+(.+?)\s+in\s+(.+?)\s*:\s*(.*))re");
		static const auto whileRe = QRegularExpression(
			R"re(^while\s+(.+?)\s*:\s*(.*))re");
		static const auto tryRe = QRegularExpression(u"^try\\s*:\\s*(.*)");
		static const auto withRe = QRegularExpression(
			R"re(^with\s+(.+?)\s*:\s*(.*))re");
		const auto openBlock = [&](const QString &header) {
			while (!stack.isEmpty() && stack.last() > indent) {
				out << u"}"_q;
				stack.removeLast();
			}
			emitLine(header);
			stack.append(indent);
		};
		if (const auto m = ifRe.match(trimmed); m.hasMatch()
			&& trimmed.startsWith(u"if "_q)) {
			openBlock(u"if (%1) {"_q.arg(TransformExpression(m.captured(1))));
			if (!m.captured(2).isEmpty()) {
				emitLine(TransformExpression(m.captured(2)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}
		if (const auto m = forRe.match(trimmed); m.hasMatch()
			&& trimmed.startsWith(u"for "_q)) {
			openBlock(u"for (const %1 of %2) {"_q.arg(
				TransformExpression(m.captured(1)),
				TransformExpression(m.captured(2))));
			if (!m.captured(3).isEmpty()) {
				emitLine(TransformExpression(m.captured(3)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}
		if (const auto m = whileRe.match(trimmed); m.hasMatch()
			&& trimmed.startsWith(u"while "_q)) {
			openBlock(u"while (%1) {"_q.arg(
				TransformExpression(m.captured(1))));
			if (!m.captured(2).isEmpty()) {
				emitLine(TransformExpression(m.captured(2)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}
		if (const auto m = tryRe.match(trimmed); m.hasMatch()
			&& trimmed.startsWith(u"try"_q)) {
			openBlock(u"try {"_q);
			if (!m.captured(1).isEmpty()) {
				emitLine(TransformExpression(m.captured(1)) + u";"_q);
				out << pad + u"}"_q;
				stack.removeLast();
			}
			emitBlank();
			continue;
		}
		if (const auto m = withRe.match(trimmed); m.hasMatch()
			&& trimmed.startsWith(u"with "_q)) {
			warn(i, u"'with' converted to a plain block - check resource use"_q);
			openBlock(u"{ /* with %1 */"_q.arg(m.captured(1)));
			continue;
		}

		// Simple statements.
		if (trimmed == u"pass"_q) {
			emitLine(u"// pass"_q);
			continue;
		}
		if (trimmed.startsWith(u"raise "_q)) {
			emitLine(u"throw "_q + TransformExpression(
				trimmed.mid(6)) + u";"_q);
			continue;
		}
		if (trimmed.startsWith(u"global "_q)
			|| trimmed.startsWith(u"nonlocal "_q)
			|| trimmed.startsWith(u"del "_q)
			|| trimmed.startsWith(u"assert "_q)) {
			warn(i, u"statement dropped: %1"_q.arg(
				trimmed.section(u' ', 0, 0)));
			continue;
		}

		// Settings rows: Header(..) / Switch(..) / ... -> object literals.
		static const auto rowCtor = QRegularExpression(
			R"re(^(\w+)\((.*)\)\s*$)re");
		static const auto kRowTypes = base::flat_set<QString>{
			u"Header"_q, u"Divider"_q, u"Switch"_q, u"Selector"_q,
			u"Input"_q, u"Text"_q, u"EditText"_q,
		};
		if (const auto m = rowCtor.match(trimmed); m.hasMatch()
			&& kRowTypes.contains(m.captured(1))) {
			const auto type = m.captured(1).toLower();
			auto body = TransformExpression(m.captured(2));
			body = JsArgsFromKwargs(body);
			emitLine(u"{ type: \"%1\", %2 }"_q.arg(type, body) + u";"_q);
			continue;
		}

		// Plain expression / assignment.
		auto transformed = TransformExpression(trimmed);
		if (transformed.contains(u"len("_q)
			|| transformed.contains(u".format("_q)
			|| transformed.contains(u"% ("_q)
			|| transformed.contains(u") % "_q)) {
			warn(i, u"possible python-only construct left as is: %1"_q.arg(
				transformed.section(u' ', 0, 2)));
		}
		const auto last = transformed[transformed.size() - 1];
		const auto needsSemicolon = (last != '{')
			&& (last != '}')
			&& (last != ':')
			&& (last != ',')
			&& (last != '(');
		emitLine(transformed + (needsSemicolon ? u";"_q : QString()));
	}

	// Close everything still open.
	while (!stack.isEmpty()) {
		out << u"}"_q;
		stack.removeLast();
	}

	// Registration stub.
	if (className.isEmpty() && metaId.isEmpty()) {
		// Nothing to register.
	} else {
		out << QString();
		auto meta = QStringList();
		const auto push = [&](const char *key, const QString &value) {
			meta << u"%1: \"%2\""_q.arg(
				QString::fromLatin1(key),
				QString(value).replace(u'"', u"\\\""_q));
		};
		push("id", metaId.isEmpty() ? fileName + u".converted"_q : metaId);
		if (!metaName.isEmpty()) {
			push("name", metaName);
		}
		if (!metaDescription.isEmpty()) {
			push("description", metaDescription);
		}
		if (!metaAuthor.isEmpty()) {
			push("author", metaAuthor);
		}
		if (!metaVersion.isEmpty()) {
			push("version", metaVersion);
		}
		if (!metaIcon.isEmpty()) {
			push("icon", metaIcon);
		}
		out << u"Plugin.register({ "_q
			+ meta.join(u", "_q)
			+ (className.isEmpty()
				? u" });"_q
				: (u" }, new "_q + className + u"());"_q));
	}
	return out.join(u'\n');
}

namespace {

// Read-only minizip IO on a QFile (unicode path support), mirroring the
// ZipToFile writer from storage_folder_archive.cpp.
class ZipFromFile {
public:
	explicit ZipFromFile(const QString &path) : _file(path) {
	}

	[[nodiscard]] zlib_filefunc64_def funcs() {
		auto result = zlib_filefunc64_def();
		result.opaque = this;
		result.zopen64_file = &ZipFromFile::Open;
		result.zread_file = &ZipFromFile::Read;
		result.zwrite_file = &ZipFromFile::Write;
		result.ztell64_file = &ZipFromFile::Tell;
		result.zseek64_file = &ZipFromFile::Seek;
		result.zclose_file = &ZipFromFile::Close;
		result.zerror_file = &ZipFromFile::Error;
		return result;
	}

private:
	voidpf open() {
		return _file.open(QIODevice::ReadOnly) ? this : nullptr;
	}

	uLong read(void *buf, uLong size) {
		const auto result = _file.read(static_cast<char*>(buf), size);
		return (result > 0) ? uLong(result) : 0;
	}

	uLong write(const void *, uLong) {
		return 0;
	}

	[[nodiscard]] ZPOS64_T tell() {
		return ZPOS64_T(_file.pos());
	}

	long seek(ZPOS64_T offset, int origin) {
		const auto position = (origin == ZLIB_FILEFUNC_SEEK_SET)
			? int64(offset)
			: (origin == ZLIB_FILEFUNC_SEEK_CUR)
			? (_file.pos() + int64(offset))
			: (_file.size() + int64(offset));
		return _file.seek(position) ? 0 : -1;
	}

	int close() {
		_file.close();
		return 0;
	}

	[[nodiscard]] int error() {
		return (_file.error() == QFileDevice::NoError) ? 0 : -1;
	}

	static voidpf Open(voidpf opaque, const void *, int) {
		return static_cast<ZipFromFile*>(opaque)->open();
	}

	static uLong Read(voidpf opaque, voidpf, void *buf, uLong size) {
		return static_cast<ZipFromFile*>(opaque)->read(buf, size);
	}

	static uLong Write(
			voidpf opaque,
			voidpf,
			const void *buf,
			uLong size) {
		return static_cast<ZipFromFile*>(opaque)->write(buf, size);
	}

	static ZPOS64_T Tell(voidpf opaque, voidpf) {
		return static_cast<ZipFromFile*>(opaque)->tell();
	}

	static long Seek(
			voidpf opaque,
			voidpf,
			ZPOS64_T offset,
			int origin) {
		return static_cast<ZipFromFile*>(opaque)->seek(offset, origin);
	}

	static int Close(voidpf opaque, voidpf) {
		return static_cast<ZipFromFile*>(opaque)->close();
	}

	static int Error(voidpf opaque, voidpf) {
		return static_cast<ZipFromFile*>(opaque)->error();
	}

	QFile _file;

};

[[nodiscard]] std::optional<QByteArray> ReadZipEntry(
		const QString &path,
		const QStringList &preferredNames,
		const QString &extension,
		QString *entryName) {
	auto io = ZipFromFile(path);
	auto funcs = io.funcs();
	const auto zip = unzOpen2_64("", &funcs);
	if (!zip) {
		return std::nullopt;
	}
	auto guard = gsl::finally([&] { unzClose(zip); });
	auto names = QStringList();
	for (auto pass = 0; pass != 2; ++pass) {
		auto status = unzGoToFirstFile(zip);
		while (status == UNZ_OK) {
			auto info = unz_file_info64();
			char name[2048] = { 0 };
			if (unzGetCurrentFileInfo64(
					zip,
					&info,
					name,
					sizeof(name) - 1,
					nullptr,
					0,
					nullptr,
					0) != UNZ_OK) {
				break;
			}
			const auto entry = QString::fromUtf8(name);
			const auto matched = (pass == 0)
				? preferredNames.contains(entry)
				: entry.endsWith(extension, Qt::CaseInsensitive);
			if (matched
				&& unzOpenCurrentFile(zip) == UNZ_OK) {
				auto data = QByteArray();
				auto buffer = QByteArray(kReadChunk, 0);
				while (true) {
					const auto read = unzReadCurrentFile(
						zip,
						buffer.data(),
						kReadChunk);
					if (read < 0) {
						break;
					} else if (!read) {
						break;
					}
					data.append(buffer.data(), int(read));
				}
				unzCloseCurrentFile(zip);
				if (entryName) {
					*entryName = entry;
				}
				return data;
			}
			status = unzGoToNextFile(zip);
		}
	}
	return std::nullopt;
}

[[nodiscard]] QString InstalledBaseName(const QString &path) {
	auto base = QFileInfo(path).completeBaseName();
	base.replace(QRegularExpression(u"[^\\w\\-. ]"_q), u"_"_q);
	if (base.isEmpty()) {
		base = u"plugin"_q;
	}
	return base;
}

void ReportInstallResult(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	controller->showToast(text);
}

} // namespace

void InstallPluginFromFile(
		not_null<Window::SessionController*> controller,
		const QString &path) {
	const auto manager = &Manager::Instance();
	const auto folder = manager->pluginsFolder();
	QDir().mkpath(folder);
	const auto base = InstalledBaseName(path);
	const auto suffix = QFileInfo(path).suffix().toLower();
	auto target = QString();
	auto warnings = QStringList();

	if (suffix == u"js"_q) {
		target = base + u".js"_q;
		if (!QFile::copy(path, QDir(folder).filePath(target))) {
			ReportInstallResult(
				controller,
				u"Не удалось скопировать файл"_q);
			return;
		}
	} else if (suffix == u"jplugin"_q) {
		auto entry = QString();
		const auto data = ReadZipEntry(
			path,
			{ u"plugin.js"_q, u"main.js"_q },
			u".js"_q,
			&entry);
		if (!data) {
			ReportInstallResult(
				controller,
				u"В пакете не найден plugin.js"_q);
			return;
		}
		target = base + u".js"_q;
		QFile out(QDir(folder).filePath(target));
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
			|| out.write(*data) != data->size()) {
			ReportInstallResult(
				controller,
				u"Не удалось сохранить плагин"_q);
			return;
		}
	} else if (suffix == u"plugin"_q) {
		auto entry = QString();
		const auto data = ReadZipEntry(
			path,
			{ u"main.py"_q, u"plugin.py"_q },
			u".py"_q,
			&entry);
		if (!data) {
			ReportInstallResult(
				controller,
				u"В пакете не найден python-файл плагина"_q);
			return;
		}
		const auto converted = ConvertExteraPlugin(
			QString::fromUtf8(*data),
			entry.isEmpty() ? u"main.py"_q : entry,
			&warnings);
		target = base + u".js"_q;
		QFile out(QDir(folder).filePath(target));
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
			|| out.write(converted.toUtf8()) != converted.toUtf8().size()) {
			ReportInstallResult(
				controller,
				u"Не удалось сохранить плагин"_q);
			return;
		}
		if (!warnings.isEmpty()) {
			QFile log(QDir(folder).filePath(
				base + u".converted.log"_q));
			if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				log.write(warnings.join(u'\n').toUtf8());
			}
		}
	} else {
		ReportInstallResult(
			controller,
			u"Неизвестный формат плагина: "_q + suffix);
		return;
	}

	manager->reload();
	auto text = u"Плагин установлен: "_q + target;
	if (!warnings.isEmpty()) {
		text += u"\nПредупреждений конвертера: "_q
			+ QString::number(warnings.size())
			+ u" (лог в папке плагинов)"_q;
	}
	controller->show(Ui::MakeInformBox({ .text = text }));
}

void RunConverter(not_null<Window::SessionController*> controller) {
	const auto path = QFileDialog::getOpenFileName(
		controller->window().widget(),
		u"Выберите плагин exteraGram (.py)"_q,
		QString(),
		u"Плагин exteraGram (*.py)"_q);
	if (path.isEmpty()) {
		return;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		controller->showToast(u"Не удалось открыть файл"_q);
		return;
	}
	auto warnings = QStringList();
	const auto converted = ConvertExteraPlugin(
		QString::fromUtf8(file.readAll()),
		QFileInfo(path).fileName(),
		&warnings);
	const auto folder = Manager::Instance().pluginsFolder();
	QDir().mkpath(folder);
	const auto base = InstalledBaseName(path);
	QFile out(QDir(folder).filePath(base + u".js"_q));
	if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| out.write(converted.toUtf8()) != converted.toUtf8().size()) {
		controller->showToast(u"Не удалось сохранить плагин"_q);
		return;
	}
	if (!warnings.isEmpty()) {
		QFile log(QDir(folder).filePath(base + u".converted.log"_q));
		if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			log.write(warnings.join(u'\n').toUtf8());
		}
	}
	Manager::Instance().reload();
	auto text = u"Конвертация завершена: "_q + base + u".js"_q;
	if (!warnings.isEmpty()) {
		text += u"\nПредупреждений: "_q + QString::number(warnings.size())
			+ u"\nЛог: "_q + base + u".converted.log"_q;
	}
	controller->show(Ui::MakeInformBox({ .text = text }));
}

} // namespace Plugins
