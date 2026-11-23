// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QImage>
#include <QQuickImageProvider>

namespace oviora::engine { struct SubtitleSample; }

namespace oviora::ui {

/**
 * The stream menu's live backdrop: the newest small picture of the strip under the drawer (QmlPlayerBridge,
 * engine backdropTap), served to QML as image://oviora-menu/<serial>.
 */
class MenuBackdropStore
{
public:
	/** A region copy of the processed frame cut down to a small blurred RGB picture (~240 px wide); null when the format is unknown */
	static QImage toSmall(const engine::SubtitleSample &sample);
	static void set(const QImage &image);
	static QImage get();
};

class MenuBackdropProvider : public QQuickImageProvider
{
public:
	MenuBackdropProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
	QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace oviora::ui
