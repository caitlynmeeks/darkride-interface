//
//  HifiAudioDeviceInfo.h
//  libraries/audio-client/src
//
//  Created by Amer Cerkic on 9/14/19.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once
#ifndef hifi_audiodeviceinfo_h
#define hifi_audiodeviceinfo_h


#include <QObject>
#include "AudioQtCompat.h"
#include <QString>

class HifiAudioDeviceInfo : public QObject {
    Q_OBJECT
    
public:
    enum DeviceType {
        desktop,
        hmd,
        both
    };

    HifiAudioDeviceInfo() : QObject() {}
    HifiAudioDeviceInfo(const HifiAudioDeviceInfo &deviceInfo) : QObject(){
        _audioDeviceInfo = deviceInfo.getDevice();
        _mode = deviceInfo.getMode();
        _isDefault = deviceInfo.isDefault();
        _deviceType = deviceInfo.getDeviceType();
        _debugName = hifi::audio::deviceName(deviceInfo.getDevice());
    }

    HifiAudioDeviceInfo(hifi::audio::Device deviceInfo, bool isDefault, QAudio::Mode mode, DeviceType devType=both) :
        _audioDeviceInfo(deviceInfo),
        _isDefault(isDefault),
        _mode(mode),
        _deviceType(devType),
        _debugName(hifi::audio::deviceName(deviceInfo)) {
    }
    
    void setMode(QAudio::Mode mode) { _mode = mode; }
    void setIsDefault() { _isDefault = true; }
    void setDevice(hifi::audio::Device devInfo);
    QString deviceName() const {
#if defined(Q_OS_ANDROID)
        return hifi::audio::deviceName(_audioDeviceInfo);
#endif
        if (_isDefault) {
            return DEFAULT_DEVICE_NAME;
        } else {
            return hifi::audio::deviceName(_audioDeviceInfo);
        }
    }
    hifi::audio::Device getDevice() const { return _audioDeviceInfo; }
    bool isDefault() const { return _isDefault; }
    QAudio::Mode getMode() const { return _mode; }
    DeviceType getDeviceType() const { return _deviceType; }
    HifiAudioDeviceInfo& operator=(const HifiAudioDeviceInfo& other);
    bool operator==(const HifiAudioDeviceInfo& rhs) const;
    bool operator!=(const HifiAudioDeviceInfo& rhs) const;

private:
    hifi::audio::Device _audioDeviceInfo;
    bool _isDefault { false };
    QAudio::Mode _mode { QAudio::AudioInput };
    DeviceType _deviceType{ both };
    QString _debugName;

public:
    static const QString DEFAULT_DEVICE_NAME;
};

#endif