/*
 * Settings dialog: machine and slot options (applied by restarting the
 * session when the dialog closes) and display options (applied live).
 * Everything persists through the shared settings store, so the GNOME
 * frontend sees the same configuration.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

QComboBox *SettingsDialog::addOptionRow(QFormLayout *form,
                                        const QString &label, const char *key,
                                        const char *(*nameAt)(int),
                                        const char *def)
{
    auto *combo = new QComboBox(this);
    const QString current =
        QString::fromUtf8(apple2session_get_str(m_session, key, def));

    for (int i = 0;; i++) {
        const char *name = nameAt(i);
        if (!name) break;
        combo->addItem(QString::fromUtf8(name));
        if (current == QString::fromUtf8(name))
            combo->setCurrentIndex(i);
    }

    connect(combo, &QComboBox::currentTextChanged, this,
            [this, key](const QString &text) {
                const QByteArray utf8 = text.toUtf8();
                if (QString::fromUtf8(
                        apple2session_get_str(m_session, key, "")) == text)
                    return;
                apple2session_set_str(m_session, key, utf8.constData());
                m_machineDirty = true;
            });

    form->addRow(label, combo);
    return combo;
}

SettingsDialog::SettingsDialog(apple2session *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Settings"));

    auto *outer = new QVBoxLayout(this);

    auto *machineBox = new QGroupBox(QStringLiteral("Machine"), this);
    auto *machineForm = new QFormLayout(machineBox);
    addOptionRow(machineForm, QStringLiteral("Model"), "machine",
                 apple2session_machine_name, apple2session_machine_name(0));
    addOptionRow(machineForm, QStringLiteral("Video"), "video_mode",
                 apple2session_video_mode_name,
                 apple2session_video_mode_name(0));
    outer->addWidget(machineBox);

    auto *slotBox = new QGroupBox(QStringLiteral("Slots"), this);
    auto *slotForm = new QFormLayout(slotBox);
    addOptionRow(slotForm, QStringLiteral("Slot 3"), "slot3",
                 apple2session_slot3_name, "Empty");
    addOptionRow(slotForm, QStringLiteral("Slot 4"), "slot4",
                 apple2session_slot4_name, "Mockingboard");
    addOptionRow(slotForm, QStringLiteral("Slot 5"), "slot5",
                 apple2session_slot5_name, "Empty");
    addOptionRow(slotForm, QStringLiteral("Slot 6"), "slot6",
                 apple2session_slot6_name, "Disk II");
    addOptionRow(slotForm, QStringLiteral("Slot 7"), "slot7",
                 apple2session_slot7_name, "FujiNet");
    auto *slotNote = new QLabel(
        QStringLiteral("FujiNet lives in slot 7, the bootable SmartPort slot "
                       "the //e scans before the Disk ][ in slot 6."),
        slotBox);
    slotNote->setWordWrap(true);
    slotForm->addRow(slotNote);
    outer->addWidget(slotBox);

    auto *displayBox = new QGroupBox(QStringLiteral("Display"), this);
    auto *displayForm = new QFormLayout(displayBox);

    auto *aspect = new QComboBox(displayBox);
    aspect->addItems({QStringLiteral("TV (4:3)"),
                      QStringLiteral("Square pixels"),
                      QStringLiteral("Integer scale")});
    aspect->setCurrentIndex(apple2session_get_int(m_session, "aspect_mode", 0));
    connect(aspect, &QComboBox::currentIndexChanged, this, [this](int idx) {
        apple2session_set_int(m_session, "aspect_mode", idx);
        emit displayChanged();
    });
    displayForm->addRow(QStringLiteral("Aspect ratio"), aspect);

    /* Scanlines is a libretro core option (like Model/Video above), so it
     * needs a restart -- but smoothing is no longer its own control, it just
     * follows scanlines, and that half applies live like the rest of
     * Display. */
    auto *scanlines = new QCheckBox(displayBox);
    scanlines->setChecked(apple2session_get_int(m_session, "scanlines", 1));
    connect(scanlines, &QCheckBox::toggled, this, [this](bool on) {
        apple2session_set_int(m_session, "scanlines", on ? 1 : 0);
        apple2session_set_int(m_session, "smooth_scaling", on ? 1 : 0);
        m_machineDirty = true;
        emit displayChanged();
    });
    displayForm->addRow(QStringLiteral("Scanlines"), scanlines);
    outer->addWidget(displayBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
}
