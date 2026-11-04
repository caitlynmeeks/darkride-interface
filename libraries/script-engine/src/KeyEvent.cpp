//
//  KeyEvent.cpp
//  script-engine/src
//
//  Created by Stephen Birarda on 2014-10-27.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "KeyEvent.h"

#include <qdebug.h>
#include <qscriptengine.h>

#include "ScriptEngineLogging.h"

KeyEvent::KeyEvent() :
    key(0),
    text(""),
    isShifted(false),
    isControl(false),
    isMeta(false),
    isAlt(false),
    isKeypad(false),
    isValid(false),
    isAutoRepeat(false)
{
    
}

KeyEvent::KeyEvent(const QKeyEvent& event) {
    key = event.key();
    text = event.text();
    isShifted = event.modifiers().testFlag(Qt::ShiftModifier);
    isMeta = event.modifiers().testFlag(Qt::MetaModifier);
    isControl = event.modifiers().testFlag(Qt::ControlModifier);
    isAlt = event.modifiers().testFlag(Qt::AltModifier);
    isKeypad = event.modifiers().testFlag(Qt::KeypadModifier);
    isValid = true;
    isAutoRepeat = event.isAutoRepeat();
    
    // handle special text for special characters...
    if (key == Qt::Key_F1) {
        text = "F1";
    } else if (key == Qt::Key_F2) {
        text = "F2";
    } else if (key == Qt::Key_F3) {
        text = "F3";
    } else if (key == Qt::Key_F4) {
        text = "F4";
    } else if (key == Qt::Key_F5) {
        text = "F5";
    } else if (key == Qt::Key_F6) {
        text = "F6";
    } else if (key == Qt::Key_F7) {
        text = "F7";
    } else if (key == Qt::Key_F8) {
        text = "F8";
    } else if (key == Qt::Key_F9) {
        text = "F9";
    } else if (key == Qt::Key_F10) {
        text = "F10";
    } else if (key == Qt::Key_F11) {
        text = "F11";
    } else if (key == Qt::Key_F12) {
        text = "F12";
    } else if (key == Qt::Key_Up) {
        text = "UP";
    } else if (key == Qt::Key_Down) {
        text = "DOWN";
    } else if (key == Qt::Key_Left) {
        text = "LEFT";
    } else if (key == Qt::Key_Right) {
        text = "RIGHT";
    } else if (key == Qt::Key_Space) {
        text = "SPACE";
    } else if (key == Qt::Key_Escape) {
        text = "ESC";
    } else if (key == Qt::Key_Tab) {
        text = "TAB";
    } else if (key == Qt::Key_Delete) {
        text = "DELETE";
    } else if (key == Qt::Key_Backspace) {
        text = "BACKSPACE";
    } else if (key == Qt::Key_Shift) {
        text = "SHIFT";
    } else if (key == Qt::Key_Alt) {
        text = "ALT";
    } else if (key == Qt::Key_Control) {
        text = "CONTROL";
    } else if (key == Qt::Key_Meta) {
        text = "META";
    } else if (key == Qt::Key_PageDown) {
        text = "PAGE DOWN";
    } else if (key == Qt::Key_PageUp) {
        text = "PAGE UP";
    } else if (key == Qt::Key_Home) {
        text = "HOME";
    } else if (key == Qt::Key_End) {
        text = "END";
    } else if (key == Qt::Key_Help) {
        text = "HELP";
    } else if (key == Qt::Key_CapsLock) {
        text = "CAPS LOCK";
    } else if (key >= Qt::Key_A && key <= Qt::Key_Z && (isMeta || isControl || isAlt))  {
        // this little bit of hackery will fix the text character keys like a-z in cases of control/alt/meta where
        // qt doesn't always give you the key characters and will sometimes give you crazy non-printable characters
        const int lowerCaseAdjust = 0x20;
        QString unicode;
        if (isShifted) {
            text = QString(QChar(key));
        } else {
            text = QString(QChar(key + lowerCaseAdjust));
        }
    }
}

bool KeyEvent::operator==(const KeyEvent& other) const {
    return other.key == key
    && other.isShifted == isShifted
    && other.isControl == isControl
    && other.isMeta == isMeta
    && other.isAlt == isAlt
    && other.isKeypad == isKeypad;
}


KeyEvent::operator QKeySequence() const {
    int resultCode = 0;
    if (text.size() == 1 && text >= "a" && text <= "z") {
        resultCode = text.toUpper().at(0).unicode();
    } else {
        resultCode = key;
    }
    
    if (isMeta) {
        resultCode |= Qt::META;
    }
    if (isAlt) {
        resultCode |= Qt::ALT;
    }
    if (isControl) {
        resultCode |= Qt::CTRL;
    }
    if (isShifted) {
        resultCode |= Qt::SHIFT;
    }
    return QKeySequence(resultCode);
}

/**jsdoc
 * A keyboard key event.
 * @typedef {object} KeyEvent
 * @property {number} key - The Qt keyboard code of the key pressed. For a list of keyboard codes, see 
 *     <a href="http://doc.qt.io/qt-5/qt.html#Key-enum">http://doc.qt.io/qt-5/qt.html#Key-enum</a>.
 * @property {string} text - A string describing the key. For example, <code>"a"</code> for the "A" key if the Shift is not 
 *     pressed, <code>"F1"</code> for the F1 key, <code>"SPACE"</code> for the space bar.
 * @property {boolean} isShifted - <code>true</code> if a Shift key was pressed when the event was generated, otherwise 
 *     <code>false</code>.
 * @property {boolean} isMeta - <code>true</code> if a meta key was pressed when the event was generated, otherwise
 *     <code>false</code>. On Windows the "meta" key is the Windows key; on OSX it is the Control (Splat) key. 
 * @property {boolean} isControl - <code>true</code> if a control key was pressed when the event was generated, otherwise
 *     <code>false</code>. On Windows the "control" key is the Ctrl key; on OSX it is the Command key.
 * @property {boolean} isAlt - <code>true</code> if an Alt key was pressed when the event was generated, otherwise 
 *     <code>false</code>.
 * @property {boolean} isKeypad - <code>true</code> if the key is on the numeric keypad, otherwise <code>false</code>.
 * @property {boolean} isAutoRepeat - <code>true</code> if the event is a repeat for key that is being held down, otherwise 
 *     <code>false</code>.
 * @example <caption>Report the KeyEvent details for each key press.</caption>
 * Controller.keyPressEvent.connect(function (event) {
 *     print(JSON.stringify(event));
 * });
 */

int KeyEvent::keyFromText(const QString& text, bool& isKeypad) {
    const QString upper = text.toUpper();
    int key = 0;
    // if the text is a special command, then map it here...
    // TODO: come up with more elegant solution here, a map? is there a Qt function that gives nice names for keys?
    if (upper == "F1") {
        key = Qt::Key_F1;
    } else if (upper == "F2") {
        key = Qt::Key_F2;
    } else if (upper == "F3") {
        key = Qt::Key_F3;
    } else if (upper == "F4") {
        key = Qt::Key_F4;
    } else if (upper == "F5") {
        key = Qt::Key_F5;
    } else if (upper == "F6") {
        key = Qt::Key_F6;
    } else if (upper == "F7") {
        key = Qt::Key_F7;
    } else if (upper == "F8") {
        key = Qt::Key_F8;
    } else if (upper == "F9") {
        key = Qt::Key_F9;
    } else if (upper == "F10") {
        key = Qt::Key_F10;
    } else if (upper == "F11") {
        key = Qt::Key_F11;
    } else if (upper == "F12") {
        key = Qt::Key_F12;
    } else if (upper == "UP") {
        key = Qt::Key_Up;
        isKeypad = true;
    } else if (upper == "DOWN") {
        key = Qt::Key_Down;
        isKeypad = true;
    } else if (upper == "LEFT") {
        key = Qt::Key_Left;
        isKeypad = true;
    } else if (upper == "RIGHT") {
        key = Qt::Key_Right;
        isKeypad = true;
    } else if (upper == "SPACE") {
        key = Qt::Key_Space;
    } else if (upper == "ESC") {
        key = Qt::Key_Escape;
    } else if (upper == "TAB") {
        key = Qt::Key_Tab;
    } else if (upper == "DELETE") {
        key = Qt::Key_Delete;
    } else if (upper == "BACKSPACE") {
        key = Qt::Key_Backspace;
    } else if (upper == "SHIFT") {
        key = Qt::Key_Shift;
    } else if (upper == "ALT") {
        key = Qt::Key_Alt;
    } else if (upper == "CONTROL") {
        key = Qt::Key_Control;
    } else if (upper == "META") {
        key = Qt::Key_Meta;
    } else if (upper == "PAGE DOWN") {
        key = Qt::Key_PageDown;
    } else if (upper == "PAGE UP") {
        key = Qt::Key_PageUp;
    } else if (upper == "HOME") {
        key = Qt::Key_Home;
    } else if (upper == "END") {
        key = Qt::Key_End;
    } else if (upper == "HELP") {
        key = Qt::Key_Help;
    } else if (upper == "CAPS LOCK") {
        key = Qt::Key_CapsLock;
    } else {
        // Key values do not distinguish between uppercase and lowercase
        // and use the uppercase key value.
        key = upper.at(0).unicode();
    }
    return key;
}
