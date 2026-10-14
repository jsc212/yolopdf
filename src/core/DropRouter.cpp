// SPDX-License-Identifier: AGPL-3.0-only
#include "DropRouter.h"
#include <algorithm>
namespace yolo::core {
QVariantMap DropRouter::route(const QString &target, bool hasDocument, bool internal, int count,
                              int hovered, bool after, const QVariantList &files) const {
    const int at = hovered < 0 ? std::max(count, 0)
                               : std::clamp(hovered + (after ? 1 : 0), 0, std::max(count, 0));
    QString action = "ignore", overlay;
    if (internal) {
        if (hasDocument && target == "overview")
            action = "reorder";
    } else if (!files.isEmpty()) {
        action = hasDocument && target == "overview" ? "insert" : "open";
        overlay = action == "insert" ? "Insert pages here" : "Open as document";
    }
    return {{"action", action}, {"at", at}, {"overlay", overlay}, {"files", files}};
}
} // namespace yolo::core
