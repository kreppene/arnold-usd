// Copyright 2019 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/// @file shape.h
///
/// Utilities for handling instanceable Arnold Shapes.
#pragma once

#include "api.h"

#include <ai.h>

#include <pxr/pxr.h>

#include <pxr/imaging/hd/rprim.h>

#include "render_delegate.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdRprim;

/// Utility class for handling instanceable Arnold Shapes.
class HdArnoldShape {
public:
    /// Constructor for HdArnoldShape.
    ///
    /// @param shapeType AtString storing the type of the Arnold Shape node.
    /// @param delegate Pointer to the Render Delegate.
    /// @param id Path to the primitive.
    HDARNOLD_API
    HdArnoldShape(const AtString& shapeType, HdArnoldRenderDelegate* delegate, const SdfPath& id, const int32_t primId);

    /// Destructor for HdArnoldShape.
    ///
    /// Frees the shape and all the ginstances created.
    HDARNOLD_API
    ~HdArnoldShape();

    /// Gets the Arnold Shape.
    ///
    /// @return Pointer to the Arnold Shape.
    AtNode* GetShape() { return _shape; }
    /// Gets the Arnold Shape.
    ///
    /// @return Constant pointer to the Arnold Shape.
    const AtNode* GetShape() const { return _shape; }
    /// Gets the Render Delegate.
    ///
    /// @return Pointer to the Render Delegate.
    HdArnoldRenderDelegate* GetDelegate() { return _delegate; }
    /// Syncs internal data and arnold state with hydra.
    HDARNOLD_API
    void Sync(HdRprim* rprim, HdDirtyBits dirtyBits, HdSceneDelegate* sceneDelegate, HdArnoldRenderParam* param);

protected:
    /// Sets a new hydra-provided primId
    void _SetPrimId(int32_t primId);
    /// Syncs the Instances.
    ///
    /// Creates and updates all the instances and destroys the ones not required
    /// anymore using the Dirty Bits.
    ///
    /// @param dirtyBits Dirty Bits to sync.
    /// @param sceneDelegate Pointer to the Scene Delegate.
    /// @param id Path to the primitive.
    /// @param instancerId Path to the Point Instancer.
    void _SyncInstances(
        HdDirtyBits dirtyBits, HdSceneDelegate* sceneDelegate, HdArnoldRenderParam* param, const SdfPath& id,
        const SdfPath& instancerId);

    std::vector<AtNode*> _instances;   ///< Storing Pointers to the ginstances.
    AtNode* _shape;                    ///< Pointer to the Arnold Shape.
    HdArnoldRenderDelegate* _delegate; ///< Pointer to the Render Delegate.
};

PXR_NAMESPACE_CLOSE_SCOPE
