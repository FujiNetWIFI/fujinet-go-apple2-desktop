/*
 * The debugger's Video tab (Qt6 Widgets): a thin view over core/include/apple2vidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QPoint>
#include <QWidget>

#include "apple2vidtab.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTableWidget;

class VideoPicture;

class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(apple2session *session, QWidget *parent = nullptr);
    ~VideoTab() override;

    /* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
    void refresh(bool force);
    /* Decoded view of one mode and page (Follow screen off): the window's APPLE2_DEBUGGER_VIEW. */
    void showDecoded(int mode, int page);
    /* Pin a point of the picture, in zoomed picture pixels: the window's APPLE2_VIDEO_PIN. */
    void pinAt(int x, int y);

signals:
    /* Show in Memory: a range in a bank (APPLE2_BANK_*) */
    void showInMemory(int bank, int lo, int hi);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    friend class VideoPicture;
    void refreshView(unsigned what);
    void syncControls();
    void updatePixelSection();
    void fillSwitches();
    void fillColours();
    void fillTiming();
    void note(const QString &text);
    void breakOnPixel();
    void showPixelInMemory();

    apple2vid_tab *m_tab = nullptr;
    apple2debug *m_dbg = nullptr;
    QComboBox *m_view = nullptr, *m_mode = nullptr, *m_page = nullptr, *m_zoom = nullptr;
    QCheckBox *m_follow = nullptr, *m_mixed = nullptr;
    QList<QCheckBox *> m_ov;
    QLabel *m_status = nullptr, *m_note = nullptr;
    VideoPicture *m_pic = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTabWidget *m_sections = nullptr;
    QLabel *m_pixelText = nullptr, *m_timingText = nullptr;
    QTableWidget *m_switches = nullptr, *m_colours = nullptr;
    QProgressBar *m_beam = nullptr;
    bool m_updating = false;
    QPoint m_pendingPin;
    bool m_havePin = false;
};
