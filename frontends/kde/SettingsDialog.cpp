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
#include <QMessageBox>
#include <QVBoxLayout>

void SettingsDialog::done(int result)
{
    const QByteArray addr = m_externalAddr->text().trimmed().toUtf8();
    const int external = m_external->isChecked() ? 1 : 0;
    char host[64];
    int port;

    if (external &&
        apple2session_parse_hostport(addr.constData(), host, sizeof host,
                                     &port) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("External FujiNet"),
            QStringLiteral("\"%1\" is not an address:port. Example: "
                           "0.0.0.0:1985")
                .arg(m_externalAddr->text()));
        m_externalAddr->setFocus();
        return;
    }
    if (apple2session_get_int(m_session, "fujinet_external", 0) != external) {
        apple2session_set_int(m_session, "fujinet_external", external);
        m_machineDirty = true;
    }
    if (QString::fromUtf8(apple2session_get_str(
            m_session, "fujinet_external_addr", "")) !=
        QString::fromUtf8(addr)) {
        apple2session_set_str(m_session, "fujinet_external_addr",
                              addr.constData());
        m_machineDirty = true;
    }
    QDialog::done(result);
}

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

    auto *fujinetBox = new QGroupBox(QStringLiteral("FujiNet"), this);
    auto *fujinetForm = new QFormLayout(fujinetBox);
    m_external = new QCheckBox(fujinetBox);
    m_external->setChecked(
        apple2session_get_int(m_session, "fujinet_external", 0));
    fujinetForm->addRow(QStringLiteral("Use external FujiNet"), m_external);
    m_externalAddr = new QLineEdit(
        QString::fromUtf8(apple2session_get_str(m_session,
                                                "fujinet_external_addr", "")),
        fujinetBox);
    m_externalAddr->setPlaceholderText(QStringLiteral("0.0.0.0:1985"));
    m_externalAddr->setEnabled(m_external->isChecked());
    connect(m_external, &QCheckBox::toggled, m_externalAddr,
            &QWidget::setEnabled);
    fujinetForm->addRow(QStringLiteral("Listen address:port"),
                        m_externalAddr);
    auto *fujinetNote = new QLabel(
        QStringLiteral("Skips the built-in FujiNet and waits for another "
                       "instance to connect to the slot 7 card. 0.0.0.0 "
                       "accepts connections from the network; 127.0.0.1 only "
                       "from this computer."),
        fujinetBox);
    fujinetNote->setWordWrap(true);
    fujinetForm->addRow(fujinetNote);
    outer->addWidget(fujinetBox);

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
