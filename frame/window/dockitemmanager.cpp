/*
 * Copyright (C) 2019 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     wangshaojun <wangshaojun_cm@deepin.com>
 *
 * Maintainer: wangshaojun <wangshaojun_cm@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dockitemmanager.h"
#include "../item/appitem.h"
#include "../item/trashitem.h"

#include <QSet>

DockItemManager::DockItemManager() : QObject()
    , m_qsettings(new QSettings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/setting.ini", QSettings::IniFormat))
{
    m_qsettings->setIniCodec(QTextCodec::codecForName("UTF-8"));
}


DockItemManager *DockItemManager::instance()
{
    static DockItemManager INSTANCE;
    return &INSTANCE;
}

void DockItemManager::setDbusDock(DBusDock *dbus) {
    m_appInter = dbus;
    connect(m_appInter, &DBusDock::EntryAdded, this, [this](const QDBusObjectPath &path, int index){ appItemAdded(path, index);});
    connect(m_appInter, &DBusDock::EntryRemoved, this, static_cast<void (DockItemManager::*)(const QString &)>(&DockItemManager::appItemRemoved), Qt::QueuedConnection);
    connect(m_appInter, &DBusDock::ServiceRestarted, this, [ this ] { QTimer::singleShot(500, [ this ] { reloadAppItems(); }); });
}

MergeMode DockItemManager::getDockMergeMode()
{
    int i = m_qsettings->value("mergeMode", MergeDock).toInt();
    if(i < 0 || i > 1)
        i = 0;
    return MergeMode(i);
}

void DockItemManager::saveDockMergeMode(MergeMode mode)
{
    if(mode != getDockMergeMode())
    {
        m_qsettings->setValue("mergeMode", mode);
        m_qsettings->sync();
        emit mergeModeChanged(mode);
    }
}

bool DockItemManager::isEnableHoverScaleAnimation()
{
    return m_qsettings->value("animation/hover", true).toBool();
}

bool DockItemManager::isEnableInOutAnimation()
{
    return m_qsettings->value("animation/inout", true).toBool();
}

bool DockItemManager::isEnableDragAnimation()
{
    return m_qsettings->value("animation/drag", true).toBool();
}

bool DockItemManager::isEnableHoverHighlight()
{
    return m_qsettings->value("animation/highlight", true).toBool();
}

void DockItemManager::setHoverScaleAnimation(bool enable)
{
    if(enable != isEnableHoverScaleAnimation()) {
        m_qsettings->setValue("animation/hover", enable);
        emit hoverScaleChanged(enable);
    }
}

void DockItemManager::setInOutAnimation(bool enable)
{
    if(enable != isEnableInOutAnimation()) {
        m_qsettings->setValue("animation/inout", enable);
        emit inoutChanged(enable);
    }
}

void DockItemManager::setDragAnimation(bool enable)
{
    m_qsettings->setValue("animation/drag", enable);
}

void DockItemManager::setHoverHighlight(bool enable)
{
    if(isEnableHoverHighlight() != enable) {
        m_qsettings->setValue("animation/highlight", enable);
        emit hoverHighlighted(enable);
    }
}

DockItemManager::ActivateAnimationType DockItemManager::animationType() {
    return m_qsettings->value("animation/activate", Jump).value<ActivateAnimationType>();
}

void DockItemManager::setAnimationType(ActivateAnimationType type) {
    m_qsettings->setValue("animation/activate", type);
    m_qsettings->sync();
}

bool DockItemManager::hasWindowItem()
{
    for(auto item : m_itemList)
        if(item->itemType() == DockItem::App && qobject_cast<AppItem *>(item)->windowCount() > 0)
            return true;

    return false;
}

int DockItemManager::itemSize()
{
    return m_appInter->windowSizeFashion();
}

const QList<QPointer<DockItem>> DockItemManager::itemList()
{
    return m_itemList;
}

const QList<QPointer<DirItem>> DockItemManager::dirList()
{
    return m_dirList;
}

bool DockItemManager::appIsOnDock(const QString &appDesktop) const
{
    return m_appInter->IsOnDock(appDesktop);
}

void DockItemManager::itemMoved(DockItem *const sourceItem, DockItem *const targetItem)
{
    Q_ASSERT(sourceItem != targetItem);

    const DockItem::ItemType moveType = sourceItem->itemType();
    const DockItem::ItemType replaceType = targetItem->itemType();

    // app move
    if (moveType == DockItem::App || moveType == DockItem::Placeholder)
        if (replaceType != DockItem::App)
            return;

    const int moveIndex = m_itemList.indexOf(sourceItem);
    const int replaceIndex = m_itemList.indexOf(targetItem);

    m_itemList.removeAt(moveIndex);
    m_itemList.insert(replaceIndex, sourceItem);

    // for app move, index 0 is launcher item, need to pass it.
    if (moveType == DockItem::App && replaceType == DockItem::App)
        m_appInter->MoveEntry(moveIndex, replaceIndex);
}

void DockItemManager::itemAdded(const QString &appDesktop, int idx)
{
    m_appInter->RequestDock(appDesktop, idx);
}

void DockItemManager::appItemAdded(const QDBusObjectPath &path, const int index, const bool updateFrame)
{
    AppItem *item = new AppItem(path.path());

    connect(item, &DockItem::requestRefreshWindowVisible, this, &DockItemManager::requestRefershWindowVisible, Qt::UniqueConnection);
    connect(item, &DockItem::requestWindowAutoHide, this, &DockItemManager::requestWindowAutoHide, Qt::UniqueConnection);

    connect(item, &AppItem::requestActivateWindow, m_appInter, &DBusDock::ActivateWindow, Qt::QueuedConnection);
    connect(item, &AppItem::requestPreviewWindow, m_appInter, &DBusDock::PreviewWindow);
    connect(item, &AppItem::requestCancelPreview, m_appInter, &DBusDock::CancelPreviewWindow);

    connect(item, &AppItem::windowItemInserted, [this](WindowItem *item){
        emit itemInserted(-1, item);
        emit itemCountChanged();
    });
    connect(item, &AppItem::windowItemRemoved, [this](WindowItem *item, bool animation){
        emit itemRemoved(item, animation);
        emit itemCountChanged();
    });


    m_itemList.insert(index != -1 ? index : m_itemList.count(), item);

    for(auto dirItem : m_dirList)
    {
        if(dirItem->hasId(item->getDesktopFile()))
        {
            dirItem->addItem(item);
            item->fetchWindowInfos();

            if(index == -1 && dirItem->currentCount() == 1)
            {
                emit itemInserted(-1, dirItem);
                if(updateFrame)
                    emit itemCountChanged();
            }
            return;
        }
    }

    item->fetchWindowInfos();
    emit itemInserted(index, item);

    if(updateFrame)
        emit itemCountChanged();
}

void DockItemManager::appItemRemoved(const QString &appId)
{
    for (int i(0); i < m_itemList.size(); ++i) {
        if(AppItem *app = static_cast<AppItem *>(m_itemList[i].data()))
        {
            if (app->itemType() == DockItem::App && (!app->isValid() || app->appId() == appId)) {
                appItemRemoved(app);
                emit itemCountChanged();
                break;
            }
        }
    }
}

void DockItemManager::appItemRemoved(AppItem *appItem, bool animation)
{
    m_itemList.removeOne(appItem);

    if(appItem->getPlace() == DockItem::DirPlace)
        appItem->getDirItem()->removeItem(appItem, false);
    else
        emit itemRemoved(appItem, animation);

    appItem->removeWindowItem(animation);
    QTimer::singleShot(animation ? 500 : 10, [ appItem ] { appItem->deleteLater(); });
}

void DockItemManager::reloadAppItems()
{
    static bool first = true;
    if(first)
    {
        emit itemInserted(0, new LauncherItem);
        emit itemInserted(0, new TrashItem);
        first = false;
    }
    else
    {
        while (!m_itemList.isEmpty())
            appItemRemoved(qobject_cast<AppItem *>(m_itemList.first().data()), false);
        m_itemList.clear();

        for(auto item : m_dirList)
        {
            emit itemRemoved(item, false);
            QTimer::singleShot(10, [ item ] { item->deleteLater(); });
        }
        m_dirList.clear();
    }

    loadDirAppData();
    for (auto &path : m_appInter->entries())
        appItemAdded(path, -1, false);

    for(auto dirItem : m_dirList)
        if(!dirItem->parentWidget() && !dirItem->isEmpty())
            emit itemInserted(dirItem->getIndex(), dirItem);

    emit itemCountChanged();
}

void DockItemManager::addDirApp(DirItem *dirItem)
{
    m_dirList.append(dirItem);
    connect(dirItem, &DockItem::requestWindowAutoHide, this, &DockItemManager::requestWindowAutoHide, Qt::UniqueConnection);
    emit itemCountChanged();
}

void DockItemManager::loadDirAppData()
{
    int count = m_qsettings->value("count", 0).toInt();
    while (count >=1)
    {
        DirItem *item = new DirItem(m_qsettings->value(QString("dir_%1/title").arg(count), "").toString());
        item->setIndex(m_qsettings->value(QString("dir_%1/index").arg(count), -1).toInt());
        QStringList desktopFiles = m_qsettings->value(QString("dir_%1/ids").arg(count), QStringList()).value<QStringList>();
        item->setIds(QSet<QString>(desktopFiles.begin(), desktopFiles.end()));

        m_dirList.append(item);

        connect(item, &DirItem::updateContent, this, &DockItemManager::updateDirApp);
        connect(item, &DockItem::requestWindowAutoHide, this, &DockItemManager::requestWindowAutoHide, Qt::UniqueConnection);

        count--;
    }
}

void DockItemManager::updateDirApp()
{
    m_qsettings->setFallbacksEnabled(true);
    int index = 0, originCount = m_qsettings->value("count", 0).toInt();
    for(auto itemDir : m_dirList)
    {
        QSet<QString> ids;
        for(auto item : itemDir->getAppList())
            ids.insert(item->getDesktopFile());

        if(ids.isEmpty())
            continue;

        index++;
        m_qsettings->setValue(QString("dir_%1/title").arg(index), itemDir->getTitle());
        m_qsettings->setValue(QString("dir_%1/index").arg(index), itemDir->getIndex());
        m_qsettings->setValue(QString("dir_%1/ids").arg(index), QStringList(ids.values()));
    }

    while(index < originCount)
    {
        m_qsettings->remove(QString("dir_%1").arg(originCount--));
    }

    m_qsettings->setValue("count", index);
    m_qsettings->sync();
}