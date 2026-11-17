/*
 * controllers_test -- the KDE Controllers window, offscreen: hidden at start,
 * fixed size, keys steering the joystick and Alt as Open Apple, the mouse
 * holding a button and dragging the stick (and letting go when it drags off,
 * and when the window hides), the Joystick / Paddles and Colour segments, the
 * paddles' knobs turned by a drag and the wheel, the keyboard's caps (a click
 * types, Shift latches, Caps Lock toggles, the Apple caps hold the buttons),
 * Map taking a key and a gamepad button, F9 hiding it, and a pad's row with its
 * port picker -- on an SDL virtual gamepad, with no machine. Waits poll with a
 * five second ceiling, so a slow runner is merely slow. SKIPs (77) where SDL
 * has no virtual joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "apple2session.h"
#include "bindings.h"
#include "controllers/ControllersWindow.h"
#include "gamepad_sdl.h"

namespace {

using apple2ctl::kMargin;
using apple2ctl::kPaddleUnit;
using apple2ctl::kUnit;

int failures = 0;

void check(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

/* Waits until the target is held (or not); the window's own timers and the pad
 * thread both need the event loop. */
void expectHeld(apple2session *sess, int target, bool want, const char *what)
{
    for (int tries = 0; tries < 250; tries++) {
        if ((apple2session_target_held(sess, target) != 0) == want) {
            std::printf("ok: %s\n", what);
            return;
        }
        QTest::qWait(20);
    }
    std::printf("FAIL: %s\n", what);
    failures++;
}

QWidget *findWindow()
{
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String("controllers")) return top;
    return nullptr;
}

QWidget *child(QWidget *win, const char *name)
{
    return win->findChild<QWidget *>(QString::fromLatin1(name), Qt::FindChildrenRecursively);
}

/* the pixel centre of a labelled part of a view, in the stage: port -1 is any */
QPoint centreOf(int view, const char *label, int port)
{
    int count = 0;
    const apple2_control *layout = apple2session_controls_layout(view, &count, nullptr, nullptr);
    const QPointF origin = view == APPLE2_VIEW_CONSOLE ? apple2ctl::consoleOrigin()
                         : view == APPLE2_VIEW_PADDLES ? apple2ctl::paddlesOrigin() : apple2ctl::joystickOrigin();
    const double unit = view == APPLE2_VIEW_PADDLES ? kPaddleUnit : kUnit;
    for (int idx = 0; idx < count; idx++) {
        if (!layout[idx].label || std::strcmp(layout[idx].label, label) != 0) continue;
        if (port >= 0 && layout[idx].port != port) continue;
        const double cx = layout[idx].kind == APPLE2_CTL_KNOB ? layout[idx].cx : layout[idx].xpos + layout[idx].width / 2;
        const double cy = layout[idx].kind == APPLE2_CTL_KNOB ? layout[idx].cy : layout[idx].ypos + layout[idx].height / 2;
        return QPoint(int(origin.x() + cx * unit), int(origin.y() + cy * unit));
    }
    return QPoint(-1, -1);
}

/* the stick's cap at rest, moved by (x, y) of full travel, as a stage pixel */
QPoint stickAt(double x, double y)
{
    int count = 0;
    const apple2_control *layout = apple2session_controls_layout(APPLE2_VIEW_JOYSTICK, &count, nullptr, nullptr);
    for (int idx = 0; idx < count; idx++) {
        if (layout[idx].kind != APPLE2_CTL_STICK) continue;
        const QPointF origin = apple2ctl::joystickOrigin();
        return QPoint(int(origin.x() + (layout[idx].capx + x * layout[idx].reach) * kUnit),
                      int(origin.y() + (layout[idx].capy + y * layout[idx].reach) * kUnit));
    }
    return QPoint(-1, -1);
}

QPushButton *findButton(QWidget *win, const char *text)
{
    for (QPushButton *button : win->findChildren<QPushButton *>())
        if (button->text() == QLatin1String(text)) return button;
    return nullptr;
}

QComboBox *waitForPadRow(QWidget *win, int items)
{
    for (int tries = 0; tries < 250; tries++) {
        for (QComboBox *box : win->findChildren<QComboBox *>())
            if (box->count() == items && box->itemText(0) == QLatin1String("Automatic")) return box;
        QTest::qWait(20);
    }
    return nullptr;
}

constexpr int LEFT = APPLE2_TARGET_JOY(APPLE2_ACT_LEFT);
constexpr int RIGHT = APPLE2_TARGET_JOY(APPLE2_ACT_RIGHT);
constexpr int UP = APPLE2_TARGET_JOY(APPLE2_ACT_UP);
constexpr int BTN0 = APPLE2_TARGET_JOY(APPLE2_ACT_BTN0);
constexpr int BTN1 = APPLE2_TARGET_JOY(APPLE2_ACT_BTN1);
constexpr int P1BTN = APPLE2_TARGET_PADDLE(1, APPLE2_PACT_BUTTON);

void keyTests(apple2session *sess, QWidget *win)
{
    /* the numpad steers (the default) */
    QTest::keyPress(win, Qt::Key_8, Qt::KeypadModifier);
    expectHeld(sess, UP, true, "a typed Keypad 8 steers the stick up");
    QTest::keyRelease(win, Qt::Key_8, Qt::KeypadModifier);
    expectHeld(sess, UP, false, "and its release lets go");

    /* Alt is the Open Apple key: it reaches the machine from this window too */
    QTest::keyPress(win, Qt::Key_Alt);
    expectHeld(sess, BTN0, true, "Alt held in the window is Open Apple (button 0)");
    QTest::keyRelease(win, Qt::Key_Alt);
    expectHeld(sess, BTN0, false, "and its release");
}

void mouseTests(apple2session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    check(stage != nullptr, "the stage widget exists");
    if (!stage) return;

    const QPoint button0 = centreOf(APPLE2_VIEW_JOYSTICK, "Button 0", -1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, button0);
    expectHeld(sess, BTN0, true, "pressing button 0 holds it");
    QTest::mouseMove(stage, QPoint(2, 2));
    expectHeld(sess, BTN0, false, "dragging off lets it go");
    QTest::mouseMove(stage, button0);
    expectHeld(sess, BTN0, true, "and back on holds it again");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, button0);
    expectHeld(sess, BTN0, false, "releasing lets go");

    const QPoint button1 = centreOf(APPLE2_VIEW_JOYSTICK, "Button 1", -1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, button1);
    expectHeld(sess, BTN1, true, "button 1 is the top one");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, button1);
    expectHeld(sess, BTN1, false, "and lets go");

    /* the stick follows the pointer, proportionally */
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, stickAt(1.0, 0));
    expectHeld(sess, RIGHT, true, "a press right of the cap tilts the stick right");
    float pdl0 = 0, pdl1 = 0;
    apple2session_axes_get(sess, &pdl0, &pdl1);
    check(pdl0 > 0.9f, "PDL0 follows to the stop");
    QTest::mouseMove(stage, stickAt(-1.0, 0));
    expectHeld(sess, LEFT, true, "dragging to the left tilts it left");
    expectHeld(sess, RIGHT, false, "and lets go of right");
    QTest::mouseMove(stage, stickAt(0.5, 0));
    apple2session_axes_get(sess, &pdl0, &pdl1);
    check(pdl0 > 0.4f && pdl0 < 0.6f, "half way gives half a deflection (the stick is analog)");
    QTest::mouseMove(stage, stickAt(0, -1.0));
    expectHeld(sess, UP, true, "and up is up");
    win->hide();
    expectHeld(sess, UP, false, "hiding the window lets go of what the mouse held");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, stickAt(0, 0));
    win->show();

    /* a console button posts its action on release, like a pad or a key */
    (void)apple2session_sysaction_take(sess);
    const QPoint reset = centreOf(APPLE2_VIEW_CONSOLE, "Reset", -1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, reset);
    check(apple2session_sysaction_take(sess) == 0, "Reset does nothing on the press");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, reset);
    check((apple2session_sysaction_take(sess) & (1u << APPLE2SESSION_SYSACT_RESET)) != 0, "and posts Reset on the release");
}

void paddleTests(apple2session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    auto *paddles = win->findChild<QPushButton *>(QStringLiteral("mode1"));
    check(stage && paddles, "the Paddles segment exists");
    if (!stage || !paddles) return;
    paddles->click();
    check(apple2session_controller_mode(sess) == APPLE2_MODE_PADDLES, "Paddles switches the mode");
    check(apple2session_controller_ports(sess) == 2, "and gives the pads two ports");
    check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
          "the window is still fixed size");

    const QPoint dial = centreOf(APPLE2_VIEW_PADDLES, "Knob", 0);
    apple2session_knob_set(sess, 0, 0.0f);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, dial + QPoint(0, -22));
    check(std::fabs(apple2session_knob_get(sess, 0)) < 0.06f, "a press straight up centres the knob");
    QTest::mouseMove(stage, dial + QPoint(22, 22));
    check(apple2session_knob_get(sess, 0) > 0.95f, "dragging to the lower right turns it full clockwise");
    QTest::mouseMove(stage, dial + QPoint(-22, 22));
    check(apple2session_knob_get(sess, 0) < -0.95f, "and to the lower left full counter-clockwise");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, dial + QPoint(-22, 22));
    check(apple2session_knob_get(sess, 0) < -0.95f, "a knob holds where it was left");

    apple2session_knob_set(sess, 1, 0.0f);
    const QPoint dial1 = centreOf(APPLE2_VIEW_PADDLES, "Knob", 1);
    QWheelEvent wheel(QPointF(dial1), QPointF(stage->mapToGlobal(dial1)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(stage, &wheel);
    check(std::fabs(apple2session_knob_get(sess, 1) - 0.08f) < 0.011f, "a wheel notch nudges the knob");

    const QPoint button = centreOf(APPLE2_VIEW_PADDLES, "Button", 1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, button);
    expectHeld(sess, P1BTN, true, "paddle 1's button is held by the mouse");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, button);
    expectHeld(sess, P1BTN, false, "and let go");

    /* back to the joystick, and the other colour */
    auto *joystick = win->findChild<QPushButton *>(QStringLiteral("mode0"));
    if (joystick) joystick->click();
    check(apple2session_controller_mode(sess) == APPLE2_MODE_JOYSTICK, "Joystick switches back");
    auto *platinum = win->findChild<QPushButton *>(QStringLiteral("model1"));
    check(platinum != nullptr, "the Colour segment exists");
    if (platinum) {
        platinum->click();
        check(apple2session_controller_model(sess) == APPLE2_MODEL_IIC, "Platinum is the IIc look");
        win->findChild<QPushButton *>(QStringLiteral("model0"))->click();
        check(apple2session_controller_model(sess) == APPLE2_MODEL_IIE, "and Beige the IIe's");
    }
}

/* the cap with this legend (the nth of them), as a keyboard-widget pixel */
int capIndex(const char *legend, int nth = 0)
{
    int count = 0;
    const apple2_keycap *caps = apple2session_keyboard_layout(&count, nullptr, nullptr);
    for (int idx = 0; idx < count; idx++)
        if (caps[idx].legend && std::strcmp(caps[idx].legend, legend) == 0 && nth-- == 0) return idx;
    return -1;
}

QPoint capCentre(int idx)
{
    int count = 0;
    const apple2_keycap *caps = apple2session_keyboard_layout(&count, nullptr, nullptr);
    return QPoint(int(kMargin + (caps[idx].xpos + caps[idx].width / 2) * kUnit),
                  int(kMargin + (caps[idx].ypos + caps[idx].height / 2) * kUnit));
}

int firstOfKind(int kind, uint32_t keysym)
{
    int count = 0;
    const apple2_keycap *caps = apple2session_keyboard_layout(&count, nullptr, nullptr);
    for (int idx = 0; idx < count; idx++)
        if (caps[idx].kind == kind && caps[idx].keysym == keysym) return idx;
    return -1;
}

void keyboardTests(apple2session *sess, QWidget *win)
{
    auto *box = win->findChild<QCheckBox *>(QStringLiteral("keyboardBox"));
    QWidget *kbd = child(win, "keyboard");
    check(box && kbd, "the Keyboard check box and the keyboard exist");
    if (!box || !kbd) return;
    check(!box->isChecked() && !kbd->isVisible(), "the keyboard starts hidden");
    box->click();
    check(kbd->isVisible() && apple2session_get_int(sess, "controllers_keyboard", 0) == 1, "the check box shows it, remembered");
    check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
          "the window is fixed size with it shown");

    const int q = capIndex("Q");
    QTest::mousePress(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(q));
    check((apple2session_keyboard_held(sess, q) & 1) != 0, "a pressed cap is down");
    QTest::mouseRelease(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(q));

    /* Shift latches until a second press, Caps Lock toggles */
    const int shift = capIndex("Shift");
    QTest::mouseClick(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(shift));
    check((apple2session_keyboard_held(sess, shift) & 2) != 0, "Shift latches");
    QTest::mouseClick(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(shift));
    check((apple2session_keyboard_held(sess, shift) & 2) == 0, "and a second press lets go");
    const int caps = firstOfKind(APPLE2_KEYCAP_LOCK, 0);
    const int before = apple2session_caps_lock(sess);
    QTest::mouseClick(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(caps));
    check(apple2session_caps_lock(sess) == !before, "Caps Lock toggles");
    QTest::mouseClick(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(caps));
    check(apple2session_caps_lock(sess) == before, "and back");

    /* the Apple caps are held, and press the buttons */
    const int open = firstOfKind(APPLE2_KEYCAP_APPLE, 0), closed = firstOfKind(APPLE2_KEYCAP_APPLE, 1);
    QTest::mousePress(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(open));
    expectHeld(sess, BTN0, true, "the Open Apple cap holds button 0");
    QTest::mouseRelease(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(open));
    expectHeld(sess, BTN0, false, "and lets go");
    QTest::mousePress(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(closed));
    expectHeld(sess, BTN1, true, "the Option cap holds button 1");
    QTest::mouseRelease(kbd, Qt::LeftButton, Qt::NoModifier, capCentre(closed));
    expectHeld(sess, BTN1, false, "and lets go");

    box->click();
    check(!kbd->isVisible(), "unticking hides it");
}

void mapTests(apple2session *sess, QWidget *win, SDL_Joystick *vjoy)
{
    QPushButton *mapButton = findButton(win, "Map");
    QWidget *stage = child(win, "stage");
    check(mapButton && stage, "the Map button exists");
    if (!mapButton || !stage) return;

    mapButton->click();
    check(mapButton->text() == QLatin1String("Done"), "Map arms");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(APPLE2_VIEW_JOYSTICK, "Button 0", -1));
    QTest::keyClick(win, Qt::Key_Q);
    check(bindings_target_keysym(BTN0) == 'q', "the next key is bound to the picked part");
    expectHeld(sess, BTN0, false, "a key taken by Map is not typed into the machine");

    /* a side of the stick's well */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, stickAt(-1.0, 0.6) + QPoint(-40, 0));
    QTest::keyClick(win, Qt::Key_Z);
    check(bindings_target_keysym(LEFT) == 'z', "clicking the left of the well picks Left");

    /* Alt is the Open Apple key and cannot be mapped */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(APPLE2_VIEW_JOYSTICK, "Button 1", -1));
    QTest::keyClick(win, Qt::Key_Alt);
    check(bindings_target_keysym(BTN1) != APPLE2SESSION_KEYSYM_LALT, "Alt is refused");

    QTest::keyClick(win, Qt::Key_F9);
    check(!win->isVisible(), "F9 hides the window");
    win->show();

    mapButton = findButton(win, "Map");
    check(mapButton != nullptr, "the window comes back");
    if (!mapButton) return;
    mapButton->click();
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(APPLE2_VIEW_JOYSTICK, "Button 1", -1));
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        for (int tries = 0; tries < 250 && bindings_target_pad(BTN1) != APPLE2_PAD_BTN_RIGHT_STICK; tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        check(bindings_target_pad(BTN1) == APPLE2_PAD_BTN_RIGHT_STICK, "the next gamepad button is bound to the picked part");
    }
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::keyClick(win, Qt::Key_Escape);
    check(mapButton->text() == QLatin1String("Map"), "Escape twice leaves Map");

    bindings_reset_defaults(sess);
    check(bindings_target_keysym(BTN0) == APPLE2SESSION_KEYSYM_KP_0, "Defaults put the key back");
}

void padRowTests(apple2session *sess, QWidget *win, const char *padName)
{
    QComboBox *box = waitForPadRow(win, 2);
    check(box != nullptr, "a pad's row appears with Automatic and Joystick");
    if (!box) return;
    int slot = -1;
    for (int idx = 0; idx < apple2session_gamepad_count(sess); idx++) {
        char got[64];
        apple2session_gamepad_name(sess, idx, got, sizeof got);
        if (std::strcmp(got, padName) == 0) slot = idx;
    }
    check(slot >= 0, "the pad is in the session");
    if (slot < 0) return;
    check(box->currentIndex() == 0, "it starts on Automatic");
    box->setCurrentIndex(1);
    check(apple2session_gamepad_assignment(sess, slot) == 0, "choosing Joystick assigns it");
    check(apple2session_gamepad_effective_port(sess, slot) == 0, "and it plays the joystick");
    box->setCurrentIndex(0);
    check(apple2session_gamepad_assignment(sess, slot) == -1, "Automatic clears the assignment");

    /* in paddles mode the row offers the paddles */
    win->findChild<QPushButton *>(QStringLiteral("mode1"))->click();
    QComboBox *paddleBox = waitForPadRow(win, 3);
    check(paddleBox != nullptr, "in paddles mode the row offers Paddle 0 and Paddle 1");
    if (paddleBox) {
        paddleBox->setCurrentIndex(2);
        check(apple2session_gamepad_assignment(sess, slot) == 1, "choosing Paddle 1 assigns it");
        check(apple2session_gamepad_effective_port(sess, slot) == 1, "and it plays paddle 1");
        paddleBox->setCurrentIndex(0);
    }
    win->findChild<QPushButton *>(QStringLiteral("mode0"))->click();
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir config, data;
    if (!config.isValid() || !data.isValid()) return 1;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    static const char padName[] = "apple2test kde pad";
    desc.name = padName;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Joystick *vjoy = SDL_OpenJoystick(vid);

    const QByteArray cfg = config.path().toLocal8Bit(), dat = data.path().toLocal8Bit();
    apple2session_paths paths;
    std::memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg.constData();
    paths.data_dir = dat.constData();
    paths.fujinet_lib = "";      /* never the user's real FujiNet */
    apple2session *sess = apple2session_new(&paths);
    /* the pad layer, which the session starts with a machine: here it runs alone */
    if (!sess || apple2_gamepad_start(sess) != 0) {
        std::printf("SKIP: the gamepad layer would not start\n");
        return 77;
    }

    QWidget host;
    check(!apple2ControllersVisible(), "hidden at start");
    apple2ControllersToggle(&host, sess);
    check(apple2ControllersVisible(), "F9 / the menu shows it");
    QWidget *win = findWindow();
    check(win != nullptr, "the window exists");
    if (win) {
        check(win->windowTitle() == QLatin1String("Controllers"), "titled Controllers");
        check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
              "fixed size: minimum == maximum == size");
        keyTests(sess, win);
        mouseTests(sess, win);
        paddleTests(sess, win);
        keyboardTests(sess, win);
        win->show();
        mapTests(sess, win, vjoy);
        padRowTests(sess, win, padName);
        apple2ControllersToggle(&host, sess);
        check(!apple2ControllersVisible(), "toggling again hides it");
    }

    apple2_gamepad_stop();
    if (vjoy) SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    apple2session_free(sess);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
