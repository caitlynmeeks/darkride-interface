//
//  AudioWireFormatTests.cpp
//  tests/audio/src
//
//  Created by social on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  The recorded bytes (GOLDEN_*) came from a Qt 5.15 build of this code. To record them again on a Qt 5 build (never on
//  Qt 6: they are what Qt 6 must match), run the test with WIRE_FORMAT_PRINT=1 and paste the printed lines in.
//

#include "AudioWireFormatTests.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVector>
#include <QtMultimedia/QAudioFormat>

#include <AudioConstants.h>
#include <AudioFileWav.h>
#include <AudioHelpers.h>
#include <AudioInjector.h>
#include <InjectedAudioStream.h>
#include <ReceivedMessage.h>
#include <udt/PacketHeaders.h>

QTEST_MAIN(AudioWireFormatTests)

namespace {

const QUuid STREAM_ID("{5c1e9a40-7b2d-4f3e-8a61-0d9c4b7e2f15}");
const glm::vec3 POSITION(1.5f, -2.25f, 3.0f);
const glm::quat ORIENTATION(0.5f, 0.5f, -0.5f, 0.5f);  // w, x, y, z
const uchar LOOPBACK = 1;

// recorded from the Qt 5.15 build (see the top of the file)
// an injector's header: the position, orientation and box go as raw little-endian floats, the radius as an 8-byte float
const char* GOLDEN_INJECT_AUDIO_HEADER =
    "0000000000005c1e9a407b2d4f3e8a610d9c4b7e2f1500010000c03f000010c0000040400000003f000000bf0000003f0000003f0000c03f"
    "000010c0000040400000000000000000000000000000000000000000c301";
// a 48 kHz 16-bit stereo recording of two frames: the standard 44-byte WAV header, then the samples as given
const char* GOLDEN_WAV_FILE =
    "524946462c00000057415645666d7420100000000100020080bb000000ee02000400100064617461080000000102030405060708";

// compares with the recorded bytes, or prints them when recording
void checkGolden(const char* name, const QByteArray& bytes, const char* golden) {
    if (qEnvironmentVariableIsSet("WIRE_FORMAT_PRINT")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QFAIL("record on a Qt 5 build only: the recorded bytes are what Qt 6 must match");
#endif
        printf("const char* GOLDEN_%s = \"%s\";\n", name, bytes.toHex().constData());
        return;
    }
    QVERIFY2(QByteArray(golden).size() > 0, qPrintable(QString("no recorded bytes for %1").arg(name)));
    QCOMPARE(bytes.toHex(), QByteArray(golden));
}

AudioInjectorOptions injectorOptions() {
    AudioInjectorOptions options;
    options.position = POSITION;
    options.orientation = ORIENTATION;
    options.stereo = false;
    options.ignorePenumbra = true;
    return options;
}

QByteArray writtenHeader(AudioInjector::InjectAudioHeader& header) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    header = AudioInjector::writeInjectAudioHeader(&buffer, STREAM_ID, injectorOptions(), LOOPBACK);
    return bytes;
}

}

// What an injector sends ahead of its audio (U4 in the Qt 6 move's list).
void AudioWireFormatTests::injectAudioHeader() {
    AudioInjector::InjectAudioHeader header;
    const QByteArray bytes = writtenHeader(header);
    checkGolden("INJECT_AUDIO_HEADER", bytes, GOLDEN_INJECT_AUDIO_HEADER);

    // sequence number 2, codec name 4 (empty), stream id 16, stereo 1, then the loopback flag
    QCOMPARE(header.loopbackOffset, 23);
    QCOMPARE(header.positionOffset, 24);
    // position 12, orientation 16, box corner 12, box scale 12, radius 8, then the volume
    QCOMPARE(header.volumeOffset, 84);
    // volume 1, ignore penumbra 1, then the audio
    QCOMPARE(header.audioOffset, 86);
    QCOMPARE(bytes.size(), 86);
}

// The audio mixer reads the recorded header and one mono frame after it (U5 and U6): every field comes back, and the
// audio starts exactly where the header ends.
void AudioWireFormatTests::theMixerReadsAnInjector() {
    QByteArray packet = QByteArray::fromHex(GOLDEN_INJECT_AUDIO_HEADER);
    QVERIFY(packet.size() > 0);
    QVector<int16_t> samples(AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);
    for (int i = 0; i < samples.size(); i++) {
        samples[i] = int16_t(1000 + i);
    }
    packet.append(reinterpret_cast<const char*>(samples.constData()), samples.size() * (int)sizeof(int16_t));

    ReceivedMessage message(packet, PacketType::InjectAudio, versionForPacketType(PacketType::InjectAudio),
                            HifiSockAddr());
    InjectedAudioStream stream(STREAM_ID, false);
    stream.parseData(message);

    QVERIFY(stream.getPosition() == POSITION);
    QVERIFY(stream.getOrientation() == ORIENTATION);
    QVERIFY(stream.getAvatarBoundingBoxCorner() == POSITION);
    QVERIFY(stream.getAvatarBoundingBoxScale() == glm::vec3(0.0f));
    QVERIFY(stream.shouldLoopbackForNode());
    QVERIFY(!stream.isStereo());
    QCOMPARE(stream.getRadius(), 0.0f);
    QCOMPARE(stream.getAttenuationRatio(), unpackFloatGainFromByte(packFloatGainToByte(1.0f)));

    QCOMPARE(stream.getSamplesAvailable(), samples.size());
    QCOMPARE(stream.popSamples(samples.size(), true), samples.size());
    AudioRingBuffer::ConstIterator popped = stream.getLastPopOutput();
    for (int i = 0; i < samples.size(); i++, ++popped) {
        QCOMPARE(*popped, samples[i]);
    }
}

// A recording's file (SO1 to SO3): raw little-endian, so only the bytes are checked.
void AudioWireFormatTests::wavFile() {
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    format.setSampleFormat(QAudioFormat::Int16);
#else
    format.setSampleSize(16);
    format.setSampleType(QAudioFormat::SignedInt);
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setCodec("audio/pcm");
#endif

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("recording.wav");
    AudioFileWav wav;
    QVERIFY(wav.create(format, path));
    char frames[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    QVERIFY(wav.addRawAudioChunk(frames, sizeof(frames)));
    wav.close();

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    checkGolden("WAV_FILE", bytes, GOLDEN_WAV_FILE);
    QCOMPARE(bytes.size(), 44 + (int)sizeof(frames));
}
