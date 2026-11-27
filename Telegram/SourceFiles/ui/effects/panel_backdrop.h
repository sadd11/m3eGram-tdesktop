/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/effects/layer_backdrop.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core.h"

namespace Ui {

// m3eGram: owning wrapper around LayerBackdrop with a late-bound capture
// lambda, for the frosted-glass panels over the full-height message list.
class PanelBackdrop final {
public:
	// Binds the owner widget and the capture lambda (a null lambda clears
	// the backdrop). The lambda must return an image of exactly the owner
	// widget's size * devicePixelRatio (Format_ARGB32_Premultiplied, with
	// the device pixel ratio set), or a null image while it is not ready.
	void setCapture(not_null<RpWidget*> owner, Fn<QImage()> capture);
	void invalidate();

	// Starts the synchronous capture+blur once, when the owner has a real
	// size. Returns whether the backdrop has something to paint.
	[[nodiscard]] bool ensureStarted();
	void paint(QPainter &p) const;

private:
	std::optional<Ui::LayerBackdrop> _backdrop;
	Fn<QImage()> _capture;
	RpWidget *_owner = nullptr;
	RpWidget *_createdFor = nullptr;

};

// A widget variant: paints the blurred backdrop of the content underneath,
// an optional translucent tint on top, and optionally delegates its own
// content drawing (in parent widget coordinates) to a callback painted last.
class PanelBackdropWidget final : public RpWidget {
public:
	using ContentPainter = Fn<void(Painter &p, QRect clip)>;

	PanelBackdropWidget(not_null<QWidget*> parent);

	void setCapture(Fn<QImage()> capture);
	void setTint(const style::color &tint, int alpha);
	void setContentPainter(ContentPainter painter);
	void invalidate();

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	PanelBackdrop _backdrop;
	std::optional<style::color> _tint;
	int _tintAlpha = 0;
	ContentPainter _contentPainter;

};

} // namespace Ui
