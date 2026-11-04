//
//  OpenEXRReader.cpp
//  image/src/image
//
//  Created by Olivier Prat
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OpenEXRReader.h"

#include "TextureProcessing.h"
#include "ImageLogging.h"

#include <QIODevice>
#include <QDebug>

#if !defined(Q_OS_ANDROID)

#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfTestFile.h>
#include <OpenEXR/IexBaseExc.h>

class QIODeviceImfStream : public Imf::IStream {
public:

    QIODeviceImfStream(QIODevice& device, const std::string& filename) :
        Imf::IStream(filename.c_str()), _device(device) {
    }

    bool read(char c[/*n*/], int n) override {
        // a short read is an error, as the library expects (it would otherwise go on with bytes never read)
        if (_device.read(c, n) != n) {
            throw Iex::InputExc(std::string("OpenEXR - short read in ") + fileName() + ": " + _device.errorString().toStdString());
        }
        return !_device.atEnd();
    }

    Imf::Int64 tellg() override {
        return _device.pos();
    }

    void seekg(Imf::Int64 pos) override {
        _device.seek(pos);
    }

    void clear() override {
        // Not much to do
    }

private:

    QIODevice&  _device;
};

#endif

// The largest EXR read at all: an 8192 x 4096 equirectangular sky, twice the largest lightmap the exporter writes
// (4096 x 4096); the pixels are held twice while read (the library's half floats, then our floats), and the
// window's size is a 32-bit field of the file, so the cap comes before any allocation
static const int64_t MAX_EXR_DIMENSION { 8192 };
static const int64_t MAX_EXR_PIXELS { int64_t(8192) * 4096 };

image::Image image::readOpenEXR(QIODevice& content, const std::string& filename) {
#if !defined(Q_OS_ANDROID)
    // the library reports a bad or truncated file by throwing, on whichever thread reads it
    try {
        QIODeviceImfStream device(content, filename);
        if (!Imf::isOpenExrFile(device)) {
            qWarning(imagelogging) << "OpenEXR - File " << filename.c_str() << " doesn't have the proper format";
            return QImage();
        }
        Imf::RgbaInputFile file(device);
        Imath::Box2i viewport = file.dataWindow();
        int64_t width = int64_t(viewport.max.x) - viewport.min.x + 1;
        int64_t height = int64_t(viewport.max.y) - viewport.min.y + 1;
        if (width <= 0 || height <= 0 || width > MAX_EXR_DIMENSION || height > MAX_EXR_DIMENSION || width * height > MAX_EXR_PIXELS) {
            qWarning(imagelogging) << "OpenEXR - File " << filename.c_str() << " is " << width << "x" << height << ", past the limit";
            return QImage();
        }
        Imf::Array2D<Imf::Rgba> pixels;
        pixels.resizeErase((long)height, (long)width);
        file.setFrameBuffer(&pixels[0][0] - viewport.min.x - int64_t(viewport.min.y) * width, 1, (size_t)width);
        file.readPixels(viewport.min.y, viewport.max.y);

        // the pixels as floats, every channel kept: a lightmap stays at half precision from here to the GPU
        // (LIGHTMAPS.md); the cubemap path packs them to R11G11B10 itself, as this reader used to
        Image image{ (int)width, (int)height, Image::Format_RGBAF };
        for (int y = 0; y < (int)height; y++) {
            const auto srcScanline = pixels[y];
            glm::vec4* dstScanline = (glm::vec4*) image.editScanLine(y);
            for (int x = 0; x < (int)width; x++) {
                const auto& srcPixel = srcScanline[x];
                dstScanline[x] = glm::vec4((float)srcPixel.r, (float)srcPixel.g, (float)srcPixel.b, (float)srcPixel.a);
            }
        }
        return image;
    } catch (const std::exception& e) {
        qWarning(imagelogging) << "OpenEXR - File " << filename.c_str() << " could not be read: " << e.what();
    } catch (...) {
        qWarning(imagelogging) << "OpenEXR - File " << filename.c_str() << " could not be read";
    }
#endif
    return QImage();
}
