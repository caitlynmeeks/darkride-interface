//
//  Created by Bradley Austin Davis on 2015/07/08
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once
#ifndef hifi_BufferParser_h
#define hifi_BufferParser_h

#include <cstdint>

#include <QUuid>
#include <QtEndian>

#include "GLMHelpers.h"
#include "ByteCountCoding.h"
#include "PropertyFlags.h"

class BufferParser {
public:
    BufferParser(const uint8_t* data, size_t size, size_t offset = 0) :
        _offset(offset), _data(data), _size(size) {
    }

    // A read that would pass the end of the buffer reads nothing (the value is left zeroed), moves to the end and
    // marks the parser overrun; a reader checks overrun() before trusting what it read.
    template<typename T>
    inline void readValue(T& result) {
        if (remaining() < sizeof(T)) {
            fail(&result, sizeof(T));
            return;
        }
        memcpy(&result, _data + _offset, sizeof(T));
        _offset += sizeof(T);
    }

    inline bool overrun() const {
        return _overrun;
    }

    inline void readUuid(QUuid& result) {
        readValue(result.data1);
        readValue(result.data2);
        readValue(result.data3);
        readValue(result.data4);
        result.data1 = qFromBigEndian<quint32>(result.data1);
        result.data2 = qFromBigEndian<quint16>(result.data2);
        result.data3 = qFromBigEndian<quint16>(result.data3);
    }

    template <typename T>
    inline void readFlags(PropertyFlags<T>& result) {
        _offset += result.decode(_data + _offset, remaining());
    }

    template<typename T>
    inline void readCompressedCount(T& result) {
        // FIXME switch to a heapless implementation as soon as Brad provides it.
        ByteCountCoded<T> codec;
        _offset += codec.decode(reinterpret_cast<const char*>(_data + _offset), (int)remaining());
        result = codec.data;
    }

    inline size_t remaining() const {
        return _offset < _size ? _size - _offset : 0;
    }

    inline void fail(void* result, size_t size) {
        memset(result, 0, size);
        _offset = _size;
        _overrun = true;
    }

    inline size_t offset() const {
        return _offset;
    }

    inline const uint8_t* data() const {
        return _data;
    }

private:

    size_t _offset{ 0 };
    const uint8_t* const _data;
    const size_t _size;
    bool _overrun{ false };
};


template<>
inline void BufferParser::readValue<quat>(quat& result) {
    if (remaining() < 4 * sizeof(uint16_t)) {
        result = quat();
        _offset = _size;
        _overrun = true;
        return;
    }
    size_t advance = unpackOrientationQuatFromBytes(_data + _offset, result);
    _offset += advance;
}

template<>
inline void BufferParser::readValue(QString& result) {
    uint16_t length; readValue(length);
    if (remaining() < length) {
        result = QString();
        _offset = _size;
        _overrun = true;
        return;
    }
    result = QString::fromUtf8((const char*)_data + _offset, (int)length);
    _offset += length;
}

template<>
inline void BufferParser::readValue(QUuid& result) {
    uint16_t length; readValue(length);
    if (length != 16) {
        result = QUuid();
        _offset = _size;
        _overrun = true;
        return;
    }
    readUuid(result);
}

template<>
inline void BufferParser::readValue(QVector<glm::vec3>& result) {
    uint16_t length; readValue(length);
    if (remaining() < sizeof(glm::vec3) * length) {
        result.clear();
        _offset = _size;
        _overrun = true;
        return;
    }
    result.resize(length);
    memcpy(result.data(), _data + _offset, sizeof(glm::vec3) * length);
    _offset += sizeof(glm::vec3) * length;
}

template<>
inline void BufferParser::readValue(QByteArray& result) {
    uint16_t length; readValue(length);
    if (remaining() < length) {
        result.clear();
        _offset = _size;
        _overrun = true;
        return;
    }
    result = QByteArray((char*)_data + _offset, (int)length);
    _offset += length;
}

#endif
