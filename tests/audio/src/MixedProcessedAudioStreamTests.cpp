//
//  MixedProcessedAudioStreamTests.cpp
//
//  Created by social on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MixedProcessedAudioStreamTests.h"

#include <AudioConstants.h>
#include <MixedProcessedAudioStream.h>
#include <plugins/CodecPlugin.h>
#include <udt/PacketHeaders.h>

QTEST_MAIN(MixedProcessedAudioStreamTests)

namespace {

const int FRAME_BYTES = AudioConstants::NETWORK_FRAME_BYTES_STEREO;

// decoders that hand on what the codec plugins' decoders do: pcm passes the payload on and leaves a lost frame as
// it was (empty), zlib passes on whatever the payload uncompresses to, Opus always gives one whole frame
enum class Codec { None, Pcm, Zlib, Opus };

class TestDecoder : public Decoder {
public:
    TestDecoder(Codec codec) : _codec(codec) {}

    void decode(const QByteArray& encodedBuffer, QByteArray& decodedBuffer) override {
        switch (_codec) {
            case Codec::Zlib:
                decodedBuffer = qUncompress(encodedBuffer);
                break;
            case Codec::Opus:
                decodedBuffer.resize(FRAME_BYTES);
                memcpy(decodedBuffer.data(), encodedBuffer.constData(), std::min(FRAME_BYTES, encodedBuffer.size()));
                break;
            default:
                decodedBuffer = encodedBuffer;
                break;
        }
    }

    void lostFrame(QByteArray& decodedBuffer) override {
        if (_codec == Codec::Opus) {
            decodedBuffer.resize(FRAME_BYTES);
        }
        memset(decodedBuffer.data(), 0, decodedBuffer.size());
    }

private:
    Codec _codec;
};

class TestCodec : public CodecPlugin {
public:
    TestCodec(Codec codec) : _codec(codec) {}

    const QString getName() const override { return "test"; }
    Encoder* createEncoder(int sampleRate, int numChannels) override { return nullptr; }
    Decoder* createDecoder(int sampleRate, int numChannels) override { return new TestDecoder(_codec); }
    void releaseEncoder(Encoder* encoder) override {}
    void releaseDecoder(Decoder* decoder) override { delete static_cast<TestDecoder*>(decoder); }

private:
    Codec _codec;
};

class TestStream : public MixedProcessedAudioStream {
public:
    TestStream(Codec codec) : MixedProcessedAudioStream(100) {
        outputFormatChanged(AudioConstants::SAMPLE_RATE, AudioConstants::STEREO);
        if (codec != Codec::None) {
            setupCodec(std::make_shared<TestCodec>(codec), "test", AudioConstants::STEREO);
        }
    }

    using MixedProcessedAudioStream::parseAudioData;
    using MixedProcessedAudioStream::lostAudioData;
};

// what the stream handed on, as AudioClient::processReceivedSamples and the recorder would get it
struct Received {
    QList<int> addedSizes;
    QList<QByteArray> processed;

    void listen(TestStream& stream) {
        QObject::connect(&stream, &MixedProcessedAudioStream::addedStereoSamples, [this](const QByteArray& samples) {
            addedSizes << samples.size();
        });
        QObject::connect(&stream, &MixedProcessedAudioStream::processSamples,
                         [this](const QByteArray& inputBuffer, QByteArray& outputBuffer) {
            processed << inputBuffer;
            outputBuffer.fill(0, FRAME_BYTES);
        });
    }
};

QByteArray loudFrame(int bytes) {
    QByteArray frame(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; i++) {
        frame[i] = (char)(1 + i % 251);
    }
    return frame;
}

}

void MixedProcessedAudioStreamTests::receivedFrames_data() {
    QTest::addColumn<int>("codec");
    QTest::addColumn<int>("decodedBytes");
    QTest::addColumn<bool>("garbage");

    const QList<QPair<Codec, const char*>> codecs {
        { Codec::None, "none" }, { Codec::Pcm, "pcm" }, { Codec::Zlib, "zlib" }, { Codec::Opus, "opus" }
    };
    const QList<int> sizes { 0, 1, 2, 480, FRAME_BYTES - 2, FRAME_BYTES, FRAME_BYTES + 2, 4096, 1 << 20 };
    for (auto& codec : codecs) {
        for (int size : sizes) {
            QTest::newRow(qPrintable(QString("%1, %2 bytes").arg(codec.second).arg(size)))
                << (int)codec.first << size << false;
        }
    }
    QTest::newRow("zlib, not zlib data") << (int)Codec::Zlib << FRAME_BYTES << true;
}

// every frame the stream hands on is one whole stereo network frame: the mixer's own when it is one, else silence
void MixedProcessedAudioStreamTests::receivedFrames() {
    QFETCH(int, codec);
    QFETCH(int, decodedBytes);
    QFETCH(bool, garbage);

    TestStream stream((Codec)codec);
    Received received;
    received.listen(stream);

    QByteArray decoded = loudFrame(decodedBytes);
    QByteArray payload = (Codec)codec == Codec::Zlib && !garbage ? qCompress(decoded) : decoded;
    stream.parseAudioData(PacketType::MixedAudio, payload);

    QCOMPARE(received.addedSizes, QList<int>({ FRAME_BYTES }));
    QCOMPARE(received.processed.size(), 1);
    const QByteArray& handedOn = received.processed.first();
    QCOMPARE(handedOn.size(), FRAME_BYTES);

    bool wholeFrame = decodedBytes == FRAME_BYTES || (Codec)codec == Codec::Opus;
    if (garbage) {
        QCOMPARE(handedOn, QByteArray(FRAME_BYTES, 0));
    } else if (wholeFrame) {
        QCOMPARE(handedOn, loudFrame(FRAME_BYTES).left(std::min(FRAME_BYTES, decodedBytes)) +
                 QByteArray(std::max(0, FRAME_BYTES - decodedBytes), 0));
    } else {
        QCOMPARE(handedOn, QByteArray(FRAME_BYTES, 0));
    }
}

void MixedProcessedAudioStreamTests::lostFrames_data() {
    QTest::addColumn<int>("codec");
    QTest::newRow("none") << (int)Codec::None;
    QTest::newRow("pcm") << (int)Codec::Pcm;
    QTest::newRow("zlib") << (int)Codec::Zlib;
    QTest::newRow("opus") << (int)Codec::Opus;
}

// a lost frame is one whole silent frame under every codec (pcm's and zlib's decoders leave it empty)
void MixedProcessedAudioStreamTests::lostFrames() {
    QFETCH(int, codec);

    TestStream stream((Codec)codec);
    Received received;
    received.listen(stream);

    stream.lostAudioData(3);

    QCOMPARE(received.addedSizes, QList<int>({ FRAME_BYTES, FRAME_BYTES, FRAME_BYTES }));
    QCOMPARE(received.processed.size(), 3);
    for (const QByteArray& handedOn : received.processed) {
        QCOMPARE(handedOn, QByteArray(FRAME_BYTES, 0));
    }
}
