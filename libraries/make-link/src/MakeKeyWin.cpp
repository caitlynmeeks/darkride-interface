//
//  MakeKeyWin.cpp
//  libraries/make-link/src
//
//  MakeKey on Windows: the builder's API key as one generic credential in Windows' Credential Manager, kept on this
//  computer for this user only and never roaming (CRED_PERSIST_LOCAL_MACHINE; security's make-blender-box-windows-linux,
//  "the key"). Never the registry, a setting or a file. Weaker than the Mac's keychain, as security says plainly: any
//  program running as this person can read it without a prompt, because Windows gives an ordinary app no secret of its
//  own; such a program could also read their keystrokes. The key is read only when Claude Code starts, into that
//  child's environment, and every copy this file makes is wiped before it's freed.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeKey.h"

#include <QtCore/QtGlobal>

#ifdef Q_OS_WIN

#include <windows.h>
#include <wincred.h>

namespace {

const wchar_t* const TARGET = L"Darkride Make: Anthropic API key";
const wchar_t* const USER = L"Darkride Make";

// the kept credential, or none; its secret wiped before Windows frees it
struct Kept {
    PCREDENTIALW credential { nullptr };
    Kept() {
        if (!CredReadW(TARGET, CRED_TYPE_GENERIC, 0, &credential)) {
            credential = nullptr;
        }
    }
    Kept(const Kept&) = delete;
    ~Kept() {
        if (credential) {
            SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
            CredFree(credential);
        }
    }
};

}  // namespace

bool MakeKey::store(const QString& key, QString& why) {
    if (!looksRight(key, why)) {
        return false;
    }
    QByteArray bytes = key.toUtf8();
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(TARGET);
    credential.UserName = const_cast<LPWSTR>(USER);
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;  // this computer only: never roams with the profile
    const bool kept = CredWriteW(&credential, 0) != FALSE;
    const DWORD error = kept ? 0 : GetLastError();
    SecureZeroMemory(bytes.data(), static_cast<size_t>(bytes.size()));
    if (!kept) {
        why = "Windows' Credential Manager didn't keep it (" + QString::number(error) + ").";
        return false;
    }
    return true;
}

bool MakeKey::isStored() {
    // Windows has no look without the secret: it's read, then wiped at once
    return Kept().credential != nullptr;
}

bool MakeKey::forget() {
    return CredDeleteW(TARGET, CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

QByteArray MakeKey::readForChild() {
    Kept kept;
    if (!kept.credential || kept.credential->CredentialBlobSize == 0) {
        return QByteArray();
    }
    return QByteArray(reinterpret_cast<const char*>(kept.credential->CredentialBlob),
                      static_cast<int>(kept.credential->CredentialBlobSize));
}

#endif  // Q_OS_WIN
