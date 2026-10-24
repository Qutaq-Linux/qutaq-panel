#pragma once

#include <QObject>
#include <QString>

class QMenu;

// Common interface for menu sources: com.canonical.dbusmenu (Qt, Electron, libdbusmenu)
// and org.gtk.Menus + org.gtk.Actions (GTK). rootMenu() holds the top-level items.
class MenuImporter : public QObject
{
    Q_OBJECT
public:
    MenuImporter(const QString &service, const QString &path, QObject *parent = nullptr)
        : QObject(parent), m_service(service), m_path(path) {}

    virtual QMenu *rootMenu() const = 0;
    virtual void refresh() = 0; // re-read the whole tree

    QString service() const { return m_service; }
    QString path() const { return m_path; }

Q_SIGNALS:
    void menuUpdated(); // the root was rebuilt
    void invalid();     // the menu is unavailable

protected:
    QString m_service;
    QString m_path;
};
