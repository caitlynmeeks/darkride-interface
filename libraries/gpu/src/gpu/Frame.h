//
//  Created by Bradley Austin Davis on 2016/07/26
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the frame's submitTime, mainTime and buildTime stamps (the present cadence and the refresh governor read
// them).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//
#ifndef hifi_gpu_Frame_h
#define hifi_gpu_Frame_h

#include <functional>
#include <queue>

#include "Forward.h"
#include "Batch.h"
#include "Resource.h"

namespace gpu {

    class Frame {
        friend class Context;

    public:
        Frame();
        virtual ~Frame();
        using Batches = std::vector<BatchPointer>;
        using FramebufferRecycler = std::function<void(const FramebufferPointer&)>;
        using OverlayRecycler = std::function<void(const TexturePointer&)>;

        StereoState stereoState;
        uint32_t frameIndex{ 0 };
        // FROZEN (vehicle path): see the note at the top of this file.
        /// usecTimestampNow() when the render thread handed the frame to the display plugin (the present cadence's
        /// frame latency: how long after the present that asked for it the frame was ready)
        uint64_t submitTime{ 0 };
        /// the frame trace's record of the game-loop frame this frame was built for (FrameTrace::currentFrame when
        /// the render args were taken), so the present thread writes its columns to the right frame; 0 = none
        uint64_t traceFrame{ 0 };
        // FROZEN (vehicle path): see the note at the top of this file.
        /// the main thread's frame (idle) that made this frame and the render thread's build of it, in microseconds:
        /// what the refresh governor reads to know whether the frames would fit a refresh at the display's full rate
        uint64_t mainTime{ 0 };
        uint64_t buildTime{ 0 };
        /// The view matrix used for rendering the frame, only applicable for HMDs
        Mat4 view;
        /// The sensor pose used for rendering the frame, only applicable for HMDs
        Mat4 pose;
        /// The collection of batches which make up the frame
        Batches batches;
        /// The main thread updates to buffers that are applicable for this frame.
        BufferUpdates bufferUpdates;
        /// The destination framebuffer in which the results will be placed
        FramebufferPointer framebuffer;
        /// How to process the framebuffer when the frame dies.  MUST BE THREAD SAFE
        FramebufferRecycler framebufferRecycler;

        std::queue<std::tuple<std::function<void(const QImage&)>, float, bool>> snapshotOperators;

    protected:
        friend class Deserializer;

        // Should be called once per frame, on the recording thred
        void finish();
        void preRender();
    };

};


#endif
