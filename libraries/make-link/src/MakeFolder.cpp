//
//  MakeFolder.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeFolder.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#ifdef Q_OS_WIN
#include <string>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#ifdef Q_OS_WIN

// this user's SID as text (the same read as MakeLinkServer's, kept small here so the folder code stands alone)
std::wstring thisUserSidText() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::wstring();
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::wstring result;
    if (size > 0) {
        std::string user(size, '\0');
        LPWSTR text = nullptr;
        if (GetTokenInformation(token, TokenUser, &user[0], size, &size) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&user[0])->User.Sid, &text)) {
            result = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return result;
}

// the owner of a file or folder as a SID string, empty when it can't be read
std::wstring ownerText(const std::wstring& path) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    std::wstring result;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr,
                              &descriptor) == ERROR_SUCCESS) {
        LPWSTR text = nullptr;
        if (owner && ConvertSidToStringSidW(owner, &text)) {
            result = text;
            LocalFree(text);
        }
        LocalFree(descriptor);
    }
    return result;
}

#endif

}  // namespace

namespace MakeFolder {

bool isLinkOrJunction(const QString& path) {
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(QDir::toNativeSeparators(path).toStdWString().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat info;
    return lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

bool makePrivate(const QString& path, QString& why) {
    const QString name = QFileInfo(path).fileName();
    if (isLinkOrJunction(path)) {
        why = QString("a link stands where the %1 folder goes").arg(name);
        return false;
    }
    if (!QFileInfo::exists(path)) {
        // the parents are the app's own data folder; the folder itself is made here with its access
        QDir().mkpath(QFileInfo(path).path());
#ifdef Q_OS_WIN
        if (!CreateDirectoryW(QDir::toNativeSeparators(path).toStdWString().c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            why = QString("can't make the %1 folder").arg(name);
            return false;
        }
#else
        if (mkdir(QFile::encodeName(path).constData(), 0700) != 0 && errno != EEXIST) {
            why = QString("can't make the %1 folder").arg(name);
            return false;
        }
#endif
        if (isLinkOrJunction(path)) {   // planted between the look and the make
            why = QString("a link stands where the %1 folder goes").arg(name);
            return false;
        }
    }
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        why = QString("something that isn't a folder stands where the %1 folder goes").arg(name);
        return false;
    }
    const std::wstring user = thisUserSidText();
    if (user.empty()) {
        why = "can't read who this user is";
        return false;
    }
    if (ownerText(native) != user) {
        why = QString("the %1 folder isn't this user's own").arg(name);
        return false;
    }
    // protected (no inheritance from above), full access for this user, SYSTEM and Administrators, inherited by what
    // is made inside (OICI), and nothing else
    const std::wstring sddl = L"D:P(A;OICI;FA;;;" + user + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    PSECURITY_DESCRIPTOR wanted = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &wanted, nullptr)) {
        why = QString("can't describe the %1 folder's access").arg(name);
        return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    const bool haveDacl = GetSecurityDescriptorDacl(wanted, &present, &dacl, &defaulted) && present;
    const DWORD set = haveDacl ? SetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()), SE_FILE_OBJECT,
                                                       DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                       nullptr, nullptr, dacl, nullptr)
                               : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(wanted);
    if (set != ERROR_SUCCESS) {
        why = QString("can't make the %1 folder private (%2)").arg(name).arg(set);
        return false;
    }
    return true;
#else
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        why = QString("something that isn't a folder stands where the %1 folder goes").arg(name);
        return false;
    }
    if (info.st_uid != getuid()) {
        why = QString("the %1 folder isn't this user's own").arg(name);
        return false;
    }
    if ((info.st_mode & 0777) != 0700 && chmod(QFile::encodeName(path).constData(), 0700) != 0) {
        why = QString("can't make the %1 folder private").arg(name);
        return false;
    }
    return true;
#endif
}

QString access(const QString& path) {
#ifdef Q_OS_WIN
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(QDir::toNativeSeparators(path).toStdWString().c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return QString();
    }
    LPWSTR text = nullptr;
    QString result;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text,
                                                             nullptr)) {
        result = QString::fromWCharArray(text);
        LocalFree(text);
        const QString user = QString::fromStdWString(thisUserSidText());
        if (!user.isEmpty()) {
            result.replace(user, QStringLiteral("this user"));
        }
    }
    LocalFree(descriptor);
    return result;
#else
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0) {
        return QString();
    }
    return QString::number(info.st_mode & 0777, 8);
#endif
}

bool copyProgram(const QString& source, const QString& copy, QString& why) {
    QFile from(source);
    if (!from.open(QIODevice::ReadOnly)) {
        why = "the app's helper program can't be read";
        return false;
    }
    const QByteArray bytes = from.readAll();
    from.close();
    if (bytes.isEmpty()) {
        why = "the app's helper program is empty";
        return false;
    }
    // whatever stands at the copy's place goes first, a file or a link alike: the folder is this user's own
    if (isLinkOrJunction(copy) || QFileInfo::exists(copy)) {
        if (!QFile::remove(copy)) {
            why = "an older copy of the helper can't be replaced";
            return false;
        }
    }
#ifdef Q_OS_WIN
    QFile out(copy);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(bytes) != bytes.size()) {
        why = "the helper's private copy can't be written";
        return false;
    }
    out.close();
#else
    // made new here, never through a link planted between the remove and this, this user's to run and nobody else's
    const QByteArray native = QFile::encodeName(copy);
    const int fd = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
    if (fd < 0) {
        why = "the helper's private copy can't be written";
        return false;
    }
    qint64 written = 0;
    while (written < bytes.size()) {
        const ssize_t chunk = ::write(fd, bytes.constData() + written, static_cast<size_t>(bytes.size() - written));
        if (chunk <= 0) {
            ::close(fd);
            why = "the helper's private copy can't be written";
            return false;
        }
        written += chunk;
    }
    if (::close(fd) != 0 || chmod(native.constData(), 0700) != 0) {
        why = "the helper's private copy can't be written";
        return false;
    }
#endif
    // the copy read back is the program, byte for byte, and no link
    QFile check(copy);
    if (isLinkOrJunction(copy) || !check.open(QIODevice::ReadOnly) || check.readAll() != bytes) {
        why = "the helper's private copy doesn't match the app's";
        return false;
    }
    return true;
}

}  // namespace MakeFolder
