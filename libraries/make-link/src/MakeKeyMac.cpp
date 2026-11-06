//
//  MakeKeyMac.cpp
//  libraries/make-link/src
//
//  MakeKey on macOS: the builder's API key as a generic password in the login keychain, this device only, never
//  synchronised. MakeKeyWindows.cpp (Credential Manager) and MakeKeyLinux.cpp (libsecret) take the same calls.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeKey.h"

#include <QtCore/QtGlobal>

#ifdef Q_OS_MAC

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

const char* SERVICE = "Darkride Make";
const char* ACCOUNT = "Anthropic API key";

// the one item's query: a generic password of this service and account, this device only
CFMutableDictionaryRef query() {
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(nullptr, SERVICE, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(nullptr, ACCOUNT, kCFStringEncodingUTF8);
    CFDictionarySetValue(dictionary, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dictionary, kSecAttrService, service);
    CFDictionarySetValue(dictionary, kSecAttrAccount, account);
    CFDictionarySetValue(dictionary, kSecAttrSynchronizable, kCFBooleanFalse);
    CFRelease(service);
    CFRelease(account);
    return dictionary;
}

}  // namespace

bool MakeKey::store(const QString& key, QString& why) {
    if (!looksRight(key, why)) {
        return false;
    }
    const QByteArray bytes = key.toUtf8();
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size());
    CFMutableDictionaryRef item = query();
    SecItemDelete(item);
    CFDictionarySetValue(item, kSecValueData, data);
    CFDictionarySetValue(item, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    CFStringRef label = CFStringCreateWithCString(nullptr, "Darkride Make: Anthropic API key", kCFStringEncodingUTF8);
    CFDictionarySetValue(item, kSecAttrLabel, label);
    const OSStatus status = SecItemAdd(item, nullptr);
    CFRelease(label);
    CFRelease(item);
    CFRelease(data);
    if (status != errSecSuccess) {
        why = "The keychain didn't keep it (" + QString::number(status) + ").";
        return false;
    }
    return true;
}

bool MakeKey::isStored() {
    CFMutableDictionaryRef item = query();
    CFDictionarySetValue(item, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(item, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(item, &found);
    if (found) {
        CFRelease(found);
    }
    CFRelease(item);
    return status == errSecSuccess;
}

bool MakeKey::forget() {
    CFMutableDictionaryRef item = query();
    const OSStatus status = SecItemDelete(item);
    CFRelease(item);
    return status == errSecSuccess || status == errSecItemNotFound;
}

QByteArray MakeKey::readForChild() {
    CFMutableDictionaryRef item = query();
    CFDictionarySetValue(item, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(item, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef found = nullptr;
    const OSStatus status = SecItemCopyMatching(item, &found);
    CFRelease(item);
    QByteArray key;
    if (status == errSecSuccess && found && CFGetTypeID(found) == CFDataGetTypeID()) {
        CFDataRef data = static_cast<CFDataRef>(found);
        key = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<int>(CFDataGetLength(data)));
    }
    if (found) {
        CFRelease(found);
    }
    return key;
}

#endif  // Q_OS_MAC
