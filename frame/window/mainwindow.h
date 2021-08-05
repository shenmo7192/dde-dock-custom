﻿/*
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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "xcb/xcb_misc.h"
#include "../interfaces/constants.h"

#include <com_deepin_api_xeventmonitor.h>
#include <DPlatformWindowHandle>
#include <DWindowManagerHelper>
#include <DBlurEffectWidget>
#include <DGuiApplicationHelper>

#include <QWidget>

DWIDGET_USE_NAMESPACE

using XEventMonitor = ::com::deepin::api::XEventMonitor;
using namespace Dock;

class DockSettings;
class DragWidget;
class MainPanel;
class MainPanelControl;
class QTimer;
class MainWindow : public DBlurEffectWidget
{
    Q_OBJECT

    enum Flag{
        Motion = 1 << 0,
        Button = 1 << 1,
        Key    = 1 << 2
    };

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void setEffectEnabled(const bool enabled);
    void setComposite(const bool hasComposite);

    friend class MainPanel;
    friend class MainPanelControl;

public slots:
    void launch();

private:
    using QWidget::show;
    bool event(QEvent *e);
    void mousePressEvent(QMouseEvent *e);
    void keyPressEvent(QKeyEvent *e);
    void enterEvent(QEvent *e);
    void leaveEvent(QEvent *e);
    void dragEnterEvent(QDragEnterEvent *e);

    void initComponents();
    void initConnections();
    void resizeMainWindow();
    void resizeMainPanelWindow();

    const QPoint x11GetWindowPos();
    void x11MoveWindow(const int x, const int y);
    void x11MoveResizeWindow(const int x, const int y, const int w, const int h);
    void onRegionMonitorChanged(int x, int y, const QString &key);
    void updateRegionMonitorWatch();

signals:
    void panelGeometryChanged();

private slots:
    void positionChanged(const Position prevPos, const Position nextPos);
    void updateGeometry();
    void clearStrutPartial();
    void setStrutPartial();
    void compositeChanged();
    void internalMove(const QPoint &p);

    void expand();
    void narrow(const Position prevPos);
    void resetPanelEnvironment(const bool visible, const bool resetPosition = true);
    void updatePanelVisible();

    void adjustShadowMask();
    void positionCheck();

    void onMainWindowSizeChanged(QPoint offset);
    void onDragFinished();
    void themeTypeChanged(DGuiApplicationHelper::ColorType themeType);

private:
    bool m_launched;
    MainPanelControl *m_mainPanel;

    DPlatformWindowHandle m_platformWindowHandle;
    DWindowManagerHelper *m_wmHelper;
    XEventMonitor *m_eventInter;
    QString m_registerKey;
    QStringList m_registerKeys;

    QTimer *m_positionUpdateTimer;
    QTimer *m_expandDelayTimer;
    QTimer *m_leaveDelayTimer;
    QTimer *m_shadowMaskOptimizeTimer;
    QVariantAnimation *m_panelShowAni;
    QVariantAnimation *m_panelHideAni;

    XcbMisc *m_xcbMisc;
    DockSettings *m_settings;

    QSize m_size;
    DragWidget *m_dragWidget;
    Position m_curDockPos;
    Position m_newDockPos;
    bool m_mouseCauseDock;
};

#endif // MAINWINDOW_H
