#ifndef tivoli_Brotli_h
#define tivoli_Brotli_h

#include <QtCore/QByteArray>

// Quality is between 0 to 11
// 0 gives no compression at all
// 1 gives best speed
// 11 gives best compression
// -1 is default, currently ?

// bool brotliCompress(QByteArray source, QByteArray& destination, int quality = -1);

// The most one decompression may produce unless the caller says otherwise. A caller that knows how big its real
// content gets passes its own limit.
static const int BROTLI_DEFAULT_MAX_SIZE = 256 * 1024 * 1024;

// No limit can go above this: a QByteArray must be able to hold the result.
static const int BROTLI_MAX_SIZE_CEILING = 1024 * 1024 * 1024;

// Decompresses a whole brotli stream into destination. Returns false, with destination empty, if the stream is
// broken or incomplete, or if its content would be larger than maxSize bytes (capped at BROTLI_MAX_SIZE_CEILING).
bool brotliDecompress(const QByteArray& source, QByteArray& destination, int maxSize = BROTLI_DEFAULT_MAX_SIZE);

#endif // tivoli_Brotli_h
