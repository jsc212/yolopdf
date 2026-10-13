// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QByteArray>
#include <QMap>
namespace yolo::core {
using ArchiveEntries = QMap<QString, QByteArray>;
QByteArray writeArchive(const ArchiveEntries &);
ArchiveEntries readArchive(const QByteArray &);
} // namespace yolo::core
