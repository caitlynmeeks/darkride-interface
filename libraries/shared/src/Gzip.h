//
//  Gzip.h
//  libraries/shared/src
//
//  Created by Seth Alves on 2015-08-03.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef GZIP_H
#define GZIP_H

#include <QByteArray>

// The compression level must be Z_DEFAULT_COMPRESSION (-1), or between 0 and
// 9: 1 gives best speed, 9 gives best compression, 0 gives no
// compression at all (the input data is simply copied a block at a
// time).  Z_DEFAULT_COMPRESSION requests a default compromise between
// speed and compression (currently equivalent to level 6).

bool gzip(QByteArray source, QByteArray &destination, int compressionLevel = -1); // -1 is Z_DEFAULT_COMPRESSION

// The most one gunzip may produce unless the caller says otherwise (a world file of thousands of entities is a few MB).
// A caller that knows how big its real content gets passes its own limit.
static const int GZIP_DEFAULT_MAX_SIZE = 256 * 1024 * 1024;

// No limit can go above this: a QByteArray must be able to hold the result.
static const int GZIP_MAX_SIZE_CEILING = 1024 * 1024 * 1024;

// Unpacks a whole gzip stream into destination. An empty source gives an empty destination and true. Returns false,
// with destination empty, if the stream is broken or incomplete, or if its content would be larger than maxSize bytes
// (capped at GZIP_MAX_SIZE_CEILING).
bool gunzip(QByteArray source, QByteArray &destination, int maxSize = GZIP_DEFAULT_MAX_SIZE);

#endif
