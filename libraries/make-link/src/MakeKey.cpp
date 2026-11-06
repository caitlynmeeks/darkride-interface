//
//  MakeKey.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeKey.h"

#include <QtCore/QRegularExpression>

#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace {

const char* SERVICE = "Darkride Make";
const char* ACCOUNT = "Anthropic API key";

#ifdef Q_OS_MAC
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
#endif

}  // namespace

bool MakeKey::looksRight(const QString& key, QString& why) {
    if (key.startsWith("sk-ant-oat")) {
        why = "That's a sign-in token from a Claude plan, which an app may not use for you. Use Make from Terminal "
              "with your plan instead, or make an API key at console.anthropic.com.";
        return false;
    }
    static const QRegularExpression SHAPE("^sk-ant-api[0-9A-Za-z_-]{30,300}$");
    if (!SHAPE.match(key).hasMatch()) {
        why = "That doesn't look like an Anthropic API key (it starts sk-ant-api, with no spaces).";
        return false;
    }
    return true;
}

bool MakeKey::store(const QString& key, QString& why) {
    if (!looksRight(key, why)) {
        return false;
    }
#ifdef Q_OS_MAC
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
#else
    why = "Keeping a key is on macOS only for now; use Make from Terminal.";
    return false;
#endif
}

bool MakeKey::isStored() {
#ifdef Q_OS_MAC
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
#else
    return false;
#endif
}

bool MakeKey::forget() {
#ifdef Q_OS_MAC
    CFMutableDictionaryRef item = query();
    const OSStatus status = SecItemDelete(item);
    CFRelease(item);
    return status == errSecSuccess || status == errSecItemNotFound;
#else
    return true;
#endif
}

QByteArray MakeKey::readForChild() {
#ifdef Q_OS_MAC
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
#else
    return QByteArray();
#endif
}
