/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_plugins.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "plugins/plugins_converter.h"
#include "plugins/plugins_manager.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtGui/QCursor>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QFileDialog>

namespace Settings {
namespace {

using namespace Builder;

class PluginsSection final : public Section<PluginsSection> {
public:
	PluginsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void rebuildList();
	void showDetail(const QString &fileName);
	void hideDetail();
	void relayout();

	Ui::VerticalLayout *_content = nullptr;
	Ui::VerticalLayout *_list = nullptr;
	Ui::VerticalLayout *_actions = nullptr;
	Ui::VerticalLayout *_detail = nullptr;
	QString _detailFileName;
	rpl::lifetime _changedLifetime;

};

PluginsSection::PluginsSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> PluginsSection::title() {
	return tr::lng_m3e_plugins_title();
}

void PluginsSection::rebuildList() {
	if (!_content) {
		return;
	}
	if (_list) {
		_list->deleteLater();
	}
	_list = Ui::CreateChild<Ui::VerticalLayout>(_content);
	const auto manager = &Plugins::Manager::Instance();
	Ui::AddSkip(_list);
	Ui::AddSubsectionTitle(_list, tr::lng_m3e_plugins_available());

	const auto &descriptors = manager->descriptors();
	if (descriptors.empty()) {
		Ui::AddDividerText(_list, tr::lng_m3e_plugins_empty());
	} else {
		for (const auto &descriptor : descriptors) {
			const auto fileName = descriptor.fileName;
			auto title = descriptor.meta.name;
			if (!descriptor.meta.version.isEmpty()) {
				title += u" (%1)"_q.arg(descriptor.meta.version);
			}
			const auto button = AddButtonWithIcon(
				_list,
				rpl::single(title),
				st::settingsButtonNoIcon,
				{ &st::menuIconManage });
			button->toggleOn(rpl::single(descriptor.state.enabled));
			button->toggledValue(
			) | rpl::on_next([=](bool enabled) {
				manager->setEnabled(fileName, enabled);
			}, button->lifetime());

			auto subtitle = descriptor.meta.author;
			if (!subtitle.isEmpty()) {
				subtitle += u" · "_q;
			}
			subtitle += descriptor.meta.description;
			if (subtitle.isEmpty()) {
				subtitle = fileName;
			}
			const auto info = AddButtonWithIcon(
				_list,
				rpl::single(subtitle),
				st::settingsButtonNoIcon,
				{ &st::menuIconManage });
			info->setClickedCallback([=] {
				showDetail(fileName);
			});
			if (descriptor.loadFailed && !descriptor.error.isEmpty()) {
				const auto error = _list->add(object_ptr<Ui::FlatLabel>(
					_list,
					u"%1: %2"_q.arg(
						tr::lng_m3e_plugins_load_error(tr::now),
						descriptor.error),
					st::defaultFlatLabel));
				error->setTextColorOverride(st::boxTextFgError->c);
				error->setWordWrap(true);
			}
			Ui::AddDivider(_list);
		}
	}
	relayout();
}

void PluginsSection::showDetail(const QString &fileName) {
	if (!_content) {
		return;
	}
	_detailFileName = fileName;
	if (_detail) {
		_detail->deleteLater();
	}
	_detail = Ui::CreateChild<Ui::VerticalLayout>(_content);
	const auto manager = &Plugins::Manager::Instance();
	Ui::AddSkip(_detail);
	const auto back = AddButtonWithIcon(
		_detail,
		tr::lng_m3e_plugins_back(),
		st::settingsButtonNoIcon,
		{ &st::menuIconSettings });
	back->setClickedCallback([=] {
		hideDetail();
	});

	const auto rows = manager->settingsRows(fileName);
	for (const auto &rowVariant : rows) {
		const auto row = rowVariant.toMap();
		const auto type = row.value(u"type"_q).toString();
		const auto key = row.value(u"key"_q).toString();
		const auto text = row.value(u"text"_q).toString();
		const auto def = row.value(u"default"_q);
		const auto callbackId = row
			.value(u"on_change"_q).toMap()
			.value(u"__callback"_q).toString();
		if (type == u"header") {
			Ui::AddSkip(_detail);
			Ui::AddSubsectionTitle(
				_detail,
				rpl::single(row.value(u"text"_q).toString()));
		} else if (type == u"divider") {
			Ui::AddDivider(_detail);
		} else if (type == u"switch") {
			const auto value = manager->settingValue(
				fileName,
				key,
				def.toBool());
			const auto button = AddButtonWithIcon(
				_detail,
				rpl::single(text),
				st::settingsButtonNoIcon,
				{ &st::menuIconManage });
			button->toggleOn(rpl::single(value.toBool()));
			button->toggledValue(
			) | rpl::on_next([=](bool toggled) {
				manager->settingChanged(fileName, key, toggled);
				if (!callbackId.isEmpty()) {
					manager->fireCallback(fileName, callbackId, toggled);
				}
			}, button->lifetime());
		} else if (type == u"selector") {
			const auto items = row.value(u"items"_q).toList();
			const auto index = manager->settingValue(
				fileName,
				key,
				def.isValid() ? def : QVariant(0)).toInt();
			const auto current = (index >= 0 && index < items.size())
				? items[index].toString()
				: text;
			const auto button = AddButtonWithLabel(
				_detail,
				rpl::single(text),
				rpl::single(current),
				st::settingsButtonNoIcon);
			button->setClickedCallback([=] {
				const auto menu = Ui::CreateChild<Ui::PopupMenu>(
					this,
					st::popupMenuWithIcons);
				auto n = 0;
				for (const auto &item : items) {
					const auto value = n++;
					menu->addAction(item.toString(), [=] {
						manager->settingChanged(fileName, key, value);
						if (!callbackId.isEmpty()) {
							manager->fireCallback(
								fileName,
								callbackId,
								value);
						}
						showDetail(fileName);
					});
				}
				menu->popup(QCursor::pos());
			});
		} else if (type == u"input" || type == u"edittext") {
			const auto value = manager->settingValue(fileName, key, def);
			Ui::AddSkip(_detail);
			_detail->add(object_ptr<Ui::FlatLabel>(
				_detail,
				rpl::single(text.isEmpty() ? key : text),
				st::defaultFlatLabel));
			const auto field = _detail->add(object_ptr<Ui::InputField>(
				_detail,
				st::defaultInputField,
				row.value(u"hint"_q).toString(),
				value.toString()));
			if (type == u"edittext") {
				field->setMinimumHeight(st::defaultInputField.heightMin * 2);
			}
			field->changes(
			) | rpl::start_with_next([=] {
				manager->settingChanged(fileName, key, field->getLastText());
				if (!callbackId.isEmpty()) {
					manager->fireCallback(
						fileName,
						callbackId,
						field->getLastText());
				}
			}, field->lifetime());
			Ui::AddSkip(_detail);
		} else if (type == u"text") {
			const auto onClickCallback = row
				.value(u"on_click"_q).toMap()
				.value(u"__callback"_q).toString();
			const auto button = AddButtonWithIcon(
				_detail,
				rpl::single(row.value(u"text"_q).toString()),
				st::settingsButtonNoIcon,
				{ &st::menuIconManage });
			button->setClickedCallback([=] {
				if (!onClickCallback.isEmpty()) {
					manager->fireCallback(
						fileName,
						onClickCallback,
						QVariant());
				}
			});
		}
	}
	Ui::AddSkip(_detail);
	relayout();
}

void PluginsSection::hideDetail() {
	_detailFileName = QString();
	if (_detail) {
		_detail->deleteLater();
		_detail = nullptr;
	}
	relayout();
}

void PluginsSection::relayout() {
	if (!_content) {
		return;
	}
	const auto width = _content->width();
	const auto detailShown = _detail && !_detail->isHidden();
	if (_list) {
		_list->resizeToWidth(width);
		_list->moveToLeft(0, 0);
		_list->setVisible(!detailShown);
	}
	if (_actions) {
		_actions->resizeToWidth(width);
		_actions->setVisible(!detailShown);
	}
	auto height = 0;
	if (!detailShown && _list) {
		_list->moveToLeft(0, 0);
		height = _list->height();
		if (_actions) {
			_actions->moveToLeft(0, height);
			height += _actions->height();
		}
	}
	if (detailShown && _detail) {
		_detail->moveToLeft(0, 0);
		height = _detail->height();
	}
	_content->resize(width, height);
}

void PluginsSection::setupContent() {
	const auto manager = &Plugins::Manager::Instance();
	_content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto controller = this->controller();

	_list = Ui::CreateChild<Ui::VerticalLayout>(_content);
	_actions = Ui::CreateChild<Ui::VerticalLayout>(_content);
	_detail = nullptr;

	// Action buttons (outside the rebuildable list).
	AddButtonWithIcon(
		_actions,
		tr::lng_m3e_plugins_reload(),
		st::settingsButtonNoIcon,
		{ &st::menuIconManage }
	)->setClickedCallback([=] {
		manager->reload();
		hideDetail();
		controller->showToast(tr::lng_m3e_plugins_reload_done(tr::now));
	});
	AddButtonWithIcon(
		_actions,
		tr::lng_m3e_plugins_open_folder(),
		st::settingsButtonNoIcon,
		{ &st::menuIconShowInFolder }
	)->setClickedCallback([=] {
		QDesktopServices::openUrl(QUrl::fromLocalFile(
			manager->pluginsFolder()));
	});
	AddButtonWithIcon(
		_actions,
		tr::lng_m3e_plugins_convert(),
		st::settingsButtonNoIcon,
		{ &st::menuIconEdit }
	)->setClickedCallback([=] {
		Plugins::RunConverter(controller);
	});
	AddButtonWithIcon(
		_actions,
		tr::lng_m3e_plugins_install(),
		st::settingsButtonNoIcon,
		{ &st::menuIconAdd }
	)->setClickedCallback([=] {
		const auto path = QFileDialog::getOpenFileName(
			controller->window().widget(),
			tr::lng_m3e_plugins_install(tr::now),
			QString(),
			u"Плагины (*.jplugin *.plugin *.js)"_q);
		if (path.isEmpty()) {
			return;
		}
		Plugins::InstallPluginFromFile(controller, path);
	});

	rebuildList();

	manager->changed(
	) | rpl::start_with_next([=] {
		rebuildList();
		if (!_detailFileName.isEmpty()) {
			showDetail(_detailFileName);
		}
	}, _changedLifetime);

	_content->geometryValue(
	) | rpl::start_with_next([=](QRect) {
		relayout();
	}, _content->lifetime());

	Ui::ResizeFitChild(this, _content);
	relayout();
}

const auto kMeta = BuildHelper({
	.id = PluginsSection::Id(),
	.parentId = MainId(),
	.title = &tr::lng_m3e_plugins_title,
	.icon = &st::menuIconManage,
}, [](SectionBuilder &builder) {
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"plugins/open-folder"_q,
			.title = tr::lng_m3e_plugins_open_folder(tr::now),
			.keywords = { u"plugin"_q, u"plugins"_q, u"плагины"_q, u"папка"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"plugins/reload"_q,
			.title = tr::lng_m3e_plugins_reload(tr::now),
			.keywords = { u"plugin"_q, u"reload"_q, u"перезагрузить"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"plugins/convert"_q,
			.title = tr::lng_m3e_plugins_convert(tr::now),
			.keywords = { u"convert"_q, u"extera"_q, u"конвертировать"_q },
		};
	});
});

} // namespace

Type PluginsId() {
	return PluginsSection::Id();
}

} // namespace Settings
