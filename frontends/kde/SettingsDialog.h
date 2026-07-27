/*
 * Settings dialog for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QComboBox>
#include <QDialog>
#include <QVector>

#include "apple2session.h"

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(apple2session *session, QWidget *parent = nullptr);

    /* True when a machine/slot option changed, so the caller knows the
     * session has to be restarted for it to take effect. */
    bool machineDirty() const { return m_machineDirty; }

signals:
    /* Display-only options apply live. */
    void displayChanged();

private:
    /* A combo over one of the session's core-option label tables, stored by
     * name: these are strings AppleWin matches literally, so storing an index
     * would silently change meaning if the core reordered its values. */
    QComboBox *addOptionRow(class QFormLayout *form, const QString &label,
                            const char *key,
                            const char *(*nameAt)(int), const char *def);

    apple2session *m_session;
    bool m_machineDirty = false;
};
