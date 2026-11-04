//
//  MenuItemProperties.cpp
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 1/28/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "MenuItemProperties.h"

#include <QDebug>


MenuItemProperties::MenuItemProperties(const QString& menuName, const QString& menuItemName,
                        const QString& shortcutKey, bool checkable, bool checked, bool separator) :
    menuName(menuName),
    menuItemName(menuItemName),
    shortcutKey(shortcutKey),
    shortcutKeyEvent(),
    shortcutKeySequence(shortcutKey),
    isCheckable(checkable),
    isChecked(checked),
    isSeparator(separator)
{
}

MenuItemProperties::MenuItemProperties(const QString& menuName, const QString& menuItemName,
                        const KeyEvent& shortcutKeyEvent, bool checkable, bool checked, bool separator) :
    menuName(menuName),
    menuItemName(menuItemName),
    shortcutKeyEvent(shortcutKeyEvent),
    shortcutKeySequence(shortcutKeyEvent),
    isCheckable(checkable),
    isChecked(checked),
    isSeparator(separator)
{
}
