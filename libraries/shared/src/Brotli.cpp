#include "Brotli.h"

#include <algorithm>

// #include "brotli/encode.h"
#include "brotli/decode.h"

// bool brotliCompress(QByteArray source, QByteArray &destination, int quality) {
// }

bool brotliDecompress(const QByteArray& source, QByteArray& destination, int maxSize) {
    destination.clear();
    if (maxSize <= 0) {
        return false;
    }
    // sizes are worked out in 64 bits, so growing the buffer can never wrap around
    const qint64 limit = std::min<qint64>(maxSize, BROTLI_MAX_SIZE_CEILING);

    auto state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        return false;
    }
    // standard brotli only (RFC 7932, what HTTP "br" uses): its window is at most 16 MiB. The large-window extension
    // stays off, so a stream can't make the decoder itself set aside up to 1 GiB.

    qint64 capacity = std::min<qint64>(4096, limit);
    destination.resize(static_cast<int>(capacity));

    size_t availableIn = source.size();
    const uint8_t* nextIn = reinterpret_cast<const uint8_t*>(source.constData());

    size_t availableOut = static_cast<size_t>(capacity);
    uint8_t* nextOut = reinterpret_cast<uint8_t*>(destination.data());

    bool success = false;
    for (;;) {
        auto result = BrotliDecoderDecompressStream(state, &availableIn, &nextIn, &availableOut, &nextOut, nullptr);
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            if (capacity >= limit) {
                break;   // the content is larger than the limit: refused
            }
            const qint64 used = capacity - static_cast<qint64>(availableOut);
            const qint64 newCapacity = std::min<qint64>(capacity * 2, limit);
            destination.resize(static_cast<int>(newCapacity));
            nextOut = reinterpret_cast<uint8_t*>(destination.data()) + used;
            availableOut = static_cast<size_t>(newCapacity - used);
            capacity = newCapacity;
        } else if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            destination.resize(static_cast<int>(capacity - static_cast<qint64>(availableOut)));
            success = true;
            break;
        } else {
            break;   // a broken stream, or one that ends early
        }
    }

    BrotliDecoderDestroyInstance(state);
    if (!success) {
        destination.clear();
    }
    return success;
}
