/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/cached_round_corners.h"
#include "ui/chat/chat_style.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/image/image_prepare.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_overview.h"
#include "styles/style_media_view.h"
#include "styles/style_chat_helpers.h"

namespace Ui {
namespace {

constexpr auto kCachedCornerRadiusCount = int(CachedCornerRadius::kCount);

std::vector<CornersPixmaps> Corners;
QImage CornersMaskLarge[4], CornersMaskSmall[4];
rpl::lifetime PaletteChangedLifetime;

std::array<std::array<QImage, 4>, kCachedCornerRadiusCount> CachedMasks;

[[nodiscard]] std::array<QImage, 4> PrepareCorners(int32 radius, const QBrush &brush, const style::color *shadow = nullptr) {
	int32 r = radius * style::DevicePixelRatio(), s = st::msgShadow * style::DevicePixelRatio();
	QImage rect(r * 3, r * 3 + (shadow ? s : 0), QImage::Format_ARGB32_Premultiplied);
	rect.fill(Qt::transparent);
	{
		auto p = QPainter(&rect);
		PainterHighQualityEnabler hq(p);

		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.setPen(Qt::NoPen);
		if (shadow) {
			p.setBrush((*shadow)->b);
			p.drawRoundedRect(0, s, r * 3, r * 3, r, r);
		}
		p.setBrush(brush);
		p.drawRoundedRect(0, 0, r * 3, r * 3, r, r);
	}
	auto result = std::array<QImage, 4>();
	result[0] = rect.copy(0, 0, r, r);
	result[1] = rect.copy(r * 2, 0, r, r);
	result[2] = rect.copy(0, r * 2, r, r + (shadow ? s : 0));
	result[3] = rect.copy(r * 2, r * 2, r, r + (shadow ? s : 0));
	return result;
}

void PrepareCorners(CachedRoundCorners index, int32 radius, const QBrush &brush, const style::color *shadow = nullptr) {
	Expects(index < Corners.size());

	auto images = PrepareCorners(radius, brush, shadow);
	for (int i = 0; i < 4; ++i) {
		Corners[index].p[i] = PixmapFromImage(std::move(images[i]));
		Corners[index].p[i].setDevicePixelRatio(style::DevicePixelRatio());
	}
}

void CreateMaskCorners() {
	auto mask = PrepareCorners(st::roundRadiusSmall, QColor(255, 255, 255), nullptr);
	for (int i = 0; i < 4; ++i) {
		CornersMaskSmall[i] = mask[i].convertToFormat(QImage::Format_ARGB32_Premultiplied);
		CornersMaskSmall[i].setDevicePixelRatio(style::DevicePixelRatio());
	}
	mask = PrepareCorners(st::roundRadiusLarge, QColor(255, 255, 255), nullptr);
	for (int i = 0; i < 4; ++i) {
		CornersMaskLarge[i] = mask[i].convertToFormat(QImage::Format_ARGB32_Premultiplied);
		CornersMaskLarge[i].setDevicePixelRatio(style::DevicePixelRatio());
	}
}

void CreatePaletteCorners() {
	PrepareCorners(MenuCorners, st::innerDropdownRadius, st::menuBg);
	PrepareCorners(BoxCorners, st::boxRadius, st::boxBg);
	PrepareCorners(DateCorners, st::dateRadius, st::msgDateImgBg);
	PrepareCorners(OverviewVideoCorners, st::overviewVideoStatusRadius, st::msgDateImgBg);
	PrepareCorners(OverviewVideoSelectedCorners, st::overviewVideoStatusRadius, st::msgDateImgBgSelected);
	PrepareCorners(ForwardCorners, st::roundRadiusLarge, st::historyForwardChooseBg);
	PrepareCorners(MediaviewSaveCorners, st::mediaviewControllerRadius, st::mediaviewSaveMsgBg);
	PrepareCorners(StickerHoverCorners, st::roundRadiusSmall, st::emojiPanHover);
	PrepareCorners(BotKeyboardCorners, st::roundRadiusSmall, st::botKbBg);

	PrepareCorners(Doc1Corners, st::roundRadiusSmall, st::msgFile1Bg);
	PrepareCorners(Doc2Corners, st::roundRadiusSmall, st::msgFile2Bg);
	PrepareCorners(Doc3Corners, st::roundRadiusSmall, st::msgFile3Bg);
	PrepareCorners(Doc4Corners, st::roundRadiusSmall, st::msgFile4Bg);
}

} // namespace

void StartCachedCorners() {
	Corners.resize(RoundCornersCount);
	CreateMaskCorners();
	CreatePaletteCorners();

	style::PaletteChanged(
	) | rpl::on_next([=] {
		CreatePaletteCorners();
	}, PaletteChangedLifetime);
}

void FinishCachedCorners() {
	Corners.clear();
	PaletteChangedLifetime.destroy();
}

void FillRoundRect(QPainter &p, int32 x, int32 y, int32 w, int32 h, style::color bg, const CornersPixmaps &corners) {
	using namespace Images;

	const auto fillBg = [&](QRect rect) {
		p.fillRect(rect, bg);
	};
	const auto fillCorner = [&](int x, int y, int index) {
		if (const auto &pix = corners.p[index]; !pix.isNull()) {
			p.drawPixmap(x, y, pix);
		}
	};

	if (corners.p[kTopLeft].isNull()
		&& corners.p[kTopRight].isNull()
		&& corners.p[kBottomLeft].isNull()
		&& corners.p[kBottomRight].isNull()) {
		p.fillRect(x, y, w, h, bg);
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto cornerSize = [&](int index) {
		return corners.p[index].isNull()
			? 0
			: (corners.p[index].width() / ratio);
	};
	const auto verticalSkip = [&](int left, int right) {
		return std::max(cornerSize(left), cornerSize(right));
	};
	const auto top = verticalSkip(kTopLeft, kTopRight);
	const auto bottom = verticalSkip(kBottomLeft, kBottomRight);
	const auto leftMax = std::max(cornerSize(kTopLeft), cornerSize(kBottomLeft));
	const auto rightMax = std::max(cornerSize(kTopRight), cornerSize(kBottomRight));
	if (w > 0 && h > 0 && (h < top + bottom || w < leftMax + rightMax)) {
		// m3eGram (M3 Expressive): the corner radius is larger than half of
		// this element, so the corner strips would overlap and a translucent
		// fill would show a lighter band. Draw one path with clamped radii.
		const auto limit = std::min(w, h) / 2.;
		const auto radius = [&](int index) {
			return std::min<qreal>(cornerSize(index), limit);
		};
		const auto tl = radius(kTopLeft);
		const auto tr = radius(kTopRight);
		const auto bl = radius(kBottomLeft);
		const auto br = radius(kBottomRight);
		auto path = QPainterPath();
		path.moveTo(x + tl, y);
		path.lineTo(x + w - tr, y);
		if (tr > 0) {
			path.arcTo(QRectF(x + w - 2 * tr, y, 2 * tr, 2 * tr), 90, -90);
		}
		path.lineTo(x + w, y + h - br);
		if (br > 0) {
			path.arcTo(QRectF(x + w - 2 * br, y + h - 2 * br, 2 * br, 2 * br), 0, -90);
		}
		path.lineTo(x + bl, y + h);
		if (bl > 0) {
			path.arcTo(QRectF(x, y + h - 2 * bl, 2 * bl, 2 * bl), 270, -90);
		}
		path.lineTo(x, y + tl);
		if (tl > 0) {
			path.arcTo(QRectF(x, y, 2 * tl, 2 * tl), 180, -90);
		}
		path.closeSubpath();
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(bg->b);
		p.drawPath(path);
		return;
	}
	if (top) {
		const auto left = cornerSize(kTopLeft);
		const auto right = cornerSize(kTopRight);
		if (left) {
			fillCorner(x, y, kTopLeft);
			if (const auto add = top - left) {
				fillBg({ x, y + left, left, add });
			}
		}
		if (const auto fill = w - left - right; fill > 0) {
			fillBg({ x + left, y, fill, top });
		}
		if (right) {
			fillCorner(x + w - right, y, kTopRight);
			if (const auto add = top - right) {
				fillBg({ x + w - right, y + right, right, add });
			}
		}
	}
	if (const auto fill = h - top - bottom; fill > 0) {
		fillBg({ x, y + top, w, fill });
	}
	if (bottom) {
		const auto left = cornerSize(kBottomLeft);
		const auto right = cornerSize(kBottomRight);
		if (left) {
			fillCorner(x, y + h - left, kBottomLeft);
			if (const auto add = bottom - left) {
				fillBg({ x, y + h - bottom, left, add });
			}
		}
		if (const auto fill = w - left - right; fill > 0) {
			fillBg({ x + left, y + h - bottom, fill, bottom });
		}
		if (right) {
			fillCorner(x + w - right, y + h - right, kBottomRight);
			if (const auto add = bottom - right) {
				fillBg({ x + w - right, y + h - bottom, right, add });
			}
		}
	}
}

void FillRoundRect(QPainter &p, int32 x, int32 y, int32 w, int32 h, style::color bg, CachedRoundCorners index) {
	FillRoundRect(p, x, y, w, h, bg, CachedCornerPixmaps(index));
}

void FillRoundShadow(QPainter &p, int32 x, int32 y, int32 w, int32 h, style::color shadow, const CornersPixmaps &corners) {
	constexpr auto kLeft = 2;
	constexpr auto kRight = 3;

	const auto ratio = style::DevicePixelRatio();
	const auto size = [&](int index) {
		const auto &pix = corners.p[index];
		return pix.isNull() ? 0 : (pix.width() / ratio);
	};
	const auto fillCorner = [&](int left, int bottom, int index) {
		const auto &pix = corners.p[index];
		if (pix.isNull()) {
			return;
		}
		const auto size = pix.width() / ratio;
		p.drawPixmap(left, bottom - size, pix);
	};
	const auto left = size(kLeft);
	const auto right = size(kRight);
	const auto from = x + left;
	fillCorner(x, y + h + st::msgShadow, kLeft);
	if (const auto width = w - left - right; width > 0) {
		p.fillRect(from, y + h, width, st::msgShadow, shadow);
	}
	fillCorner(x + w - right, y + h + st::msgShadow, kRight);
}

const CornersPixmaps &CachedCornerPixmaps(CachedRoundCorners index) {
	Expects(index >= 0 && index < RoundCornersCount);

	return Corners[index];
}

CornersPixmaps PrepareCornerPixmaps(int radius, style::color bg, const style::color *sh) {
	auto images = PrepareCorners(radius, bg, sh);
	auto result = CornersPixmaps();
	for (int j = 0; j < 4; ++j) {
		result.p[j] = PixmapFromImage(std::move(images[j]));
		result.p[j].setDevicePixelRatio(style::DevicePixelRatio());
	}
	return result;
}

CornersPixmaps PrepareCornerPixmaps(ImageRoundRadius radius, style::color bg, const style::color *sh) {
	switch (radius) {
	case ImageRoundRadius::Small:
		return PrepareCornerPixmaps(st::roundRadiusSmall, bg, sh);
	case ImageRoundRadius::Large:
		return PrepareCornerPixmaps(st::roundRadiusLarge, bg, sh);
	}
	Unexpected("Image round radius in PrepareCornerPixmaps.");
}

CornersPixmaps PrepareInvertedCornerPixmaps(int radius, style::color bg) {
	const auto size = radius * style::DevicePixelRatio();
	auto circle = style::colorizeImage(
		style::createInvertedCircleMask(radius * 2),
		bg);
	circle.setDevicePixelRatio(style::DevicePixelRatio());
	auto result = CornersPixmaps();
	const auto fill = [&](int index, int xoffset, int yoffset) {
		result.p[index] = PixmapFromImage(
			circle.copy(QRect(xoffset, yoffset, size, size)));
	};
	fill(0, 0, 0);
	fill(1, size, 0);
	fill(2, size, size);
	fill(3, 0, size);
	return result;
}

[[nodiscard]] int CachedCornerRadiusValue(CachedCornerRadius tag) {
	using Radius = CachedCornerRadius;
	switch (tag) {
	case Radius::Small: return st::roundRadiusSmall;
	case Radius::ThumbSmall: return MsgFileThumbRadiusSmall();
	case Radius::ThumbLarge: return MsgFileThumbRadiusLarge();
	case Radius::BubbleSmall: return BubbleRadiusSmall();
	case Radius::BubbleLarge: return BubbleRadiusLarge();
	}
	Unexpected("Radius tag in CachedCornerRadiusValue.");
}

[[nodiscard]] const std::array<QImage, 4> &CachedCornersMasks(
		CachedCornerRadius radius) {
	const auto index = static_cast<int>(radius);
	Assert(index >= 0 && index < kCachedCornerRadiusCount);

	if (CachedMasks[index][0].isNull()) {
		CachedMasks[index] = Images::CornersMask(
			CachedCornerRadiusValue(CachedCornerRadius(index)));
	}
	return CachedMasks[index];
}

} // namespace Ui
