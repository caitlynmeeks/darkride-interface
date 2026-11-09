//
//  SpatialEvent.cpp
//  script-engine/src
//
//  Created by Stephen Birarda on 2014-10-27.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "SpatialEvent.h"

#include <RegisteredMetaTypes.h>

SpatialEvent::SpatialEvent() :
    locTranslation(0.0f),
    locRotation(),
    absTranslation(0.0f),
    absRotation()
{
    
}

SpatialEvent::SpatialEvent(const SpatialEvent& event) {
    locTranslation = event.locTranslation;
    locRotation = event.locRotation;
    absTranslation = event.absTranslation;
    absRotation = event.absRotation;
}

