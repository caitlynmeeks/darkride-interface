//
//  ImageTests.cpp
//  tests/image/src
//
//  Created by the Darkride rendering session on 2026-11-04.
//  Copyright 2026 Tivoli Cloud VR, Inc.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE or https://opensource.org/licenses/MIT
//

#include "ImageTests.h"

#include <QtTest/QtTest>
#include <QtCore/QBuffer>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageIOHandler>

#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>

#include <image/Image.h>
#include <image/OpenEXRReader.h>
#include <image/TextureProcessing.h>

QTEST_GUILESS_MAIN(ImageTests)

static const int EXR_SIDE = 8;

// a real 8 x 8 EXR, written by the library itself, as bytes
static QByteArray smallExr() {
    QTemporaryDir dir;
    QString path = dir.filePath("small.exr");
    {
        Imf::Array2D<Imf::Rgba> pixels(EXR_SIDE, EXR_SIDE);
        for (int y = 0; y < EXR_SIDE; y++) {
            for (int x = 0; x < EXR_SIDE; x++) {
                pixels[y][x] = Imf::Rgba(0.5f, 0.25f, 1.0f, 1.0f);
            }
        }
        Imf::RgbaOutputFile file(path.toStdString().c_str(), EXR_SIDE, EXR_SIDE, Imf::WRITE_RGBA);
        file.setFrameBuffer(&pixels[0][0], 1, EXR_SIDE);
        file.writePixels(EXR_SIDE);
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

static image::Image readExr(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return image::readOpenEXR(buffer, "test.exr");
}

void ImageTests::testExrReadsWhole() {
    QByteArray bytes = smallExr();
    QVERIFY(bytes.size() > 0);
    image::Image image = readExr(bytes);
    QVERIFY(!image.isNull());
    QCOMPARE(image.getWidth(), EXR_SIDE);
    QCOMPARE(image.getHeight(), EXR_SIDE);
}

void ImageTests::testExrTruncated() {
    QByteArray bytes = smallExr();
    QVERIFY(bytes.size() > 0);
    // cut at every tenth from the pixels' end back to the header: none may throw past the reader
    for (int tenths = 9; tenths >= 1; tenths--) {
        image::Image image = readExr(bytes.left(bytes.size() * tenths / 10));
        QVERIFY2(image.isNull(), qPrintable(QString("a file cut at %1/10 read as a picture").arg(tenths)));
    }
}

void ImageTests::testExrHugeWindow() {
    QByteArray bytes = smallExr();
    QVERIFY(bytes.size() > 0);
    // the dataWindow attribute: its name, its type, a 4-byte size, then xmin, ymin, xmax, ymax as 32-bit ints
    int at = bytes.indexOf(QByteArray("dataWindow\0box2i\0", 17));
    QVERIFY(at >= 0);
    int values = at + 17 + 4;
    int32_t huge = 1 << 30;
    memcpy(bytes.data() + values + 8, &huge, sizeof(huge));    // xmax
    memcpy(bytes.data() + values + 12, &huge, sizeof(huge));   // ymax
    // refused by its size before any pixel is held
    image::Image image = readExr(bytes);
    QVERIFY(image.isNull());
}

static uint32_t crc32(const QByteArray& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (char c : bytes) {
        crc ^= (uint8_t)c;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static image::Image readPicture(const QByteArray& bytes, const std::string& name) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return image::processRawImageData(buffer, name);
}

void ImageTests::testPictureHeaderSize() {
    // a real 4 x 4 PNG
    QImage small(4, 4, QImage::Format_RGBA8888);
    small.fill(Qt::red);
    QByteArray bytes;
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(small.save(&buffer, "png"));
    }
    image::Image read = readPicture(bytes, "small.png");
    QVERIFY(!read.isNull());
    QCOMPARE(read.getWidth(), 4);

    // the same bytes with a header claiming 60000 x 60000 (the IHDR chunk: 8 bytes of signature, 4 of length, 4 of
    // type, then width and height big-endian, then the chunk's CRC over its type and data)
    QByteArray lying = bytes;
    QVERIFY(lying.mid(12, 4) == QByteArray("IHDR"));
    uint32_t side = 60000;
    for (int i = 0; i < 4; i++) {
        lying[16 + i] = (char)((side >> (24 - 8 * i)) & 0xFF);
        lying[20 + i] = (char)((side >> (24 - 8 * i)) & 0xFF);
    }
    uint32_t crc = crc32(lying.mid(12, 4 + 13));
    for (int i = 0; i < 4; i++) {
        lying[29 + i] = (char)((crc >> (24 - 8 * i)) & 0xFF);
    }
    image::Image refused = readPicture(lying, "lying.png");
    QVERIFY(refused.isNull());
}

// Every picture format this Qt reads must report its size from the header before decoding, or the size check above
// would refuse it whole; the list is printed for the record
void ImageTests::testFormatsReportSize() {
    QStringList without;
    QStringList with;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        QBuffer empty;
        empty.open(QIODevice::ReadOnly);
        QImageReader reader(&empty, format);
        (reader.supportsOption(QImageIOHandler::Size) ? with : without) << QString::fromLatin1(format);
    }
    qInfo() << "formats reporting a size:" << with.join(" ");
    qInfo() << "formats NOT reporting a size:" << (without.isEmpty() ? "none" : without.join(" "));
    for (const char* common : { "png", "jpg", "jpeg", "bmp", "gif", "tga", "ico", "webp", "tif", "tiff", "svg" }) {
        if (QImageReader::supportedImageFormats().contains(QByteArray(common))) {
            QVERIFY2(with.contains(QString(common)), common);
        }
    }
}
