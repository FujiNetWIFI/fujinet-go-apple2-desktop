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
#include "help/HelpWindow.h"

#include "bindings.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <utility>
#include <vector>

namespace {

const QString kGray = QStringLiteral("color: gray;");

/* One row per pad: the port it is set to (applied at once and remembered, so
 * not part of what OK restarts for) and the port it plays now. The choices
 * are the current controller's ports: the joystick, or Paddle 0 and 1. */
void fillPadRows(QFormLayout *form, std::vector<QWidget *> &rows, apple2session *session)
{
    for (QWidget *row : rows)
        form->removeRow(row);
    rows.clear();
    const int count = apple2session_gamepad_count(session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in: it is picked up as it appears."));
        none->setWordWrap(true);
        none->setStyleSheet(kGray);
        form->addRow(none);
        rows.push_back(none);
        return;
    }
    const int ports = apple2session_controller_ports(session);
    for (int idx = 0; idx < count && idx < 8; ++idx) {
        char name[128];
        apple2session_gamepad_name(session, idx, name, sizeof name);
        auto *box = new QComboBox;
        box->addItem(QStringLiteral("Automatic"));
        if (ports == 1) {
            box->addItem(QStringLiteral("Joystick"));
        } else {
            box->addItem(QStringLiteral("Paddle 0"));
            box->addItem(QStringLiteral("Paddle 1"));
        }
        const int assign = apple2session_gamepad_assignment(session, idx);
        box->setCurrentIndex(assign < 0 || assign >= ports ? 0 : assign + 1);
        auto tip = [session, idx](QComboBox *combo) {
            const int eff = apple2session_gamepad_effective_port(session, idx);
            const int mode = apple2session_controller_mode(session);
            combo->setToolTip(eff < 0 ? QStringLiteral("Playing nothing")
                              : mode == APPLE2_MODE_PADDLES ? QStringLiteral("Playing paddle %1").arg(eff)
                                                            : QStringLiteral("Playing the joystick"));
        };
        tip(box);
        QObject::connect(box, &QComboBox::currentIndexChanged, box, [session, idx, box, tip](int sel) {
            apple2session_gamepad_assign(session, idx, sel <= 0 ? -1 : sel - 1);
            tip(box);
        });
        form->addRow(QString::fromUtf8(name), box);
        rows.push_back(box);
    }
}

} // namespace

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

SettingsDialog::SettingsDialog(apple2session *session, QWidget *parent,
                               const QString &startTab)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Settings"));

    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    auto *machine = new QWidget;
    auto *machineLayout = new QVBoxLayout(machine);

    auto *machineBox = new QGroupBox(QStringLiteral("Machine"), this);
    auto *machineForm = new QFormLayout(machineBox);
    addOptionRow(machineForm, QStringLiteral("Model"), "machine",
                 apple2session_machine_name, apple2session_machine_name(0));
    addOptionRow(machineForm, QStringLiteral("Video"), "video_mode",
                 apple2session_video_mode_name,
                 apple2session_video_mode_name(0));
    machineLayout->addWidget(machineBox);
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
    machineLayout->addWidget(slotBox);

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
    machineLayout->addWidget(fujinetBox);

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
    machineLayout->addWidget(displayBox);

    machineLayout->addStretch(1);
    const int machineTab = tabs->addTab(machine, QStringLiteral("Machine"));
    Q_UNUSED(machineTab);

    /* The Input tab: what is plugged in, the pads, and the controls' two
     * shortcuts. Keys and gamepad buttons are re-mapped in the Controllers
     * window (Map). */
    auto *input = new QWidget;
    auto *iv = new QVBoxLayout(input);

    auto *plugBox = new QGroupBox(QStringLiteral("Controller"));
    auto *plugForm = new QFormLayout(plugBox);
    auto *modeBox = new QComboBox;
    modeBox->addItems({ QStringLiteral("Joystick"), QStringLiteral("Paddles") });
    modeBox->setCurrentIndex(apple2session_controller_mode(session));
    plugForm->addRow(QStringLiteral("Plugged into the game port"), modeBox);
    auto *modelBox = new QComboBox;
    modelBox->addItems({ QStringLiteral("Beige (Apple Joystick IIe)"), QStringLiteral("Platinum (Apple Joystick IIc)") });
    modelBox->setCurrentIndex(apple2session_controller_model(session));
    plugForm->addRow(QStringLiteral("Joystick colour"), modelBox);
    auto *plugNote = new QLabel(QStringLiteral(
        "The game port takes one joystick, or two paddles. The change applies at once and lets go of "
        "anything held."));
    plugNote->setWordWrap(true);
    plugNote->setStyleSheet(kGray);
    plugForm->addRow(plugNote);
    iv->addWidget(plugBox);

    auto *padsBox = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsLayout = new QVBoxLayout(padsBox);
    auto *enablePads = new QCheckBox(QStringLiteral("Enable gamepads (restarts the machine)"));
    enablePads->setObjectName(QStringLiteral("enablePads"));
    enablePads->setChecked(apple2session_get_int(session, "enable_gamepad", 1) != 0);
    connect(enablePads, &QCheckBox::toggled, this, [this](bool on) {
        apple2session_set_int(m_session, "enable_gamepad", on ? 1 : 0);
        m_machineDirty = true;   /* the pad thread starts with the machine */
    });
    padsLayout->addWidget(enablePads);
    auto *padsNote = new QLabel(QStringLiteral(
        "The first pad plays the joystick (or Paddle 0), the next Paddle 1, unless one is chosen here."));
    padsNote->setWordWrap(true);
    padsNote->setStyleSheet(kGray);
    padsLayout->addWidget(padsNote);
    auto *padForm = new QFormLayout;
    padsLayout->addLayout(padForm);
    iv->addWidget(padsBox);
    auto padRows = std::make_shared<std::vector<QWidget *>>();
    auto padState = std::make_shared<std::pair<unsigned, int>>(apple2session_gamepad_generation(session),
                                                               apple2session_controller_mode(session));
    fillPadRows(padForm, *padRows, session);
    auto *padTimer = new QTimer(this);
    connect(padTimer, &QTimer::timeout, this, [this, padForm, padRows, padState] {
        const unsigned gen = apple2session_gamepad_generation(m_session);
        const int mode = apple2session_controller_mode(m_session);
        if (gen == padState->first && mode == padState->second) return;
        *padState = { gen, mode };
        fillPadRows(padForm, *padRows, m_session);
    });
    padTimer->start(1000);
    connect(modeBox, &QComboBox::currentIndexChanged, this, [this, padForm, padRows, padState](int mode) {
        apple2session_set_controller_mode(m_session, mode);
        *padState = { apple2session_gamepad_generation(m_session), mode };
        fillPadRows(padForm, *padRows, m_session);
    });
    connect(modelBox, &QComboBox::currentIndexChanged, this,
            [this](int model) { apple2session_set_controller_model(m_session, model); });

    auto *controls = new QGroupBox(QStringLiteral("Controls"));
    auto *controlsLayout = new QVBoxLayout(controls);
    auto *controlsNote = new QLabel(QStringLiteral(
        "Keys and gamepad buttons are re-mapped in the Controllers window (Map)."));
    controlsNote->setWordWrap(true);
    controlsNote->setStyleSheet(kGray);
    controlsLayout->addWidget(controlsNote);
    auto *controlsRow = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"));
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openWindow = new QPushButton(QStringLiteral("Open Controllers"));
    openWindow->setToolTip(QStringLiteral("Close Settings and open the Controllers window"));
    controlsRow->addWidget(resetControls);
    controlsRow->addWidget(openWindow);
    controlsRow->addStretch(1);
    controlsLayout->addLayout(controlsRow);
    auto *controlsState = new QLabel;
    controlsLayout->addWidget(controlsState);
    iv->addWidget(controls);
    iv->addStretch(1);
    connect(resetControls, &QPushButton::clicked, this, [this, controlsState] {
        bindings_reset_defaults(m_session);
        controlsState->setText(QStringLiteral("Every key and gamepad button is back to its default."));
    });
    connect(openWindow, &QPushButton::clicked, this, [this] {
        m_openControllers = true;
        accept();
    });
    const int inputTab = tabs->addTab(input, QStringLiteral("Input"));
    if (startTab == QLatin1String("Input"))
        tabs->setCurrentIndex(inputTab);
    outer->addWidget(tabs);

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    auto pageContext = [inputTab](int i) {
        return i == inputTab ? QStringLiteral("prefs/Input") : QStringLiteral("prefs/Machine");
    };
    setProperty("apple2HelpContext", pageContext(tabs->currentIndex()));
    connect(tabs, &QTabWidget::currentChanged, this,
            [this, pageContext](int i) { setProperty("apple2HelpContext", pageContext(i)); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Help, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { apple2HelpFor(this); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), this, [this] { apple2HelpFor(this); });
    outer->addWidget(buttons);
}
