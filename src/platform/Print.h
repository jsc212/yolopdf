// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "engine/api/Document.h"
#include <QString>
#include <QVariantList>
namespace yolo::platform {
struct PrintBundle {
    QString folder, pdf;
    QVariantList pages;
};
PrintBundle preparePrint(engine::IDocument &, const QString &title);
void previewPrint(const PrintBundle &, bool fit);
} // namespace yolo::platform
