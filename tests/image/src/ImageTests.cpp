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
#include <image/TGAReader.h>

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

// An 8-bit black-and-white TGA (type 3 uncompressed, type 11 run-length), as a modelling tool writes specular maps:
// read by our own reader from its header, so the size check never needs Qt's plugin for it
static QByteArray tgaHeader(uint8_t type, uint16_t width, uint16_t height) {
    QByteArray header(18, '\0');
    header[2] = (char)type;
    header[12] = (char)(width & 0xFF); header[13] = (char)(width >> 8);
    header[14] = (char)(height & 0xFF); header[15] = (char)(height >> 8);
    header[16] = (char)8;        // bits per pixel
    header[17] = (char)0x20;     // top-left origin, no alpha bits
    return header;
}

void ImageTests::testTgaBlackWhite() {
    const int side = 4;
    QByteArray plain = tgaHeader(3, side, side);
    for (int i = 0; i < side * side; i++) {
        plain.append((char)(i * 16));
    }
    {
        QBuffer buffer;
        buffer.setData(plain);
        buffer.open(QIODevice::ReadOnly);
        image::Image image = image::readTGA(buffer);
        QVERIFY(!image.isNull());
        QCOMPARE(image.getWidth(), side);
        QCOMPARE(image.getHeight(), side);
        QCOMPARE((int)image.getFormat(), (int)image::Image::Format_Grayscale8);
        QCOMPARE((int)image.getScanLine(1)[2], 6 * 16);
    }
    // the same picture through the picture path, by its extension
    image::Image viaPath = readPicture(plain, "specular.tga");
    QVERIFY(!viaPath.isNull());
    QCOMPARE(viaPath.getWidth(), side);

    // the same picture whose descriptor calls its channel an alpha channel too (8 attribute bits): read as grey
    QByteArray alphaBits = plain;
    alphaBits[17] = (char)0x28;
    {
        QBuffer b; b.setData(alphaBits); b.open(QIODevice::ReadOnly);
        image::Image image = image::readTGA(b);
        QVERIFY(!image.isNull());
        QCOMPARE((int)image.getScanLine(1)[2], 6 * 16);
    }

    // run-length: each row one run of four pixels of one value
    QByteArray rle = tgaHeader(11, side, side);
    for (int y = 0; y < side; y++) {
        rle.append((char)(0x80 | (side - 1)));
        rle.append((char)(y * 50));
    }
    QBuffer buffer;
    buffer.setData(rle);
    buffer.open(QIODevice::ReadOnly);
    image::Image image = image::readTGA(buffer);
    QVERIFY(!image.isNull());
    QCOMPARE((int)image.getScanLine(3)[0], 150);
    QCOMPARE((int)image.getScanLine(3)[3], 150);

    // refused, never read past the end or written past the row: a run longer than the row; a file shorter than its
    // claimed pixels; a run-length file that ends inside a run; an ID field past the end
    QByteArray overrun = tgaHeader(11, side, side);
    overrun.append((char)(0x80 | 7));   // a run of 8 in a row of 4
    overrun.append((char)200);
    {
        QBuffer b; b.setData(overrun); b.open(QIODevice::ReadOnly);
        QVERIFY(image::readTGA(b).isNull());
    }
    QByteArray shortFile = plain.left(18 + 10);
    {
        QBuffer b; b.setData(shortFile); b.open(QIODevice::ReadOnly);
        QVERIFY(image::readTGA(b).isNull());
    }
    QByteArray cutRun = rle.left(18 + 5);
    {
        QBuffer b; b.setData(cutRun); b.open(QIODevice::ReadOnly);
        QVERIFY(image::readTGA(b).isNull());
    }
    QByteArray longId = plain;
    longId[0] = (char)200;   // an ID field longer than the file
    {
        QBuffer b; b.setData(longId); b.open(QIODevice::ReadOnly);
        QVERIFY(image::readTGA(b).isNull());
    }
}
