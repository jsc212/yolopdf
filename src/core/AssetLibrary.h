// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <functional>
namespace yolo::core {
class AssetLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList stamps READ stamps NOTIFY changed)
    Q_PROPERTY(QVariantList fonts READ fonts NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
  public:
    explicit AssetLibrary(QString root = {}, QObject *parent = nullptr);
    QVariantList stamps() const;
    QVariantList fonts() const;
    QString error() const { return error_; }
    bool busy() const { return busy_; }
    Q_INVOKABLE void importStamp(const QUrl &, const QString &name, double width, double height,
                                 bool aspectLocked = true, int frame = 0);
    Q_INVOKABLE void importFont(const QUrl &);
    Q_INVOKABLE void remove(const QString &kind, const QString &id);
    Q_INVOKABLE void exportStamps(const QUrl &);
    Q_INVOKABLE void importStamps(const QUrl &);
  signals:
    void changed();
    void stampImported(QString id);
    void fontImported(QUrl file);
    void operationFinished(bool success);

  private:
    struct Result {
        QString error, id;
        QUrl file;
    };
    void run(std::function<Result()>);
    QString importStampNow(const QUrl &, const QString &, double, double, bool, int);
    QUrl importFontNow(const QUrl &);
    void removeNow(const QString &, const QString &);
    void exportStampsNow(const QUrl &);
    void importStampsNow(const QUrl &);
    QVariantList list(const QString &kind) const;
    QString root_, error_;
    bool busy_{};
};
} // namespace yolo::core
