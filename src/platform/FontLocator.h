// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QString>
namespace yolo::platform {
struct FontMatch {
    QString file;
    int faceIndex{};
};
// Exact PostScript-name matching only. A substitute is never returned as exact.
FontMatch findExactFont(QString postscriptName);
} // namespace yolo::platform
