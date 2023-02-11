﻿/*
 * Copyright (C) 2018 ~ 2020 Deepin Technology Co., Ltd.
 *
 * Author:     fanpengcheng <fanpengcheng_cm@deepin.com>
 *
 * Maintainer: fanpengcheng <fanpengcheng_cm@deepin.com>
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

#include "multiscreenworker.h"
#include "window/mainwindow.h"
#include "window/mainpanelcontrol.h"
#include "utils.h"
#include "xcb/xcb_misc.h"
#include "displaymanager.h"
#include "window/dockitemmanager.h"

#include <QWidget>
#include <QScreen>
#include <QEvent>
#include <QRegion>
#include <QVariantAnimation>
#include <QX11Info>
#include <QDBusConnection>
#include <qpa/qplatformscreen.h>
#include <QApplication>

static const QString MonitorsSwitchTime = "monitorsSwitchTime";

#define DIS_INS DisplayManager::instance()
#define SPLITER_SIZE 3
#define MODE_PADDING 5

class SharedData {
    public:
        static SharedData *instance()
        {
            static SharedData *INSTANCE = new SharedData;
            return INSTANCE;
        }

        QRect getRect(const QString name, Position pos, qreal ratio, int itemCount)
        {
            return data.value(generalKey(name, pos, ratio, itemCount), {});
        }

        void setRect(const QString name, Position pos, qreal ratio, int itemCount, QRect rect)
        {
            if(rect.isValid())
                data.insert(generalKey(name, pos, ratio, itemCount), rect);
        }

        QString generalKey(const QString name, Position pos, qreal ratio, int itemCount)
        {
            return QString("%1_%2_%3_%4").arg(name).arg(pos).arg(ratio).arg(itemCount);
        }

        void clear() { data.clear(); }

    private:
        SharedData(){}

    private:
        QMap<QString, QRect> data;
};

MultiScreenWorker::MultiScreenWorker(MainWindow *parent)
    : QObject(parent)
    , m_parent(parent)
    , m_eventInter(nullptr)
    , m_extralEventInter(nullptr)
    , m_dockInter(nullptr)
    , m_launcherInter(nullptr)
    , m_delayWakeTimer(new QTimer(this))
    , m_ds(DIS_INS->primary())
    , m_state(AutoHide)
{
    m_delayWakeTimer->setSingleShot(true);
    m_delayWakeTimer->setInterval(Utils::SettingValue("com.deepin.dde.dock.mainwindow", "/com/deepin/dde/dock/mainwindow/", MonitorsSwitchTime, 2000).toInt());

    ani = new QPropertyAnimation(m_parent, "pos", this);
    ani->setEasingCurve(QEasingCurve::InOutCubic);
    connect(ani, &QPropertyAnimation::finished, this, [this]{
        if(ani->direction() == QPropertyAnimation::Backward) m_parent->hide();
    });

    QTimer::singleShot(100, this, &MultiScreenWorker::checkDaemonDockService);
}

void MultiScreenWorker::initShow()
{
    // 仅在初始化时调用一次
    static bool first = true;
    if (first)
    {
        if(!m_dockInter)
            return QTimer::singleShot(100, this, &MultiScreenWorker::initShow);

        first = false;

        const QRect rect = getDockShowGeometry(m_ds.current(), m_position);
        m_parent->resize(rect.size());
        m_parent->panel()->setPositonValue(m_position);

        if (m_hideMode == HideMode::KeepShowing || m_hideState == HideState::Show)
            displayAnimation(m_ds.current(), AniAction::Show);
        else if (m_hideMode == HideMode::KeepHidden || m_hideState == HideState::Hide)
            displayAnimation(m_ds.current(), AniAction::Hide);
    }
}

/**
 * @brief MultiScreenWorker::setStates 用于存储一些状态
 * @param state 标识是哪一种状态，后面有需求可以扩充
 * @param on 设置状态为true或false
 */
void MultiScreenWorker::setStates(RunStates state, bool on)
{
    RunState type = static_cast<RunState>(int(state & RunState::RunState_Mask));

    if (on)
        m_state |= type;
    else
        m_state &= ~(type);
}

void MultiScreenWorker::setWindowSize(int size)
{
    if(m_windowSize != size) {
        SharedData::instance()->clear();
        m_windowSize = size;
    }
}

void MultiScreenWorker::setComposite(bool composite)
{
    ani->setDuration(composite ? 300 : 0);
}

void MultiScreenWorker::onAutoHideChanged(bool autoHide)
{
    setStates(AutoHide, autoHide);
}

/**
 * @brief updateDaemonDockSize
 * @param dockSize              这里的高度是通过qt获取的，不能使用后端的接口数据
 */
void MultiScreenWorker::updateDaemonDockSize(int dockSize)
{
    m_dockInter->setWindowSize(uint(dockSize));
    m_dockInter->setWindowSizeFashion(uint(dockSize));

    setWindowSize(dockSize);

    requestNotifyWindowManager();
    requestUpdateFrontendGeometry();
    onRequestUpdateRegionMonitor();
}

void MultiScreenWorker::handleDbusSignal(QDBusMessage msg)
{
    QList<QVariant> arguments = msg.arguments();
    // 参数固定长度
    if (3 != arguments.count())
        return;
    // 返回的数据中,这一部分对应的是数据发送方的interfacename,可判断是否是自己需要的服务
    QString interfaceName = msg.arguments().at(0).toString();
    if (interfaceName == "com.deepin.dde.daemon.Dock") {
        QVariantMap changedProps = qdbus_cast<QVariantMap>(arguments.at(1).value<QDBusArgument>());
        QStringList keys = changedProps.keys();
        for (const QString &prop : keys) {
            if (prop == "Position") {
                Position position = static_cast<Position>(changedProps.value(prop).toInt());
                if(position == Top) position = Bottom;
                if (m_position != position) {
                    Position lastPos = m_position;
                    m_position = position;

                    m_ds.updateDockedScreen(getValidScreen(m_position));
                    changeDockPosition(m_ds.last(), m_ds.current(), lastPos, position);
                }
            } else if (prop == "HideMode") {
                HideMode hideMode = static_cast<HideMode>(changedProps.value(prop).toInt());
                if (m_hideMode != hideMode) {
                    m_hideMode = hideMode;

                    if (m_hideMode == HideMode::KeepShowing || m_hideState == HideState::Show)
                        displayAnimation(m_ds.current(), AniAction::Show);
                    else if (m_hideMode != HideMode::KeepShowing && m_hideState == HideState::Hide)
                        displayAnimation(m_ds.current(), AniAction::Hide);
                    requestNotifyWindowManager();
                    onRequestUpdateRegionMonitor();
                    emit requestUpdateFrontendGeometry();
                }
            } else if (prop == "HideState") {
                HideState state = static_cast<HideState>(changedProps.value(prop).toInt());
                if (state != Dock::Unknown && state != m_hideState) {
                    m_hideState = state;

                    // 检查当前屏幕的当前位置是否允许显示,不允许需要更新显示信息(这里应该在函数外部就处理好,不应该走到这里)

                    //TODO 这里是否存在屏幕找不到的问题，m_ds的当前屏幕是否可以做成实时同步的，公用一个指针？
                    //TODO 这里真的有必要加以下代码吗，只是隐藏模式的切换，理论上不需要检查屏幕是否允许任务栏停靠
                    QScreen *curScreen = DIS_INS->screen(m_ds.current());
                    if (!DIS_INS->canDock(curScreen, m_position))
                        m_ds.updateDockedScreen(getValidScreen(m_position));

                    if(testState(ChangePositionAnimationStart)) continue;

                    if (m_hideMode == HideMode::KeepShowing || m_hideState == HideState::Show)
                        displayAnimation(m_ds.current(), AniAction::Show);
                    else if ((m_hideMode == HideMode::KeepHidden || m_hideMode == HideMode::SmartHide) && m_hideState == HideState::Hide)
                        // 如果鼠标正在任务栏要显示的区域,就可以不用隐藏(相当于智能隐藏被唤醒一样)
                        if (!getDockShowGeometry(m_ds.current(), m_position).contains(QCursor::pos()))
                            displayAnimation(m_ds.current(), AniAction::Hide);
                }
            } else if(prop == "WindowSizeFashion") {
                setWindowSize(changedProps.value(prop).toInt());
            }
        }
    }
}

void MultiScreenWorker::updateDisplay()
{
    if (ani->state() == QPropertyAnimation::Stopped) {
        const QRect rect = getDockShowGeometry(m_ds.current(), m_position);
        if(!m_parent->isHidden())
            m_parent->setGeometry(rect);
        else
            m_parent->resize(rect.size());

        emit requestUpdateFrontendGeometry();
    }
}

/**
 * @brief onRequestUpdateRegionMonitor  更新监听区域信息
 * 触发时机:屏幕大小,屏幕坐标,屏幕数量,发生变化
 *          任务栏位置发生变化
 *          任务栏'模式'发生变化
 */
void MultiScreenWorker::onRequestUpdateRegionMonitor()
{
    if(!m_eventInter || !m_extralEventInter) return;

    if (!m_registerKey.isEmpty()) {
        m_eventInter->UnregisterArea(m_registerKey);
        m_registerKey.clear();
    }

    if (!m_extralRegisterKey.isEmpty()) {
        m_extralEventInter->UnregisterArea(m_extralRegisterKey);
        m_extralRegisterKey.clear();
    }

    if(DIS_INS->screens().size() < 2 && (m_hideMode == KeepShowing || !testState(AutoHide))) return;

    const static int flags = Motion | Button | Key;
    const static int monitorHeight = static_cast<int>(10 * qApp->devicePixelRatio());
    // 后端认为的任务栏大小(无缩放因素影响)
    const int realDockSize = int( m_dockInter->windowSizeFashion() * qApp->devicePixelRatio());

    // 任务栏唤起区域
    m_monitorRectList.clear();
    for (auto s : DIS_INS->screens()) {
        // 屏幕此位置不可停靠时,不用监听这块区域
        if (!DIS_INS->canDock(s, m_position)) continue;

        MonitRect monitorRect;
        QRect screenRect = s->geometry();
        screenRect.setSize(screenRect.size() * s->devicePixelRatio());

        switch (m_position) {
        case Top:
        case Bottom: {
            monitorRect.x1 = screenRect.x();
            monitorRect.y1 = screenRect.y() + screenRect.height() - monitorHeight;
            monitorRect.x2 = screenRect.x() + screenRect.width();
            monitorRect.y2 = screenRect.y() + screenRect.height();
        }
        break;
        case Left: {
            monitorRect.x1 = screenRect.x();
            monitorRect.y1 = screenRect.y();
            monitorRect.x2 = screenRect.x() + monitorHeight;
            monitorRect.y2 = screenRect.y() + screenRect.height();
        }
        break;
        case Right: {
            monitorRect.x1 = screenRect.x() + screenRect.width() - monitorHeight;
            monitorRect.y1 = screenRect.y();
            monitorRect.x2 = screenRect.x() + screenRect.width();
            monitorRect.y2 = screenRect.y() + screenRect.height();
        }
        break;
        }

        if (!m_monitorRectList.contains(monitorRect)) m_monitorRectList << monitorRect;
    }

    m_extralRectList.clear();
    for (auto s : DIS_INS->screens()) {
        // 屏幕此位置不可停靠时,不用监听这块区域
        if (!DIS_INS->canDock(s, m_position)) continue;

        MonitRect monitorRect;
        QRect screenRect = s->geometry();
        screenRect.setSize(screenRect.size() * s->devicePixelRatio());

        switch (m_position) {
        case Top:
        case Bottom: {
            monitorRect.x1 = screenRect.x();
            monitorRect.y1 = screenRect.y();
            monitorRect.x2 = screenRect.x() + screenRect.width();
            monitorRect.y2 = screenRect.y() + screenRect.height() - realDockSize;
        }
        break;
        case Left: {
            monitorRect.x1 = screenRect.x() + realDockSize;
            monitorRect.y1 = screenRect.y();
            monitorRect.x2 = screenRect.x() + screenRect.width();
            monitorRect.y2 = screenRect.y() + screenRect.height();
        }
        break;
        case Right: {
            monitorRect.x1 = screenRect.x();
            monitorRect.y1 = screenRect.y();
            monitorRect.x2 = screenRect.x() + screenRect.width() - realDockSize;
            monitorRect.y2 = screenRect.y() + screenRect.height();
        }
        break;
        }

        if (!m_extralRectList.contains(monitorRect)) m_extralRectList << monitorRect;
    }

    m_registerKey = m_eventInter->RegisterAreas(m_monitorRectList, flags);
    m_extralRegisterKey = m_extralEventInter->RegisterAreas(m_extralRectList, flags);
}

/**
 * @brief 判断屏幕是否为复制模式的依据，第一个屏幕的X和Y值是否和其他的屏幕的X和Y值相等
 * 对于复制模式，这两个值肯定是相等的，如果不是复制模式，这两个值肯定不等，目前支持双屏
 */
bool MultiScreenWorker::isCopyMode()
{
    QList<QScreen *> screens = DIS_INS->screens();
    if (screens.size() < 2)
        return false;

    // 在多个屏幕的情况下，如果所有屏幕的位置的X和Y值都相等，则认为是复制模式
    QRect rect0 = screens[0]->availableGeometry();
    for (int i = 1; i < screens.size(); i++) {
        QRect rect = screens[i]->availableGeometry();
        if (rect0.x() != rect.x() || rect0.y() != rect.y())
            return false;
    }

    return true;
}

/**
 * @brief 这里用到xcb去设置任务栏的高度，比较特殊，参考_NET_WM_STRUT_PARTIAL属性
 * 在屏幕旋转后，所有参数以控制中心自定义设置里主屏显示的图示为准（旋转不用特殊处理）
 */
void MultiScreenWorker::requestNotifyWindowManager()
{
    static QRect lastRect = QRect();
    static int lastScreenWidth = 0;
    static int lastScreenHeight = 0;

    /* 在非主屏或非一直显示状态时，清除任务栏区域，不挤占应用 */
    if (m_hideMode != HideMode::KeepShowing || (!isCopyMode() && m_ds.current() != m_ds.primary())) {
        lastRect = QRect();

        if (QX11Info::display()) XcbMisc::instance()->clear_strut_partial(xcb_window_t(m_parent->winId()));

        return;
    }

    QRect dockGeometry = getDockShowGeometry(m_ds.current(), m_position, true);
    if (lastRect == dockGeometry && lastScreenWidth == DIS_INS->screenRawWidth() && lastScreenHeight == DIS_INS->screenRawHeight())
        return;

    lastRect = dockGeometry;
    lastScreenWidth = DIS_INS->screenRawWidth();
    lastScreenHeight = DIS_INS->screenRawHeight();

    XcbMisc::Orientation orientation = XcbMisc::OrientationTop;
    double strut = 0;
    double strutStart = 0;
    double strutEnd = 0;

    switch (m_position) {
    case Top:
    case Position::Bottom:
        orientation = XcbMisc::OrientationBottom;
        strut = DIS_INS->screenRawHeight() - dockGeometry.y();
        strutStart = dockGeometry.x();
        strutEnd = qMin(dockGeometry.x() + dockGeometry.width(), dockGeometry.right());
        break;
    case Position::Left:
        orientation = XcbMisc::OrientationLeft;
        strut = dockGeometry.x() + dockGeometry.width();
        strutStart = dockGeometry.y();
        strutEnd = qMin(dockGeometry.y() + dockGeometry.height(), dockGeometry.bottom());
        break;
    case Position::Right:
        orientation = XcbMisc::OrientationRight;
        strut = DIS_INS->screenRawWidth() - dockGeometry.x();
        strutStart = dockGeometry.y();
        strutEnd = qMin(dockGeometry.y() + dockGeometry.height(), dockGeometry.bottom());
        break;
    }

    if (QX11Info::display()) {
        XcbMisc::instance()->set_strut_partial(static_cast<xcb_window_t>(m_parent->winId()), orientation,
                                           static_cast<uint>(strut), // 设置窗口与屏幕边缘距离，需要乘缩放
                                           static_cast<uint>(strutStart),                   // 设置任务栏起点坐标（上下为x，左右为y）
                                           static_cast<uint>(strutEnd));                    // 设置任务栏终点坐标（上下为x，左右为y）
    }
}

void MultiScreenWorker::initConnection()
{
    connect(m_dockInter, &DBusDock::OpacityChanged, this, [this](const double value){
        if (int(m_opacity * 100) != int(value * 100)) {
            m_opacity = value;
            emit opacityChanged(quint8(value * 255));
        }
    });
    connect(qApp, &QApplication::primaryScreenChanged, this, [this]{
        m_ds.updatePrimary(DIS_INS->primary());

        if (DIS_INS->screenRawHeight() != 0 && DIS_INS->screenRawWidth() != 0)
            resetDockScreen();
    });
    connect(DIS_INS, &DisplayManager::primaryScreenChanged, this, [this]{
        m_ds.updatePrimary(DIS_INS->primary());

        if (DIS_INS->screenRawHeight() != 0 && DIS_INS->screenRawWidth() != 0)
            resetDockScreen();
    });
    connect(DIS_INS, &DisplayManager::screenInfoChanged, this, &MultiScreenWorker::resetDockScreen);

    connect(m_launcherInter, static_cast<void (DBusLuncher::*)(bool) const>(&DBusLuncher::VisibleChanged), this, [ this ](bool value) { setStates(LauncherDisplay, value); });

    /** FIXME
     * 这里关联的信号有时候收不到是因为 qt-dbus-factory 中的 changed 的信号有时候会发不出来，
     * qt-dbus-factory 中的 DBusExtendedAbstractInterface::internalPropGet 在同步调用情况下，会将缓存中的数据写入属性中，
     * 导致后面 onPropertyChanged 中的判断认为属性值没变，就没有发出 changed 信号。
     * 建议：前端仅在初始化时主动获取一次 dbus 中的值存储在成员变量中，并建立 changed 信号连接，后面所有用到那个值的地方，均获取成员变量;
     * 或去修改 qt-dbus-factory，取消 DBusExtendedAbstractInterface::internalPropGet 中将数据写入属性值，
     * 但是 qt-dbus-factory 修改涉及面较广，需要大量测试确认没有问题，再合入。
     */
#if 0
    //    connect(m_dockInter, &DBusDock::PositionChanged, this, &MultiScreenWorker::onPositionChanged);
    //    connect(m_dockInter, &DBusDock::DisplayModeChanged, this, &MultiScreenWorker::onDisplayModeChanged);
    //    connect(m_dockInter, &DBusDock::HideModeChanged, this, &MultiScreenWorker::hideModeChanged);
    //    connect(m_dockInter, &DBusDock::HideStateChanged, this, &MultiScreenWorker::hideStateChanged);
#else
    QDBusConnection::sessionBus().connect("com.deepin.dde.daemon.Dock",
                                          "/com/deepin/dde/daemon/Dock",
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged",
                                          "sa{sv}as",
                                          this, SLOT(handleDbusSignal(QDBusMessage)));
#endif

    connect(this, &MultiScreenWorker::requestUpdateFrontendGeometry, this, [this]{
        const QRect rect = getDockShowGeometry(m_ds.current(), m_position, true);

        //!!! 向com.deepin.dde.daemon.Dock的SetFrontendWindowRect接口设置区域时,此区域的高度或宽度不能为0,否则会导致其HideState属性循环切换,造成任务栏循环显示或隐藏
        if (rect.width() > 0 && rect.height() > 0)
            m_dockInter->SetFrontendWindowRect(int(rect.x()), int(rect.y()), uint(rect.width()), uint(rect.height()));
    });

    connect(m_delayWakeTimer, &QTimer::timeout, this, [this]{
        QString toScreen = m_delayScreen;
        m_delayScreen.clear();
        // 移动Dock至相应屏相应位置
        if (testState(LauncherDisplay) || testState(ChangePositionAnimationStart)) return;

        // 复制模式．不需要响应切换屏幕
        if (DIS_INS->screens().size() == 2 && DIS_INS->screens().first()->geometry() == DIS_INS->screens().last()->geometry()) {
            QRect rect = getDockShowGeometry(m_ds.current(), m_position);
            if(m_parent->isHidden()) rect = getDockHideGeometry(rect, m_position);
            m_parent->setGeometry(rect);
            return;
        }
        m_ds.updateDockedScreen(toScreen);

        // 检查当前屏幕的当前位置是否允许显示,不允许需要更新显示信息(这里应该在函数外部就处理好,不应该走到这里)
        // 检查边缘是否允许停靠
        QScreen *curScreen = DIS_INS->screen(toScreen);
        if (curScreen && DIS_INS->canDock(curScreen, m_position))
            changeDockPosition(m_ds.last(), m_ds.current(), m_position, m_position);
    });

    //　更新任务栏内容展示方式
    connect(this, &MultiScreenWorker::requestUpdateLayout, this, [this]{

    });
}

/**
 * @brief MultiScreenWorker::displayAnimation
 * 任务栏显示或隐藏过程的动画。
 * @param screen 任务栏要显示的屏幕
 * @param pos 任务栏显示的位置（上：0，右：1，下：2，左：3）
 * @param act 显示（隐藏）任务栏
 * @return void
 */
void MultiScreenWorker::displayAnimation(const QString &screen, const Position &pos, AniAction act)
{
    if (!testState(AutoHide) || ani->state() == QPropertyAnimation::Running) return;

    if ((act == AniAction::Hide && m_parent->isHidden()) || (act == AniAction::Show && !m_parent->isHidden())) {
        if(testState(ChangePositionAnimationStart)) emit ani->finished();
        return;
    }

    QRect dockShowRect = getDockShowGeometry(screen, pos);
    QRect dockHideRect = getDockHideGeometry(dockShowRect, pos);

    ani->setStartValue(dockHideRect.topLeft());
    ani->setEndValue(dockShowRect.topLeft());
    ani->setDirection(act == AniAction::Show ? QAbstractAnimation::Forward : QAbstractAnimation::Backward);

    ani->start();
    if(act == AniAction::Show) m_parent->show();
}

/**
 * @brief MultiScreenWorker::displayAnimation
 * 任务栏显示或隐藏过程的动画。
 * @param screen 任务栏要显示的屏幕
 * @param act 显示（隐藏）任务栏
 * @return void
 */
void MultiScreenWorker::displayAnimation(const QString &screen, AniAction act)
{
    return displayAnimation(screen, m_position, act);
}

/**
 * @brief changeDockPosition    做一个动画操作
 * @param fromScreen            上次任务栏所在的屏幕
 * @param toScreen              任务栏要移动到的屏幕
 * @param fromPos               任务栏上次的方向
 * @param toPos                 任务栏打算移动到的方向
 */
void MultiScreenWorker::changeDockPosition(QString fromScreen, QString toScreen, const Position &fromPos, const Position &toPos)
{
    if (fromScreen == toScreen && fromPos == toPos) return;

    setStates(ChangePositionAnimationStart);

    // 更新屏幕信息
    m_ds.updateDockedScreen(toScreen);

    QObject *conn(new QObject(this));
    connect(ani, &QPropertyAnimation::finished, conn, [this, conn, fromPos, toPos, toScreen] {
        ani->disconnect(conn);

        // 如果更改了显示位置，在显示之前应该更新一下界面布局方向
        if (fromPos != toPos) {
            if(fromPos == Dock::Bottom || toPos == Dock::Bottom)
                m_parent->resize(getDockShowGeometry(m_ds.current(), toPos).size());
            m_parent->panel()->setPositonValue(toPos);
        }

        connect(ani, &QPropertyAnimation::finished, conn, [ this, conn ] {
            setStates(ChangePositionAnimationStart, false);
            conn->deleteLater();

            if(m_hideMode == HideMode::KeepShowing) requestNotifyWindowManager();
            emit requestUpdateFrontendGeometry();
            onRequestUpdateRegionMonitor();
        });

        displayAnimation(toScreen, toPos, Show);
    });

    displayAnimation(fromScreen, fromPos, Hide);
}

/**
 * @brief getValidScreen        获取一个当前任务栏可以停靠的屏幕，优先使用主屏
 * @return
 */
QString MultiScreenWorker::getValidScreen(const Position &pos)
{
    //TODO 考虑在主屏幕名变化时自动更新，是不是就不需要手动处理了
    m_ds.updatePrimary(DIS_INS->primary());
    if(DIS_INS->screens().size() == 1)
        return DIS_INS->primary();

    if (DIS_INS->canDock(DIS_INS->screen(m_ds.current()), pos))
        return m_ds.current();

    if (DIS_INS->canDock(qApp->primaryScreen(), pos))
        return DIS_INS->primary();

    for (auto s : DIS_INS->screens()) {
        if (DIS_INS->canDock(s, pos))
            return s->name();
    }

    return QString();
}

/**
 * @brief resetDockScreen     检查一下当前屏幕所在边缘是够允许任务栏停靠，不允许的情况需要更换下一块屏幕
 */
void MultiScreenWorker::resetDockScreen()
{
    if (ani->state() == QPropertyAnimation::Running || testState(ChangePositionAnimationStart)) return;

    m_ds.updateDockedScreen(getValidScreen(m_position));
    // 更新任务栏自身信息
    /**
      *注意这里要先对parent()进行setFixedSize，在分辨率切换过程中，setGeometry可能会导致其大小未改变
      */
    QRect rect = getDockShowGeometry(m_ds.current(), m_position);
    if(m_parent->isHidden()) rect = getDockHideGeometry(rect, m_position);
    m_parent->setGeometry(rect);

    requestNotifyWindowManager();
    emit requestUpdateFrontendGeometry();
    onRequestUpdateRegionMonitor();
}

/**
 * @brief checkDaemonDockService
 * 避免com.deepin.dde.daemon.Dock服务比dock晚启动，导致dock启动后的状态错误
 */
void MultiScreenWorker::checkDaemonDockService()
{
    auto connectionInit = [ this ] {
        m_dockInter = new DBusDock("com.deepin.dde.daemon.Dock", "/com/deepin/dde/daemon/Dock", QDBusConnection::sessionBus(), this);
        m_launcherInter = new DBusLuncher("com.deepin.dde.Launcher", "/com/deepin/dde/Launcher", QDBusConnection::sessionBus(), this);

        emit dockInterReady(m_dockInter);
        setStates(LauncherDisplay, m_launcherInter->visible());

        // connect(m_dockInter, &DBusDock::ServiceRestarted, this, &MultiScreenWorker::resetDockScreen);
        // connect(m_dockInter, &DBusDock::WindowSizeFashionChanged, this, &MultiScreenWorker::resetDockScreen);

        initConnection();
        checkXEventMonitorService();

        // init data
        m_position = static_cast<Dock::Position >(m_dockInter->position());
        m_hideMode = static_cast<Dock::HideMode >(m_dockInter->hideMode());
        m_hideState = static_cast<Dock::HideState >(m_dockInter->hideState());
        m_opacity = m_dockInter->opacity();
        m_windowSize = m_dockInter->windowSizeFashion();

        resetDockScreen();
    };

    static const QString serverName = "com.deepin.dde.daemon.Dock";
    QDBusConnectionInterface *ifc = QDBusConnection::sessionBus().interface();

    if (ifc->isServiceRegistered(serverName))
        connectionInit();
    else
        connect(ifc, &QDBusConnectionInterface::serviceOwnerChanged, this, [ this, connectionInit, ifc ](const QString & name, const QString & oldOwner, const QString & newOwner) {
            Q_UNUSED(oldOwner)
            if (name == serverName && !newOwner.isEmpty()) {
                connectionInit();
                disconnect(ifc);
            }
        });
}

/**
 * @brief checkDaemonXEventMonitorService
 * 避免com.deepin.api.XEventMonitor服务比dock晚启动，导致dock启动后的状态错误
 */
void MultiScreenWorker::checkXEventMonitorService()
{
    const QString serverName = "com.deepin.api.XEventMonitor";

    auto connectionInit = [ this, serverName ] {
        m_eventInter = new XEventMonitor(serverName, "/com/deepin/api/XEventMonitor", QDBusConnection::sessionBus());
        m_extralEventInter = new XEventMonitor(serverName, "/com/deepin/api/XEventMonitor", QDBusConnection::sessionBus());

        connect(m_eventInter, &XEventMonitor::CursorMove, this, [this](int x, int y, const QString &key){
            if (m_registerKey == key && !testState(MousePress) && !testState(ChangePositionAnimationStart)) {
                if (ani->state() == QPropertyAnimation::Running) return;

                QString toScreen;

                /**
                 * 坐标位于当前屏幕边缘时,当做屏幕内移动处理(防止鼠标移动到边缘时不唤醒任务栏)
                 * 使用screenAtByScaled获取屏幕名时,实际上获取的不一定是当前屏幕
                 * 举例:点(100,100)不在(0,0,100,100)的屏幕上
                 */
                if (onScreenEdge(m_ds.current(), QPoint(x, y)))
                    toScreen = m_ds.current();
                else if (QScreen *screen = Utils::screenAtByScaled(QPoint(x, y)))
                    toScreen = screen->name();

                if(toScreen.isEmpty()) return;

                // 任务栏显示状态，但需要切换屏幕
                if (toScreen != m_ds.current()) {
                    if(!m_delayScreen.isEmpty() && m_delayScreen != toScreen) m_delayWakeTimer->stop();
                    if (!m_delayWakeTimer->isActive()) {
                        m_delayScreen = toScreen;
                        m_delayWakeTimer->start();
                    }
                } else if (m_parent->isHidden())
                    displayAnimation(m_ds.current(), AniAction::Show);
            }
        });
        connect(m_eventInter, &XEventMonitor::ButtonPress, this, [ this ] { setStates(MousePress, true); });
        connect(m_eventInter, &XEventMonitor::ButtonRelease, this, [ this ] { setStates(MousePress, false); });

        connect(m_extralEventInter, &XEventMonitor::CursorMove, this, [this](int x, int y, const QString &key){
            Q_UNUSED(x);
            Q_UNUSED(y);
            if (m_extralRegisterKey == key && !testState(MousePress) && !testState(ChangePositionAnimationStart))
            {
                // FIXME:每次都要重置一下，是因为qt中的QScreen类缺少nameChanged信号，后面会给上游提交patch修复
                m_ds.updateDockedScreen(getValidScreen(position()));

                // 鼠标移动到任务栏界面之外，停止计时器（延时2秒改变任务栏所在屏幕）
                m_delayWakeTimer->stop();

                if (m_hideMode == HideMode::KeepShowing || m_hideState == HideState::Show)
                    displayAnimation(m_ds.current(), AniAction::Show);
                else if ((m_hideMode == HideMode::KeepHidden || m_hideMode == HideMode::SmartHide) && m_hideState == HideState::Hide)
                    displayAnimation(m_ds.current(), AniAction::Hide);
            }
        });

        // 只需要在屏幕信息变化的时候更新，其他时间不需要更新
        onRequestUpdateRegionMonitor();
    };

    QDBusConnectionInterface *ifc = QDBusConnection::sessionBus().interface();
    if (!ifc->isServiceRegistered(serverName)) {
        connect(ifc, &QDBusConnectionInterface::serviceOwnerChanged, this, [ this, serverName, connectionInit, ifc ](const QString & name, const QString & oldOwner, const QString & newOwner) {
            Q_UNUSED(oldOwner)
            if (name == serverName && !newOwner.isEmpty()) {
                connectionInit();
                disconnect(ifc);
            }
        });
    } else {
        connectionInit();
    }
}

/**
 * @brief 获取任务栏显示时的参数。目前多屏情况下缩放保持一致，如果后续缩放规则修改，这里需要重新调整
 *
 * @param screenName    当前屏幕名字
 * @param pos           任务栏位置
 * @param displaymode   任务栏显示模式
 * @param withoutScale  是否考虑缩放（true:获取的是真实值; false:获取的是前端认为的值(默认)）
 * @return QRect        任务栏参数
 */
QRect MultiScreenWorker::getDockShowGeometry(const QString &screenName, const Position &pos, bool withoutScale)
{
    const qreal ratio = withoutScale ? 1 : qApp->devicePixelRatio();
    const int dockSize = static_cast<int>( m_windowSize * (withoutScale ? qApp->devicePixelRatio() : 1));
    const int dockCount = DockItemManager::instance()->itemCount();

    QRect rect = SharedData::instance()->getRect(screenName, pos, ratio, dockCount);
    if(rect.isEmpty())
    {
        for (auto s : DIS_INS->screens()) {
            if (s->name() == screenName) {
                // 拿到当前显示器缩放之前的分辨率
                QRect screenRect = s->handle()->geometry();

                switch (pos) {
                case Top:
                case Position::Bottom:
                    rect.setY(static_cast<int>(screenRect.y() + screenRect.height() / ratio - dockSize));
                    rect.setHeight(dockSize);
                    break;
                case Position::Left:
                    rect.setX(screenRect.x());
                    rect.setWidth(dockSize);
                    break;
                case Position::Right:
                    rect.setX(static_cast<int>(screenRect.x() + screenRect.width() / ratio - dockSize));
                    rect.setWidth(dockSize);
                    break;
                }
                updateDockRect(rect, screenRect, pos, ratio, dockSize, dockCount);
                break;
            }
        }
        SharedData::instance()->setRect(screenName, pos, ratio, dockCount, rect);
    }

    return rect;
}

/**
 * @brief 获取任务栏隐藏时的参数。目前多屏情况下缩放保持一致，如果后续缩放规则修改，这里需要重新调整
 *
 * @param screenName    当前屏幕名字
 * @param pos           任务栏位置
 * @param displaymode   任务栏显示模式
 * @param withoutScale  是否考虑缩放（true:获取的是真实值; false:获取的是前端认为的值(默认)）
 * @return QRect        任务栏参数
 */
QRect MultiScreenWorker::getDockHideGeometry(const QRect showRect, const Position &pos)
{
    QRect rect = showRect;

    switch (pos) {
    case Position::Bottom:
        rect.setY(showRect.bottom());
        break;
    case Position::Left:
        rect.setX(showRect.x()-showRect.width());
        break;
    case Position::Right:
        rect.setX(showRect.right());
        break;
    case Top:
        break;
    }
    return rect;
}

QScreen *MultiScreenWorker::screenByName(const QString &screenName)
{
    foreach (QScreen *screen, qApp->screens()) {
        if (screen->name() == screenName)
            return screen;
    }
    return nullptr;
}

bool MultiScreenWorker::onScreenEdge(const QString &screenName, const QPoint &point)
{
    QScreen *screen = screenByName(screenName);
    if (screen) {
        const QRect r { screen->geometry() };
        const QRect rect { r.topLeft(), r.size() *screen->devicePixelRatio() };

        // 除了要判断鼠标的x坐标和当前区域的位置外，还需要判断当前的坐标的y坐标是否在任务栏的区域内
        // 因为有如下场景：任务栏在左侧，双屏幕屏幕上下拼接，此时鼠标沿着最左侧x=0的位置移动到另外一个屏幕
        // 如果不判断y坐标的话，此时就认为鼠标在当前任务栏的边缘，导致任务栏在这种状况下没有跟随鼠标
        if ((rect.x() == point.x() || rect.right() == point.x()) && point.y() >= rect.top() && point.y() <= rect.bottom())
            return true;

        // 同上，不过此时屏幕是左右拼接，任务栏位于上方或者下方
        if ((rect.y() == point.y() || rect.bottom() == point.y()) && point.x() >= rect.left() && point.x() <= rect.right())
            return true;
    }

    return false;
}

const QPoint MultiScreenWorker::rawXPosition(const QPoint &scaledPos)
{
    if(auto screen = Utils::screenAtByScaled(scaledPos))
        return screen->geometry().topLeft() + (scaledPos - screen->geometry().topLeft()) * screen->devicePixelRatio();

    return scaledPos;
}

void MultiScreenWorker::updateDockRect(QRect &dockRect, QRect screenRect, Position position, qreal ratio, int dockSize, int count)
{
    const int splitterWidth = (DockItemManager::instance()->hasWindowItem() ? MODE_PADDING + SPLITER_SIZE : 0) + (MODE_PADDING + SPLITER_SIZE);
    dockSize = dockSize-2;
    int length;

    switch (position)
    {
    case Top:
    case Bottom:
            length = screenRect.width() / ratio;
            if ((dockSize + MODE_PADDING ) * count + splitterWidth > length * 0.9)
            {
                dockRect.setWidth(length * 0.9);
                setWindowSize(round((length * 0.9 - splitterWidth) / count - MODE_PADDING));
                dockRect.setHeight(m_windowSize);
            }
            else
                dockRect.setWidth((dockSize + MODE_PADDING) * count + splitterWidth);

            dockRect.moveLeft(screenRect.left() + (screenRect.width() / ratio - dockRect.width()) / 2);
        break;
    case Left:
    case Right:
            length = screenRect.height() / ratio;
            if ((dockSize + MODE_PADDING) * count + splitterWidth > length * 0.9)
            {
                dockRect.setHeight(length * 0.9);
                setWindowSize(round((length * 0.9 - splitterWidth ) / count - MODE_PADDING));
                dockRect.setWidth(m_windowSize);
            }
            else
                dockRect.setHeight((dockSize + MODE_PADDING) * count + splitterWidth);

            dockRect.moveTop(screenRect.y() + (screenRect.height() / ratio - dockRect.height()) / 2);
    }
}
