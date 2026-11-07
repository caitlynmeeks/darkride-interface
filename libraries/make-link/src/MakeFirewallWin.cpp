//
//  MakeFirewallWin.cpp
//  libraries/make-link/src
//
//  The read of the installer's firewall rules for Make's Blender box (MakeFirewall.h): through INetFwPolicy2, the same
//  interface the installer writes them with, and the same checks as the installer's own read-back (isExactly() in
//  tools/packaging/windows-installer/firewall-rules): names and other strings compared ignoring case, SIDs as SIDs.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeFirewall.h"

#ifdef Q_OS_WIN

#include <initializer_list>

#include <QtCore/QElapsedTimer>

#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <sddl.h>
#include <userenv.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

const wchar_t* const MakeFirewall::PROFILE_NAME = L"Darkride.MakeBlender";
const wchar_t* const MakeFirewall::RULE_SID =
    L"S-1-15-2-579646438-1863717342-1691229924-821914009-3625600486-3308443927-3175962496";

namespace {

// the installer's two rules and their group (release, 11-07): fixed, matched by name
const wchar_t* const OUTBOUND = L"Darkride Make Blender box - block outbound";
const wchar_t* const INBOUND = L"Darkride Make Blender box - block inbound";
const wchar_t* const GROUP = L"Darkride Make";
const int REUSE_MS = 5000;

struct Bstr {
    BSTR value { nullptr };
    Bstr() = default;
    Bstr(const Bstr&) = delete;
    ~Bstr() { SysFreeString(value); }
    BSTR* put() {
        SysFreeString(value);
        value = nullptr;
        return &value;
    }
    QString text() const { return value ? QString::fromWCharArray(value, static_cast<int>(SysStringLen(value))) : QString(); }
};

bool same(const QString& value, const wchar_t* expected) {
    return QString::compare(value, QString::fromWCharArray(expected), Qt::CaseInsensitive) == 0;
}

bool sameSid(const QString& value, PSID sid) {
    if (value.isEmpty()) {
        return false;
    }
    PSID other = nullptr;
    if (!ConvertStringSidToSidW(value.toStdWString().c_str(), &other)) {
        return false;
    }
    const bool equal = EqualSid(other, sid) != FALSE;
    LocalFree(other);
    return equal;
}

// one of the two rules, field by field as the installer writes it; `wrong` names the first field that isn't
bool exactly(INetFwRule3* rule, NET_FW_RULE_DIRECTION direction, PSID sid, QString& wrong) {
    NET_FW_RULE_DIRECTION ruleDirection = NET_FW_RULE_DIR_MAX;
    NET_FW_ACTION action = NET_FW_ACTION_MAX;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    VARIANT_BOOL edge = VARIANT_TRUE;
    long profiles = 0;
    long protocol = 0;
    long secure = -1;
    Bstr local, remote, interfaces, group, application, service, localPorts, remotePorts, owner, users, remoteUsers,
        machines, package;
    auto empty = [](const Bstr& value) { return value.text().isEmpty(); };
    auto emptyOrAll = [](const Bstr& value) { return value.text().isEmpty() || value.text() == "*"; };
    const struct {
        const char* field;
        bool right;
    } checks[] = {
        { "direction", SUCCEEDED(rule->get_Direction(&ruleDirection)) && ruleDirection == direction },
        { "action", SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_BLOCK },
        { "enabled", SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE },
        { "profiles", SUCCEEDED(rule->get_Profiles(&profiles)) && (profiles & 7) == 7 },
        { "protocol", SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_ANY },
        { "local addresses", SUCCEEDED(rule->get_LocalAddresses(local.put())) && local.text() == "*" },
        { "remote addresses", SUCCEEDED(rule->get_RemoteAddresses(remote.put())) && remote.text() == "*" },
        { "interface types", SUCCEEDED(rule->get_InterfaceTypes(interfaces.put())) && same(interfaces.text(), L"All") },
        { "edge traversal", SUCCEEDED(rule->get_EdgeTraversal(&edge)) && edge == VARIANT_FALSE },
        { "group", SUCCEEDED(rule->get_Grouping(group.put())) && same(group.text(), GROUP) },
        { "program", SUCCEEDED(rule->get_ApplicationName(application.put())) && empty(application) },
        { "service", SUCCEEDED(rule->get_ServiceName(service.put())) && empty(service) },
        { "local ports", SUCCEEDED(rule->get_LocalPorts(localPorts.put())) && emptyOrAll(localPorts) },
        { "remote ports", SUCCEEDED(rule->get_RemotePorts(remotePorts.put())) && emptyOrAll(remotePorts) },
        // an owner or an authorized list would narrow whom the block covers
        { "owner", SUCCEEDED(rule->get_LocalUserOwner(owner.put())) && empty(owner) },
        { "local users", SUCCEEDED(rule->get_LocalUserAuthorizedList(users.put())) && empty(users) },
        { "remote users", SUCCEEDED(rule->get_RemoteUserAuthorizedList(remoteUsers.put())) && empty(remoteUsers) },
        { "remote machines", SUCCEEDED(rule->get_RemoteMachineAuthorizedList(machines.put())) && empty(machines) },
        { "secure flags", SUCCEEDED(rule->get_SecureFlags(&secure)) && secure == 0 },
        { "package", SUCCEEDED(rule->get_LocalAppPackageId(package.put())) && sameSid(package.text(), sid) },
    };
    for (const auto& check : checks) {
        if (!check.right) {
            wrong = check.field;
            return false;
        }
    }
    return true;
}

bool readRules(PSID sid, QString& why) {
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) {
        why = "can't read the firewall's settings";
        return false;
    }
    long current = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&current)) || current == 0) {
        why = "no current network profile";
        return false;
    }
    for (NET_FW_PROFILE_TYPE2 type : { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC }) {
        VARIANT_BOOL on = VARIANT_FALSE;
        if ((current & type) && (FAILED(policy->get_FirewallEnabled(type, &on)) || on != VARIANT_TRUE)) {
            why = "the firewall is off for this network";
            return false;
        }
    }
    ComPtr<INetFwRules> rules;
    ComPtr<IUnknown> listed;
    ComPtr<IEnumVARIANT> each;
    if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&listed)) || FAILED(listed.As(&each))) {
        why = "can't list the firewall's rules";
        return false;
    }
    int outbound = 0;
    int inbound = 0;
    int namingTheBox = 0;
    QString wrong;
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (each->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        ComPtr<INetFwRule3> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal) {
            item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
        }
        VariantClear(&item);
        if (!rule) {
            continue;
        }
        Bstr name, package;
        rule->get_Name(name.put());
        rule->get_LocalAppPackageId(package.put());
        if (sameSid(package.text(), sid)) {
            ++namingTheBox;
        }
        const bool isOutbound = same(name.text(), OUTBOUND);
        if (!isOutbound && !same(name.text(), INBOUND)) {
            continue;
        }
        ++(isOutbound ? outbound : inbound);
        QString field;
        if (!exactly(rule.Get(), isOutbound ? NET_FW_RULE_DIR_OUT : NET_FW_RULE_DIR_IN, sid, field) && wrong.isEmpty()) {
            wrong = QString("the %1 rule's %2").arg(isOutbound ? "outbound" : "inbound", field);
        }
    }
    if (outbound != 1 || inbound != 1) {
        why = QString("the installer's rules: %1 outbound, %2 inbound (one each wanted)").arg(outbound).arg(inbound);
        return false;
    }
    if (!wrong.isEmpty()) {
        why = wrong + " isn't as the installer writes it";
        return false;
    }
    // anything else naming the box (an allow rule, or one that overrides block rules) could open it (security, 11-07)
    if (namingTheBox != 2) {
        why = QString("%1 other firewall rule(s) name the box").arg(namingTheBox - 2);
        return false;
    }
    return true;
}

bool readNow(QString& why) {
    PSID derived = nullptr;
    if (FAILED(DeriveAppContainerSidFromAppContainerName(MakeFirewall::PROFILE_NAME, &derived))) {
        why = "can't derive the box's SID";
        return false;
    }
    // the SID the rules name must be the one Windows derives from the profile name: a slip in either is a withdrawal
    PSID named = nullptr;
    const bool matches = ConvertStringSidToSidW(MakeFirewall::RULE_SID, &named) && EqualSid(derived, named);
    LocalFree(named);
    bool inPlace = false;
    if (!matches) {
        why = "the box's SID isn't the one the installer's rules name";
    } else {
        // S_OK or S_FALSE: this call set COM up and undoes it; RPC_E_CHANGED_MODE: set up already, usable as it is
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        inPlace = readRules(derived, why);
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }
    FreeSid(derived);
    return inPlace;
}

}  // namespace

bool MakeFirewall::boxRulesInPlace(QString& why) {
    // the app's thread only (the dock's and Connect's): the last read, reused for a few seconds
    static QElapsedTimer since;
    static bool lastInPlace = false;
    static QString lastWhy;
    if (!since.isValid() || since.elapsed() > REUSE_MS) {
        lastWhy.clear();
        lastInPlace = readNow(lastWhy);
        since.start();
    }
    why = lastWhy;
    return lastInPlace;
}

#endif  // Q_OS_WIN
