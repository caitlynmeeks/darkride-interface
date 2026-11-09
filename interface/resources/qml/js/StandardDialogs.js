//
//  StandardDialogs.js
//  interface/resources/qml/js
//
//  The numbers of QtQuick.Dialogs 1's StandardButton and StandardIcon (QMessageBox's), under the same names, for the
//  dialogs that only used those: QtQuick.Dialogs 1 needs Qt Quick Controls 1, which Qt 6 doesn't have. Imported
//  "as OriginalDialogs" where the old import was, so OriginalDialogs.StandardButton.Ok reads as before.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

.pragma library

var StandardButton = {
    NoButton: 0x00000000,
    Ok: 0x00000400,
    Save: 0x00000800,
    SaveAll: 0x00001000,
    Open: 0x00002000,
    Yes: 0x00004000,
    YesToAll: 0x00008000,
    No: 0x00010000,
    NoToAll: 0x00020000,
    Abort: 0x00040000,
    Retry: 0x00080000,
    Ignore: 0x00100000,
    Close: 0x00200000,
    Cancel: 0x00400000,
    Discard: 0x00800000,
    Help: 0x01000000,
    Apply: 0x02000000,
    Reset: 0x04000000,
    RestoreDefaults: 0x08000000
};

var StandardIcon = {
    NoIcon: 0,
    Information: 1,
    Warning: 2,
    Critical: 3,
    Question: 4
};
