/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/panel_backdrop.h"

namespace Ui {

void PanelBackdrop::setCapture(not_null<RpWidget*> owner, Fn<QImage()> capture) {
	_owner = owner;
	_capture = std::move(capture);
	if (_capture) {
		// The capture lambda resolves _capture at call time, but the owner
		// is baked into LayerBackdrop - recreate it if the owner changed
		// (e.g. the subsection tabs widget is rebuilt on a mode switch).
		if (!_backdrop || _createdFor != owner) {
			_backdrop.emplace(
				owner,
				[=] {
					return _capture ? _capture() : QImage();
				},
				[](bool) { return 0; });
			_createdFor = owner;
		} else {
			_backdrop->invalidate();
		}
	} else {
		_backdrop.reset();
	}
}

void PanelBackdrop::invalidate() {
	if (_backdrop) {
		_backdrop->invalidate();
	}
}

bool PanelBackdrop::ensureStarted() {
	if (!_backdrop || !_owner) {
		return false;
	}
	// start() captures and blurs synchronously once; later refreshes go
	// through the debounced async path of LayerBackdrop.
	if (!_backdrop->started() && !_owner->size().isEmpty()) {
		_backdrop->start();
	}
	return _backdrop->started();
}

void PanelBackdrop::paint(QPainter &p) const {
	if (_backdrop) {
		_backdrop->paint(p);
	}
}

PanelBackdropWidget::PanelBackdropWidget(not_null<QWidget*> parent)
: RpWidget(parent) {
}

void PanelBackdropWidget::setCapture(Fn<QImage()> capture) {
	_backdrop.setCapture(this, std::move(capture));
	update();
}

void PanelBackdropWidget::setTint(const style::color &tint, int alpha) {
	_tint = tint;
	_tintAlpha = alpha;
	update();
}

void PanelBackdropWidget::setContentPainter(ContentPainter painter) {
	_contentPainter = std::move(painter);
	update();
}

void PanelBackdropWidget::invalidate() {
	_backdrop.invalidate();
}

void PanelBackdropWidget::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	// The frosted backdrop appears once the first capture+blur is ready;
	// until then (and whenever the capture is not available) the tint and
	// the content painter still run, so the panel never loses its content.
	if (_backdrop.ensureStarted()) {
		_backdrop.paint(p);
	}
	if (_tint && _tintAlpha > 0) {
		auto color = (*_tint)->c;
		color.setAlpha(_tintAlpha);
		p.fillRect(rect(), color);
	}
	if (_contentPainter) {
		// The content painter works in parent widget coordinates, matching
		// the old parent paintEvent painting of this area.
		const auto offset = mapTo(parentWidget(), QPoint());
		p.translate(offset);
		_contentPainter(p, QRect(offset, size()));
	}
}

} // namespace Ui
