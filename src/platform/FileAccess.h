// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QString>
#include <QUrl>

namespace yolo::platform {
QString cachePdf(const QUrl &source);
QString cacheAsset(const QUrl &source, const QString &suffix);
void saveCachedPdf(const QString &cache, const QUrl &destination);
} // namespace yolo::platform
