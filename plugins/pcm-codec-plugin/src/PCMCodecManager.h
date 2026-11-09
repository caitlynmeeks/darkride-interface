//
//  PCMCodecManager.h
//  plugins/pcmCodec/src
//
//  Created by Brad Hefta-Gaub on 6/9/2016
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi__PCMCodecManager_h
#define hifi__PCMCodecManager_h

#include <atomic>

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QtEndian>

#include <plugins/CodecPlugin.h>

class PCMCodec : public CodecPlugin, public Encoder, public Decoder {
    Q_OBJECT

public:
    // Plugin functions
    bool isSupported() const override;
    const QString getName() const override { return NAME; }

    void init() override;
    void deinit() override;

    /// Called when a plugin is being activated for use.  May be called multiple times.
    bool activate() override;
    /// Called when a plugin is no longer being used.  May be called multiple times.
    void deactivate() override;

    virtual Encoder* createEncoder(int sampleRate, int numChannels) override;
    virtual Decoder* createDecoder(int sampleRate, int numChannels) override;
    virtual void releaseEncoder(Encoder* encoder) override;
    virtual void releaseDecoder(Decoder* decoder) override;

    virtual void encode(const QByteArray& decodedBuffer, QByteArray& encodedBuffer) override {
        encodedBuffer = decodedBuffer;
    }

    virtual void decode(const QByteArray& encodedBuffer, QByteArray& decodedBuffer) override {
        decodedBuffer = encodedBuffer;
    }

    virtual void lostFrame(QByteArray& decodedBuffer) override {
        memset(decodedBuffer.data(), 0, decodedBuffer.size());
    }

private:
    static const char* NAME;
};

class zLibCodec : public CodecPlugin, public Encoder, public Decoder {
    Q_OBJECT

public:
    // Plugin functions
    bool isSupported() const override;
    const QString getName() const override { return NAME; }

    void init() override;
    void deinit() override;

    /// Called when a plugin is being activated for use.  May be called multiple times.
    bool activate() override;
    /// Called when a plugin is no longer being used.  May be called multiple times.
    void deactivate() override;

    virtual Encoder* createEncoder(int sampleRate, int numChannels) override;
    virtual Decoder* createDecoder(int sampleRate, int numChannels) override;
    virtual void releaseEncoder(Encoder* encoder) override;
    virtual void releaseDecoder(Decoder* decoder) override;

    virtual void encode(const QByteArray& decodedBuffer, QByteArray& encodedBuffer) override {
        encodedBuffer = qCompress(decodedBuffer);
    }

    virtual void decode(const QByteArray& encodedBuffer, QByteArray& decodedBuffer) override {
        // qUncompress allocates the size the data's first 4 bytes (big-endian) say before it starts, so anything
        // that says more than a few frames isn't decoded
        if (encodedBuffer.size() < 4 ||
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(encodedBuffer.constData())) > MAX_DECODED_BYTES) {
            countRefusal();
            decodedBuffer.clear();
            return;
        }
        decodedBuffer = qUncompress(encodedBuffer);
    }

    virtual void lostFrame(QByteArray& decodedBuffer) override {
        memset(decodedBuffer.data(), 0, decodedBuffer.size());
    }

private:
    static const char* NAME;

    // a frame is at most 960 bytes (240 stereo samples)
    static constexpr quint32 MAX_DECODED_BYTES = 4 * 960;

    // one log line at most every 10 s, with how many frames weren't decoded (decoders run on several threads)
    static void countRefusal() {
        static std::atomic<int> refused { 0 };
        static std::atomic<qint64> lastLog { 0 };
        const qint64 LOG_INTERVAL_MSECS = 10 * 1000;
        ++refused;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 last = lastLog.load();
        if ((last == 0 || now - last >= LOG_INTERVAL_MSECS) && lastLog.compare_exchange_strong(last, now)) {
            qWarning() << "zlib codec: refused" << refused.exchange(0) << "frames too short or declaring more than"
                     << MAX_DECODED_BYTES << "bytes";
        }
    }
};

#endif // hifi__PCMCodecManager_h
