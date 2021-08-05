/*
 * Copyright (C) 2011 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *
 * Maintainer: sbw <sbw@sbw.so>
 *             zhaolong <zhaolong@uniontech.com>
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

#include "docksettings.h"
#include "item/appitem.h"
#include "window/dockitemmanager.h"
#include "imageutil.h"

#include <QDebug>
#include <QX11Info>
#include <DApplication>
#include <QScreen>

#define DEFAULT_HEIGHT 72
#define WINDOW_MAX_SIZE          100
#define WINDOW_MIN_SIZE          40

DWIDGET_USE_NAMESPACE

extern const QPoint rawXPosition(const QPoint &scaledPos);

DockSettings::DockSettings(QWidget *parent)
    : QObject(parent)
    , m_dockInter(new DBusDock("com.deepin.dde.daemon.Dock", "/com/deepin/dde/daemon/Dock", QDBusConnection::sessionBus(), this))
    , m_autoHide(true)
    , m_opacity(0.4)
    , m_bottomPosAct("下", this)
    , m_leftPosAct("左", this)
    , m_rightPosAct("右", this)
    , m_keepShownAct("一直显示", this)
    , m_keepHiddenAct("一直隐藏", this)
    , m_smartHideAct("智能隐藏", this)
    , m_displayInter(new DisplayInter("com.deepin.daemon.Display", "/com/deepin/daemon/Display", QDBusConnection::sessionBus(), this))
    , m_itemManager(DockItemManager::instance(this))
    , m_isMouseMoveCause(false)
    , m_mouseCauseDockScreen(nullptr)
{
    checkService();

    onMonitorListChanged(m_displayInter->monitors());

    m_primaryRawRect = m_displayInter->primaryRect();
    m_currentRawRect = m_primaryRawRect;
    m_screenRawHeight = m_displayInter->screenHeight();
    m_screenRawWidth = m_displayInter->screenWidth();
    m_position = Dock::Position(m_dockInter->position());
    m_hideMode = Dock::HideMode(m_dockInter->hideMode());
    m_hideState = Dock::HideState(m_dockInter->hideState());
    DockItem::setDockPosition(m_position);
    qApp->setProperty(PROP_POSITION, QVariant::fromValue(m_position));

    m_bottomPosAct.setCheckable(true);
    m_leftPosAct.setCheckable(true);
    m_rightPosAct.setCheckable(true);
    m_keepShownAct.setCheckable(true);
    m_keepHiddenAct.setCheckable(true);
    m_smartHideAct.setCheckable(true);

    QMenu *locationSubMenu = new QMenu(&m_settingsMenu);
    locationSubMenu->addAction(&m_bottomPosAct);
    locationSubMenu->addAction(&m_leftPosAct);
    locationSubMenu->addAction(&m_rightPosAct);
    QAction *locationSubMenuAct = new QAction("位置", this);
    locationSubMenuAct->setMenu(locationSubMenu);

    QMenu *statusSubMenu = new QMenu(&m_settingsMenu);
    statusSubMenu->addAction(&m_keepShownAct);
    statusSubMenu->addAction(&m_keepHiddenAct);
    statusSubMenu->addAction(&m_smartHideAct);
    QAction *statusSubMenuAct = new QAction("状态", this);
    statusSubMenuAct->setMenu(statusSubMenu);

    m_settingsMenu.addAction(locationSubMenuAct);
    m_settingsMenu.addAction(statusSubMenuAct);
    m_settingsMenu.setTitle("设置");

    connect(&m_settingsMenu, &QMenu::triggered, this, &DockSettings::menuActionClicked);
    connect(m_dockInter, &DBusDock::PositionChanged, this, &DockSettings::onPositionChanged);
    connect(m_dockInter, &DBusDock::HideModeChanged, this, &DockSettings::hideModeChanged, Qt::QueuedConnection);
    connect(m_dockInter, &DBusDock::HideStateChanged, this, &DockSettings::hideStateChanged);
    connect(m_dockInter, &DBusDock::ServiceRestarted, this, &DockSettings::resetFrontendGeometry);
    connect(m_dockInter, &DBusDock::OpacityChanged, this, &DockSettings::onOpacityChanged);
    connect(m_dockInter, &DBusDock::WindowSizeEfficientChanged, this, &DockSettings::onWindowSizeChanged);
    connect(m_dockInter, &DBusDock::WindowSizeFashionChanged, this, &DockSettings::onWindowSizeChanged);

    connect(m_itemManager, &DockItemManager::itemInserted, this, &DockSettings::onWindowSizeChanged, Qt::QueuedConnection);
    connect(m_itemManager, &DockItemManager::itemRemoved, this, &DockSettings::onWindowSizeChanged, Qt::QueuedConnection);

    connect(m_displayInter, &DisplayInter::PrimaryRectChanged, this, &DockSettings::primaryScreenChanged, Qt::QueuedConnection);
    connect(m_displayInter, &DisplayInter::ScreenHeightChanged, this, &DockSettings::primaryScreenChanged, Qt::QueuedConnection);
    connect(m_displayInter, &DisplayInter::ScreenWidthChanged, this, &DockSettings::primaryScreenChanged, Qt::QueuedConnection);
    connect(m_displayInter, &DisplayInter::PrimaryChanged, this, &DockSettings::primaryScreenChanged, Qt::QueuedConnection);
    connect(m_displayInter, &DisplayInter::MonitorsChanged, this, &DockSettings::onMonitorListChanged);

    DApplication *app = qobject_cast<DApplication *>(qApp);
    if (app) {
        connect(app, &DApplication::iconThemeChanged, this, &DockSettings::gtkIconThemeChanged);
    }

    calculateMultiScreensPos();
    calculateWindowConfig();

    QTimer::singleShot(0, this, [ = ] {onOpacityChanged(m_dockInter->opacity());});
}

const QList<QRect> DockSettings::monitorsRect() const
{
    QList<QRect> monsRect;
    QMapIterator<Monitor *, MonitorInter *> iterator(m_monitors);
    while (iterator.hasNext()) {
        iterator.next();
        Monitor *monitor = iterator.key();
        if (monitor) {
            monsRect << monitor->rect();
        }
    }
    return  monsRect;
}

DockSettings &DockSettings::Instance()
{
    static DockSettings settings;
    return settings;
}

const QRect DockSettings::primaryRect() const
{
    QRect rect = m_primaryRawRect;
    qreal scale = qApp->primaryScreen()->devicePixelRatio();

    rect.setWidth(std::round(qreal(rect.width()) / scale));
    rect.setHeight(std::round(qreal(rect.height()) / scale));

    return rect;
}

const QRect DockSettings::currentRect() const
{
    QRect rect;
    bool positionAllowed = false;
    QList<Monitor*> monitors = m_monitors.keys();
    for (Monitor *monitor : monitors) {
        switch (m_position) {
        case Top:       positionAllowed = monitor->dockPosition().topDock;      break;
        case Right:     positionAllowed = monitor->dockPosition().rightDock;    break;
        case Bottom:    positionAllowed = monitor->dockPosition().bottomDock;   break;
        case Left:      positionAllowed = monitor->dockPosition().leftDock;     break;
        }
        if (positionAllowed) {
            rect = monitor->rect();
            if (monitor->isPrimary())
                break;
        }
    }
    m_currentRawRect = rect;
    qreal scale = qApp->primaryScreen()->devicePixelRatio();
    rect.setWidth(std::round(qreal(rect.width()) / scale));
    rect.setHeight(std::round(qreal(rect.height()) / scale));
    return rect;
}

const QSize DockSettings::panelSize() const
{
    return m_mainWindowSize;
}

const QRect DockSettings::windowRect(const Position position, const bool hide)
{
    QSize size = m_mainWindowSize;
    if (hide) {
        switch (position) {
            case Top:
            case Bottom:    size.setHeight(2);      break;
            case Left:
            case Right:     size.setWidth(2);       break;
        }
    }

    const QRect primaryRect = this->currentRect();
    const int offsetX = (primaryRect.width() - size.width()) / 2;
    const int offsetY = (primaryRect.height() - size.height()) / 2;
    QPoint p(0, 0);
    switch (position) {
        case Top:
        case Left:
            p = QPoint(0, offsetY);
            break;
        case Right:
            p = QPoint(primaryRect.width() - size.width(), offsetY);
            break;
        case Bottom:
            p = QPoint(offsetX, primaryRect.height() - size.height());
            break;
    }

    return QRect(primaryRect.topLeft() + p, size);
}

void DockSettings::showDockSettingsMenu()
{
    m_autoHide = false;
    m_bottomPosAct.setChecked(m_position == Bottom);
    m_leftPosAct.setChecked(m_position == Left);
    m_rightPosAct.setChecked(m_position == Right);
    m_keepShownAct.setChecked(m_hideMode == KeepShowing);
    m_keepHiddenAct.setChecked(m_hideMode == KeepHidden);
    m_smartHideAct.setChecked(m_hideMode == SmartHide);

    m_settingsMenu.exec(QCursor::pos());

    setAutoHide(true);
}

void DockSettings::setAutoHide(const bool autoHide)
{
    if (m_autoHide == autoHide)
        return;

    m_autoHide = autoHide;
    emit autoHideChanged(m_autoHide);
}

void DockSettings::menuActionClicked(QAction *action)
{
    Q_ASSERT(action);

    calculateMultiScreensPos();
    if (action == &m_bottomPosAct)
        return m_dockInter->setPosition(Bottom);
    if (action == &m_leftPosAct)
        return m_dockInter->setPosition(Left);
    if (action == &m_rightPosAct)
        return m_dockInter->setPosition(Right);
    if (action == &m_keepShownAct)
        return m_dockInter->setHideMode(KeepShowing);
    if (action == &m_keepHiddenAct)
        return m_dockInter->setHideMode(KeepHidden);
    if (action == &m_smartHideAct)
        return m_dockInter->setHideMode(SmartHide);
}

void DockSettings::onPositionChanged()
{
    const Position prevPos = m_position;
    const Position nextPos = Dock::Position(m_dockInter->position());

    if (prevPos == nextPos)
        return;

    emit positionChanged(prevPos, nextPos);

    QTimer::singleShot(200, this, [this, nextPos] {
        m_position = nextPos;
        DockItem::setDockPosition(nextPos);
        qApp->setProperty(PROP_POSITION, QVariant::fromValue(nextPos));

        calculateWindowConfig();

        m_itemManager->refershItemsIcon();
    });
}

void DockSettings::hideModeChanged()
{
//    qDebug() << Q_FUNC_INFO;
    m_hideMode = Dock::HideMode(m_dockInter->hideMode());

    emit windowHideModeChanged();
}

void DockSettings::hideStateChanged()
{
//    qDebug() << Q_FUNC_INFO;
    const Dock::HideState state = Dock::HideState(m_dockInter->hideState());

    if (state == Dock::Unknown)
        return;

    m_hideState = state;

    emit windowVisibleChanged();
}

void DockSettings::primaryScreenChanged()
{
//    qDebug() << Q_FUNC_INFO;
    m_primaryRawRect = m_displayInter->primaryRect();
    m_screenRawHeight = m_displayInter->screenHeight();
    m_screenRawWidth = m_displayInter->screenWidth();

    //为了防止当后端发送错误值，然后发送正确值时，任务栏没有移动在相应的位置
    //当ｑｔ没有获取到屏幕资源时候，move函数会失效。可以直接return
    if (m_screenRawHeight == 0 || m_screenRawWidth == 0){
        return;
    }
    calculateMultiScreensPos();
    emit dataChanged();
    calculateWindowConfig();

    // 主屏切换时，如果缩放比例不一样，需要刷新item的图标(bug:3176)
    m_itemManager->refershItemsIcon();
}

void DockSettings::resetFrontendGeometry()
{
    const QRect r = windowRect(m_position);
    const qreal ratio = dockRatio();
    const QPoint p = rawXPosition(r.topLeft());
    const uint w = r.width() * ratio;
    const uint h = r.height() * ratio;

    m_frontendRect = QRect(p.x(), p.y(), w, h);
    m_dockInter->SetFrontendWindowRect(p.x(), p.y(), w, h);
}

void DockSettings::onOpacityChanged(const double value)
{
    if (m_opacity == value) return;

    m_opacity = value;

    emit opacityChanged(value * 255);
}

int DockSettings::itemCount()
{
    int count = m_itemManager->itemList().count() + 1;
    for(auto item : m_itemManager->dirList())
    {
        count = count - item->currentCount() + 1;
    }

    return count;
}

int DockSettings::dockWindowSize()
{
    return m_dockWindowSize;
}

int DockSettings::itemSize()
{
    return m_itemSize;
}

void DockSettings::calculateWindowConfig()
{
    setDockWindowSize(int(m_dockInter->windowSizeFashion()));
}

void DockSettings::setDockWindowSize(int size)
{
    m_dockWindowSize = size;
    if (m_dockWindowSize > WINDOW_MAX_SIZE || m_dockWindowSize < WINDOW_MIN_SIZE) {
        m_dockWindowSize = DEFAULT_HEIGHT;
        m_dockInter->setWindowSize(m_dockWindowSize);
    }

    int count = itemCount(), length;
    m_itemSize = m_dockWindowSize - 20;
    switch (m_position) {
        case Top:
        case Bottom: {
            m_mainWindowSize.setHeight(m_dockWindowSize);
            length = primaryRect().width();
            if (m_itemSize * count + MODE_PADDING * count > length * 0.9)
            {
                m_mainWindowSize.setWidth(length * 0.9);
                m_itemSize = round(length * 0.9 / count - MODE_PADDING);
            }
            else
            {
                m_mainWindowSize.setWidth(m_itemSize * count + MODE_PADDING * (count + 4));
            }
            break;
        }
        case Left:
        case Right: {
            m_mainWindowSize.setWidth(m_dockWindowSize);
            length = primaryRect().height();
            if (m_itemSize * count + MODE_PADDING * count > length * 0.9)
            {
                m_mainWindowSize.setHeight(length * 0.9);
                m_itemSize = round(length * 0.9 / count - MODE_PADDING);
            }
            else
            {
                m_mainWindowSize.setHeight(m_itemSize * count + MODE_PADDING * (count + 4));
            }

            break;
        }
    }
    resetFrontendGeometry();
}

void DockSettings::gtkIconThemeChanged()
{
    qDebug() << Q_FUNC_INFO;
    m_itemManager->refershItemsIcon();
}

qreal DockSettings::dockRatio() const
{
    QScreen const *screen = ImageUtil::screenAtByScaled(m_frontendRect.center());

    return screen ? screen->devicePixelRatio() : qApp->devicePixelRatio();
}

void DockSettings::onWindowSizeChanged()
{
    calculateWindowConfig();
    emit windowGeometryChanged();
}

void DockSettings::checkService()
{
    QString serverName = "com.deepin.dde.daemon.Dock";
    QDBusConnectionInterface *ifc = QDBusConnection::sessionBus().interface();

    if (!ifc->isServiceRegistered(serverName)) {
        connect(ifc, &QDBusConnectionInterface::serviceOwnerChanged, this, [ = ](const QString & name, const QString & oldOwner, const QString & newOwner) {
            Q_UNUSED(oldOwner)
            if (name == serverName && !newOwner.isEmpty()) {

                m_dockInter = new DBusDock(serverName, "/com/deepin/dde/daemon/Dock", QDBusConnection::sessionBus(), this);
                onPositionChanged();
                hideModeChanged();
                hideStateChanged();
                onOpacityChanged(m_dockInter->opacity());
                onWindowSizeChanged();

                disconnect(ifc);
            }
        });
    }
}

void DockSettings::calculateMultiScreensPos()
{
    QList<Monitor *> monitors = m_monitors.keys();
    for (Monitor *monitor : monitors) {
        monitor->setDockPosition(Monitor::DockPosition(true, true, true, true));
    }

    switch (m_monitors.size()) {
    case 0:
        break;
    case 1: {
        QList<Monitor*>screens = m_monitors.keys();
        Monitor* s1 = screens.at(0);
        s1->setDockPosition(Monitor::DockPosition(true, true, true, true));
    }
        break;
    case 2:
        twoScreensCalPos();
        break;
    case 3:
        treeScreensCalPos();
        break;
    }
}

void DockSettings::monitorAdded(const QString &path)
{
    MonitorInter *inter = new MonitorInter("com.deepin.daemon.Display", path, QDBusConnection::sessionBus(), this);
    Monitor *mon = new Monitor(this);

    connect(inter, &MonitorInter::XChanged, mon, &Monitor::setX);
    connect(inter, &MonitorInter::YChanged, mon, &Monitor::setY);
    connect(inter, &MonitorInter::WidthChanged, mon, &Monitor::setW);
    connect(inter, &MonitorInter::HeightChanged, mon, &Monitor::setH);
    connect(inter, &MonitorInter::MmWidthChanged, mon, &Monitor::setMmWidth);
    connect(inter, &MonitorInter::MmHeightChanged, mon, &Monitor::setMmHeight);
    connect(inter, &MonitorInter::RotationChanged, mon, &Monitor::setRotate);
    connect(inter, &MonitorInter::NameChanged, mon, &Monitor::setName);
    connect(inter, &MonitorInter::CurrentModeChanged, mon, &Monitor::setCurrentMode);
    connect(inter, &MonitorInter::ModesChanged, mon, &Monitor::setModeList);
    connect(inter, &MonitorInter::RotationsChanged, mon, &Monitor::setRotateList);
    connect(inter, &MonitorInter::EnabledChanged, mon, &Monitor::setMonitorEnable);
    connect(m_displayInter, static_cast<void (DisplayInter::*)(const QString &) const>(&DisplayInter::PrimaryChanged), mon, &Monitor::setPrimary);

    // NOTE: DO NOT using async dbus call. because we need to have a unique name to distinguish each monitor
    Q_ASSERT(inter->isValid());
    mon->setName(inter->name());

    mon->setMonitorEnable(inter->enabled());
    mon->setPath(path);
    mon->setX(inter->x());
    mon->setY(inter->y());
    mon->setW(inter->width());
    mon->setH(inter->height());
    mon->setRotate(inter->rotation());
    mon->setCurrentMode(inter->currentMode());
    mon->setModeList(inter->modes());

    mon->setRotateList(inter->rotations());
    mon->setPrimary(m_displayInter->primary());
    mon->setMmWidth(inter->mmWidth());
    mon->setMmHeight(inter->mmHeight());

    m_monitors.insert(mon, inter);
    inter->setSync(false);
}

void DockSettings::monitorRemoved(const QString &path)
{
    Monitor *monitor = nullptr;
    for (auto it(m_monitors.cbegin()); it != m_monitors.cend(); ++it) {
        if (it.key()->path() == path) {
            monitor = it.key();
            break;
        }
    }
    if (!monitor)
        return;

    m_monitors.value(monitor)->deleteLater();
    m_monitors.remove(monitor);

    monitor->deleteLater();

    calculateMultiScreensPos();
}

void DockSettings::twoScreensCalPos()
{
    QList<Monitor*>screens = m_monitors.keys();
    Monitor* s1 = screens.at(0);
    Monitor* s2 = screens.at(1);
    if (!s1 && !s2)
        return;

    // 只在某屏显示时,其它屏禁用
    bool s1Enabled = s1->enable();
    bool s2Enabled = s2->enable();
    if (!s1Enabled || !s2Enabled) {
        if (!s1Enabled && s2Enabled) {
            s1->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s2->setDockPosition(Monitor::DockPosition(true, true, true, true));
        } else if (!s2Enabled && s1Enabled) {
            s1->setDockPosition(Monitor::DockPosition(true, true, true, true));
            s2->setDockPosition(Monitor::DockPosition(false, false, false, false));
        } else if (!s1Enabled && !s2Enabled) {
            s1->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s2->setDockPosition(Monitor::DockPosition(false, false, false, false));
        }
        return;
    }

    combination(screens);
}

void DockSettings::treeScreensCalPos()
{
    QList<Monitor*>screens = m_monitors.keys();
    Monitor* s1 = screens.at(0);
    Monitor* s2 = screens.at(1);
    Monitor* s3 = screens.at(2);
    if (!s1 && !s2 && !s3)
        return;

    // 只在某屏显示时,其它屏禁用
    bool s1Enabled = s1->enable();
    bool s2Enabled = s2->enable();
    bool s3Enabled = s3->enable();
    if (!s1Enabled || !s2Enabled || !s3Enabled) {
        if (!s1Enabled && !s2Enabled && s3Enabled) {
            s1->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s2->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s3->setDockPosition(Monitor::DockPosition(true, true, true, true));
        } else if (!s1Enabled && s2Enabled && !s3Enabled) {
            s1->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s2->setDockPosition(Monitor::DockPosition(true, true, true, true));
            s3->setDockPosition(Monitor::DockPosition(false, false, false, false));
        } else if (s1Enabled && !s2Enabled && !s3Enabled) {
            s1->setDockPosition(Monitor::DockPosition(true, true, true, true));
            s2->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s3->setDockPosition(Monitor::DockPosition(false, false, false, false));
        } else if (!s1Enabled && !s2Enabled && !s3Enabled) {
            s1->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s2->setDockPosition(Monitor::DockPosition(false, false, false, false));
            s3->setDockPosition(Monitor::DockPosition(false, false, false, false));
        }
        return;
    }

    combination(screens);
}

void DockSettings::combination(QList<Monitor *> &screens)
{
    if (screens.size() < 2)
        return;

    Monitor *last = screens.takeLast();

    for (Monitor *screen : screens) {
        calculateRelativePos(last, screen);
    }

    combination(screens);
}

void DockSettings::calculateRelativePos(Monitor *s1, Monitor *s2)
{
    /* 鼠标可移动区另算 */
    if (!s1 && !s2)
        return;

    // 对齐
    bool isAligment = false;
    // 左右拼
    // s1左 s2右
    if (s1->right() == s2->left() ) {
        isAligment = (s1->topRight() == s2->topLeft())
                && (s1->bottomRight() == s2->bottomLeft());
        if (isAligment) {
            s1->dockPosition().rightDock = false;
            s2->dockPosition().leftDock = false;
        } else {
            if (!s1->isPrimary())
                s1->dockPosition().rightDock = false;
            if (!s2->isPrimary())
                s2->dockPosition().leftDock = false;
        }
    }
    // s1右 s2左
    if (s1->left() == s2->right()) {
        isAligment = (s1->topLeft() == s2->topRight())
                && (s1->bottomLeft() == s2->bottomRight());
        if (isAligment) {
            s1->dockPosition().leftDock = false;
            s2->dockPosition().rightDock = false;
        } else {
            if (!s1->isPrimary())
                s1->dockPosition().leftDock = false;
            if (!s2->isPrimary())
                s2->dockPosition().rightDock = false;
        }
    }
    // 上下拼
    // s1上 s2下
    if (s1->top() == s2->bottom()) {
        isAligment = (s1->bottomLeft() == s2->topLeft())
                && (s1->bottomRight() == s2->topRight());
        if (isAligment) {
            s1->dockPosition().bottomDock = false;
            s2->dockPosition().topDock = false;
        } else {
            if (!s1->isPrimary())
                s1->dockPosition().bottomDock = false;
            if (!s2->isPrimary())
                s2->dockPosition().topDock = false;
        }
    }
    // s1下 s2上
    if (s1->bottom() == s2->top()) {
        isAligment = (s1->topLeft() == s2->bottomLeft())
                && (s1->topRight() == s2->bottomRight());
        if (isAligment) {
            s1->dockPosition().topDock = false;
            s2->dockPosition().bottomDock = false;
        } else {
            if (!s1->isPrimary())
                s1->dockPosition().topDock = false;
            if (!s2->isPrimary())
                s2->dockPosition().bottomDock = false;
        }
    }

    // 对角拼
    bool isDiagonal = (s1->topLeft() == s2->bottomRight())
            || (s1->topRight() == s2->bottomLeft())
            ||  (s1->bottomLeft() == s2->topRight())
            || (s1->bottomRight() == s2->topLeft());
    if (isDiagonal) {
        auto position = Monitor::DockPosition(false, false, false, false);
        if (!s1->isPrimary())
            s2->setDockPosition(position);
        if (!s2->isPrimary())
            s1->setDockPosition(position);

        switch (m_position) {
        case Top:
            s1->dockPosition().topDock = true;
            s2->dockPosition().topDock = true;
            break;
        case Bottom:
            s1->dockPosition().bottomDock = true;
            s2->dockPosition().bottomDock = true;
            break;
        case Left:
            s1->dockPosition().leftDock = true;
            s2->dockPosition().leftDock = true;
            break;
        case Right:
            s1->dockPosition().rightDock = true;
            s2->dockPosition().rightDock = true;
            break;
        }
    }
}

void DockSettings::onMonitorListChanged(const QList<QDBusObjectPath> &mons)
{
    if (mons.isEmpty())
        return;

    QList<QString> ops;
    for (const auto *mon : m_monitors.keys())
        ops << mon->path();

    QList<QString> pathList;
    for (const auto op : mons) {
        const QString path = op.path();
        pathList << path;
        if (!ops.contains(path))
            monitorAdded(path);
    }

    for (const auto op : ops)
        if (!pathList.contains(op))
            monitorRemoved(op);
}
