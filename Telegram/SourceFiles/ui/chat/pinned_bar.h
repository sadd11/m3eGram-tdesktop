/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/wrap/slide_wrap.h"
#include "ui/effects/panel_backdrop.h" // m3eGram
#include "base/object_ptr.h"

namespace Ui {

struct MessageBarContent;
template <typename Widget>
class FadeWrapScaled;
class MessageBar;
class IconButton;
class PlainShadow;
class RpWidget;

class PinnedBar final {
public:
	PinnedBar(
		not_null<QWidget*> parent,
		Fn<bool()> customEmojiPaused,
		rpl::producer<> customEmojiPausedChanges);
	~PinnedBar();

	void show();
	void hide();
	void raise();
	void customEmojiRepaint();
	void finishAnimating();

	void setShadowGeometryPostprocess(Fn<QRect(QRect)> postprocess);

	void setContent(rpl::producer<Ui::MessageBarContent> content);
	void setRightButton(object_ptr<Ui::RpWidget> button);

	void move(int x, int y);
	void resizeToWidth(int width);
	[[nodiscard]] int height() const;
	[[nodiscard]] rpl::producer<int> heightValue() const;
	[[nodiscard]] rpl::producer<> barClicks() const;
	[[nodiscard]] rpl::producer<> barRightClicks() const;
	[[nodiscard]] rpl::producer<> contextMenuRequested() const;

	// m3eGram: frosted backdrop of the list under the translucent bar.
	// The factory renders the content underneath the given panel widget.
	void setBackdropFactory(Fn<QImage(not_null<QWidget*> panel)> factory);
	void invalidateBackdrop();

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _wrap.lifetime();
	}

private:
	using RightButton = object_ptr<Ui::FadeWrapScaled<Ui::RpWidget>>;
	void createControls();
	void updateShadowGeometry(QRect wrapGeometry);
	void updateControlsGeometry(QRect wrapGeometry);

	Ui::SlideWrap<> _wrap;
	std::unique_ptr<Ui::MessageBar> _bar;

	// m3eGram: frosted backdrop of the list under the translucent bar.
	Ui::PanelBackdrop _backdrop;
	Fn<QImage(not_null<QWidget*> panel)> _backdropFactory;

	struct {
		RightButton button = { nullptr };
		rpl::lifetime previousButtonLifetime;
	} _right;

	std::unique_ptr<Ui::PlainShadow> _shadow;
	Fn<bool()> _customEmojiPaused;
	rpl::event_stream<> _barClicks;
	rpl::event_stream<> _barRightClicks;
	rpl::event_stream<> _contextMenuRequested;
	Fn<QRect(QRect)> _shadowGeometryPostprocess;
	bool _shouldBeShown = false;
	bool _forceHidden = false;

	rpl::lifetime _contentLifetime;

};

} // namespace Ui
