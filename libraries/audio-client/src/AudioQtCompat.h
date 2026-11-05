//
//  AudioQtCompat.h
//  libraries/audio-client/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

// The audio device, input and output classes of Qt 5 and Qt 6 under one set of names, so the audio client builds on
// both. Qt 6 renamed QAudioDeviceInfo to QAudioDevice (lists from QMediaDevices), QAudioInput to QAudioSource and
// QAudioOutput to QAudioSink, replaced QAudioFormat's sample size, type, byte order and codec with one sample format,
// and dropped QAudio::Mode, which the engine's signals, settings and scripts keep using.

#ifndef hifi_AudioQtCompat_h
#define hifi_AudioQtCompat_h

#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtMultimedia/QAudio>
#include <QtMultimedia/QAudioFormat>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QtMultimedia/QAudioSource>
#include <QtMultimedia/QMediaDevices>

// Qt 5's QAudio::Mode, with Qt 5's values: Qt 6 has QAudioDevice::Mode instead
namespace QAudio {
enum Mode { AudioOutput, AudioInput };
}
Q_DECLARE_METATYPE(QAudio::Mode)

namespace hifi { namespace audio {

using Device = QAudioDevice;
using Source = QAudioSource;
using Sink = QAudioSink;

inline QString deviceName(const Device& device) {
    return device.description();
}

inline QList<Device> availableDevices(QAudio::Mode mode) {
    return mode == QAudio::AudioInput ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
}

inline Device defaultDevice(QAudio::Mode mode) {
    return mode == QAudio::AudioInput ? QMediaDevices::defaultAudioInput() : QMediaDevices::defaultAudioOutput();
}

inline bool supportsChannelCount(const Device& device, int channelCount) {
    return channelCount >= device.minimumChannelCount() && channelCount <= device.maximumChannelCount();
}

// signed 16-bit samples (Qt 6 always uses the machine's own byte order: little-endian on every platform we ship)
inline void setSignedInt16(QAudioFormat& format) {
    format.setSampleFormat(QAudioFormat::Int16);
}

inline int sampleBits(const QAudioFormat& format) {
    return format.bytesPerSample() * 8;
}

} }

#else

#include <QtMultimedia/QAudioDeviceInfo>
#include <QtMultimedia/QAudioInput>
#include <QtMultimedia/QAudioOutput>

namespace hifi { namespace audio {

using Device = QAudioDeviceInfo;
using Source = QAudioInput;
using Sink = QAudioOutput;

inline QString deviceName(const Device& device) {
    return device.deviceName();
}

inline QList<Device> availableDevices(QAudio::Mode mode) {
    return QAudioDeviceInfo::availableDevices(mode);
}

inline Device defaultDevice(QAudio::Mode mode) {
    return mode == QAudio::AudioInput ? QAudioDeviceInfo::defaultInputDevice() : QAudioDeviceInfo::defaultOutputDevice();
}

inline bool supportsChannelCount(const Device& device, int channelCount) {
    return device.supportedChannelCounts().contains(channelCount);
}

// signed 16-bit little-endian PCM
inline void setSignedInt16(QAudioFormat& format) {
    format.setCodec("audio/pcm");
    format.setSampleSize(16);
    format.setSampleType(QAudioFormat::SignedInt);
    format.setByteOrder(QAudioFormat::LittleEndian);
}

inline int sampleBits(const QAudioFormat& format) {
    return format.sampleSize();
}

} }

#endif

#endif // hifi_AudioQtCompat_h
