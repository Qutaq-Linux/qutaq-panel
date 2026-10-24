#pragma once

#include <QObject>
#include <QString>

class QMenu;

// Общий интерфейс для источников меню: com.canonical.dbusmenu (Qt, Electron, libdbusmenu)
// и org.gtk.Menus + org.gtk.Actions (GTK). rootMenu() содержит пункты верхнего уровня.
class MenuImporter : public QObject
{
    Q_OBJECT
public:
    MenuImporter(const QString &service, const QString &path, QObject *parent = nullptr)
        : QObject(parent), m_service(service), m_path(path) {}

    virtual QMenu *rootMenu() const = 0;
    virtual void refresh() = 0; // перечитать всё дерево

    QString service() const { return m_service; }
    QString path() const { return m_path; }

Q_SIGNALS:
    void menuUpdated(); // корень перестроен
    void invalid();     // меню недоступно

protected:
    QString m_service;
    QString m_path;
};
