// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
namespace yolo::core {
class DropRouter final : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    Q_INVOKABLE QVariantMap route(const QString &target, bool hasDocument, bool internal, int count,
                                  int hovered, bool after, const QVariantList &files) const;
};
} // namespace yolo::core
