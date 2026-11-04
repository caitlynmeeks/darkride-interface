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

#include <OpenEXR/OpenEXRConfig.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfTestFile.h>

// OpenEXR 3 sizes its streams in uint64_t; 2.x in its own Int64 (unsigned long here, not the same type)
#if OPENEXR_VERSION_MAJOR >= 3
using ImfStreamSize = uint64_t;
#else
using ImfStreamSize = Imf::Int64;
#endif

class QIODeviceImfStream : public Imf::IStream {
public:

    QIODeviceImfStream(QIODevice& device, const std::string& filename) :
        Imf::IStream(filename.c_str()), _device(device) {
    }

    bool read(char c[/*n*/], int n) override {
        if (_device.read(c, n) <= 0) {
            qWarning(imagelogging) << "OpenEXR - in file " << fileName() << " : " << _device.errorString();
            return false;
        }
        return true;
    }

    ImfStreamSize tellg() override {
        return _device.pos();
    }

    void seekg(ImfStreamSize pos) override {
        _device.seek(pos);
    }

    void clear() override {
        // Not much to do
    }

private:

    QIODevice&  _device;
};

#endif

image::Image image::readOpenEXR(QIODevice& content, const std::string& filename) {
#if !defined(Q_OS_ANDROID)
    QIODeviceImfStream device(content, filename);

    if (Imf::isOpenExrFile(device)) {
        Imf::RgbaInputFile file(device);
        Imath::Box2i viewport = file.dataWindow();
        Imf::Array2D<Imf::Rgba> pixels;
        int width = viewport.max.x - viewport.min.x + 1;
        int height = viewport.max.y - viewport.min.y + 1;

        pixels.resizeErase(height, width);

        file.setFrameBuffer(&pixels[0][0] - viewport.min.x - viewport.min.y * width, 1, width);
        file.readPixels(viewport.min.y, viewport.max.y);

        // the pixels as floats, every channel kept: a lightmap stays at half precision from here to the GPU
        // (LIGHTMAPS.md); the cubemap path packs them to R11G11B10 itself, as this reader used to
        Image image{ width, height, Image::Format_RGBAF };

        for (int y = 0; y < height; y++) {
            const auto srcScanline = pixels[y];
            glm::vec4* dstScanline = (glm::vec4*) image.editScanLine(y);

            for (int x = 0; x < width; x++) {
                const auto& srcPixel = srcScanline[x];
                dstScanline[x] = glm::vec4((float)srcPixel.r, (float)srcPixel.g, (float)srcPixel.b, (float)srcPixel.a);
            }
        }
        return image;
    } else {
        qWarning(imagelogging) << "OpenEXR - File " << filename.c_str() << " doesn't have the proper format";
    }
#endif

    return QImage();
}
