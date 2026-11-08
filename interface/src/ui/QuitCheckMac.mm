//
//  QuitCheckMac.mm
//  interface/src/ui
//
//  macOS's part of the quit check (QuitCheck.h): whether the quit being handled is an Apple event, and whether it is
//  the system's (a log out, restart or shut down carries its reason in the event's kAEQuitReason attribute).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include <QtGlobal>
#ifdef Q_OS_MACOS

#include "QuitCheck.h"

#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>

namespace quitcheck {
namespace mac {

bool isQuitEvent(bool& fromSystem) {
    fromSystem = false;
    @autoreleasepool {
        NSAppleEventDescriptor* event = [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
        if (!event || [event eventClass] != kCoreEventClass || [event eventID] != kAEQuitApplication) {
            return false;
        }
        NSAppleEventDescriptor* reason = [event attributeDescriptorForKeyword:kAEQuitReason];
        if (reason) {
            OSType why = [reason typeCodeValue];
            if (why == 0) {
                why = [reason enumCodeValue];
            }
            fromSystem = why == kAELogOut || why == kAEReallyLogOut || why == kAEShowRestartDialog ||
                         why == kAEShowShutdownDialog || why == kAERestart || why == kAEShutDown;
        }
        return true;
    }
}

}
}

#endif // Q_OS_MACOS
