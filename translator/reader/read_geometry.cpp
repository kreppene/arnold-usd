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
#include "read_geometry.h"
#include "registry.h"

#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/curves.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdVol/volume.h>
#include <pxr/usd/usdVol/openVDBAsset.h>

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/tf/stringUtils.h>

#include <ai.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "utils.h"
//-*************************************************************************

PXR_NAMESPACE_USING_DIRECTIVE

/** Exporting a USD Mesh description to Arnold
 **/
void UsdArnoldReadMesh::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("polymesh", prim.GetPath().GetText());
    
    AiNodeSetBool(node, "smoothing", true);

    // Get mesh.
    UsdGeomMesh mesh(prim);

    MeshOrientation meshOrientation;
    // Get orientation. If Left-handed, we will need to invert the vertex
    // indices
    {
        TfToken orientation_token;
        if (mesh.GetOrientationAttr().Get(&orientation_token)) {
            if (orientation_token == UsdGeomTokens->leftHanded) {
                meshOrientation.reverse = true;
                mesh.GetFaceVertexCountsAttr().Get(&meshOrientation.nsides_array, frame);
            }
        }
    }
    exportArray<int, unsigned char>(mesh.GetFaceVertexCountsAttr(), node, "nsides", time);

    if (!meshOrientation.reverse) {
        // Basic right-handed orientation, no need to do anything special here
        exportArray<int, unsigned int>(mesh.GetFaceVertexIndicesAttr(), node, "vidxs", time);
    } else {
        // We can't call exportArray here because the orientation requires to
        // reverse face attributes. So we're duplicating the function here.
        VtIntArray array;
        mesh.GetFaceVertexIndicesAttr().Get(&array, frame);
        size_t size = array.size();

        if (size > 0) {
            meshOrientation.orientFaceIndexAttribute(array);

            // Need to convert the data from int to unsigned int
            std::vector<unsigned int> arnold_vec(array.begin(), array.end());
            AiNodeSetArray(node, "vidxs", AiArrayConvert(size, 1, AI_TYPE_UINT, arnold_vec.data()));
        } else
            AiNodeResetParameter(node, "vidxs");
    }


    // Vertex positions
    exportArray<GfVec3f, GfVec3f>(mesh.GetPointsAttr(), node, "vlist", time);

    VtValue sidedness;
    if (mesh.GetDoubleSidedAttr().Get(&sidedness))
        AiNodeSetByte(node, "sidedness", vtValueGetBool(sidedness) ? AI_RAY_ALL : 0);

    // reset subdiv_iterations to 0, it might be set in readArnoldParameter
    AiNodeSetByte(node, "subdiv_iterations", 0);
    exportMatrix(prim, node, time, context);

    exportPrimvars(prim, node, time, &meshOrientation);

    std::vector<UsdGeomSubset> subsets = UsdGeomSubset::GetAllGeomSubsets(mesh);

    if (!subsets.empty()) {
        // Currently, subsets are only used for shader & disp_map assignments
        VtIntArray faceVtxArray;
        mesh.GetFaceVertexCountsAttr().Get(&faceVtxArray, time.frame);
        exportSubsetsMaterialBinding(prim, node, context, subsets, faceVtxArray.size());
    } else {
        exportMaterialBinding(prim, node, context);
    }

    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check if subdiv_iterations were set in readArnoldParameters,
    // and only set the subdiv_type if it's > 0. If we don't do this, 
    // we get smoothed normals by default
    if (AiNodeGetByte(node, "subdiv_iterations") > 0) {
        TfToken subdiv;
        mesh.GetSubdivisionSchemeAttr().Get(&subdiv);
        if (subdiv == UsdGeomTokens->none)
            AiNodeSetStr(node, "subdiv_type", "none");
        else if (subdiv == UsdGeomTokens->catmullClark)
            AiNodeSetStr(node, "subdiv_type", "catclark");
        else if (subdiv == UsdGeomTokens->bilinear)
            AiNodeSetStr(node, "subdiv_type", "linear");
        else
            AiMsgWarning(
                "[usd] %s subdivision scheme not supported for mesh on path %s", subdiv.GetString().c_str(),
                mesh.GetPath().GetString().c_str());
    }

    // Check the prim visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);

}

void UsdArnoldReadCurves::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("curves", prim.GetPath().GetText());
    
    int basis = 3;
    if (prim.IsA<UsdGeomBasisCurves>()) {
        // TODO: use a scope_pointer for curves and basisCurves.
        UsdGeomBasisCurves basisCurves(prim);
        TfToken curveType;
        basisCurves.GetTypeAttr().Get(&curveType, frame);
        if (curveType == UsdGeomTokens->cubic) {
            TfToken basisType;
            basisCurves.GetBasisAttr().Get(&basisType, frame);

            if (basisType == UsdGeomTokens->bezier)
                basis = 0;
            else if (basisType == UsdGeomTokens->bspline)
                basis = 1;
            else if (basisType == UsdGeomTokens->catmullRom)
                basis = 2;
        }
    }
    AiNodeSetInt(node, "basis", basis);

    UsdGeomCurves curves(prim);
    // CV counts per curve
    exportArray<int, unsigned int>(curves.GetCurveVertexCountsAttr(), node, "num_points", time);
    // CVs positions
    exportArray<GfVec3f, GfVec3f>(curves.GetPointsAttr(), node, "points", time);
    AtArray *pointsArray = AiNodeGetArray(node, "points");
    unsigned int pointsSize = (pointsArray) ? AiArrayGetNumElements(pointsArray) : 0;

    // Widths
    // We need to divide the width by 2 in order to get the radius for arnold points
    VtArray<float> widthArray;
    if (curves.GetWidthsAttr().Get(&widthArray, time.frame)) {
        size_t widthCount = widthArray.size();
        if (widthCount <= 1 && pointsSize > widthCount) {
            // USD accepts empty width attributes, or a constant width for all points,
            // but arnold fails in that case. So we need to generate a dedicated array
            float radiusVal = (widthCount == 0) ? 0.f : widthArray[0] * 0.5f;
            // Create an array where each point has the same radius
            std::vector<float> radiusVec(pointsSize, radiusVal);
            AiNodeSetArray(node, "radius", AiArrayConvert(pointsSize, 1, AI_TYPE_FLOAT, &radiusVec[0]));
        } else if (widthCount > 0) {
            // TODO: Usd curves support vertex interpolation for the widths, but arnold doesn't 
            // (see #239)    
            AtArray* radiusArray = AiArrayAllocate(widthCount, 1, AI_TYPE_FLOAT);
            float* out = static_cast<float*>(AiArrayMap(radiusArray));
            for (unsigned int i = 0; i < widthCount; ++i) {
                out[i] = widthArray[i] * 0.5f; 
            }
            AiArrayUnmap(radiusArray);
            AiNodeSetArray(node, "radius", radiusArray);
        }
    }

    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    std::vector<UsdGeomSubset> subsets = UsdGeomSubset::GetAllGeomSubsets(curves);
    
    if (!subsets.empty()) {
        // Currently, subsets are only used for shader & disp_map assignments
        VtIntArray curveVtxArray;
        curves.GetCurveVertexCountsAttr().Get(&curveVtxArray, time.frame);
        exportSubsetsMaterialBinding(prim, node, context, subsets, curveVtxArray.size());
    } else {
        exportMaterialBinding(prim, node, context);
    }


    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the prim visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadPoints::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("points", prim.GetPath().GetText());
    
    UsdGeomPoints points(prim);

    // Points positions
    exportArray<GfVec3f, GfVec3f>(points.GetPointsAttr(), node, "points", time);
    AtArray *pointsArray = AiNodeGetArray(node, "points");
    unsigned int pointsSize = (pointsArray) ? AiArrayGetNumElements(pointsArray) : 0;

    // Points radius
    // We need to divide the width by 2 in order to get the radius for arnold points
    VtArray<float> widthArray;
    if (points.GetWidthsAttr().Get(&widthArray, time.frame)) {
        size_t widthCount = widthArray.size();
        if (widthCount <= 1 && pointsSize > widthCount) {
            // USD accepts empty width attributes, or a constant width for all points,
            // but arnold fails in that case. So we need to generate a dedicated array
            float radiusVal = (widthCount == 0) ? 0.f : widthArray[0] * 0.5f;
            // Create an array where each point has the same radius
            std::vector<float> radiusVec(pointsSize, radiusVal);
            AiNodeSetArray(node, "radius", AiArrayConvert(pointsSize, 1, AI_TYPE_FLOAT, &radiusVec[0]));
        } else if (widthCount > 0) {
            AtArray* radiusArray = AiArrayAllocate(widthCount, 1, AI_TYPE_FLOAT);
            float* out = static_cast<float*>(AiArrayMap(radiusArray));
            for (unsigned int i = 0; i < widthCount; ++i) {
                out[i] = widthArray[i] * 0.5f; 
            }
            AiArrayUnmap(radiusArray);
            AiNodeSetArray(node, "radius", radiusArray);
        }
    }

    exportMatrix(prim, node, time, context);

    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");
    // Check the primitive visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

/**
 *   Convert the basic USD shapes (cube, sphere, cylinder, cone,...)
 *   to Arnold. There are 2 main differences so far :
 *      - capsules don't exist in arnold
 *      - cylinders are different (one is closed the other isn't)
 **/
void UsdArnoldReadCube::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("box", prim.GetPath().GetText());
    
    UsdGeomCube cube(prim);

    VtValue size_attr;
    if (cube.GetSizeAttr().Get(&size_attr)) {
        float size_value = vtValueGetFloat(size_attr);
        AiNodeSetVec(node, "min", -size_value / 2.f, -size_value / 2.f, -size_value / 2.f);
        AiNodeSetVec(node, "max", size_value / 2.f, size_value / 2.f, size_value / 2.f);
    }

    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the primitive visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadSphere::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("sphere", prim.GetPath().GetText());
    
    UsdGeomSphere sphere(prim);

    VtValue radius_attr;
    if (sphere.GetRadiusAttr().Get(&radius_attr))
        AiNodeSetFlt(node, "radius", vtValueGetFloat(radius_attr));

    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the primitive visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

// Conversion code that is common to cylinder, cone and capsule
template <class T>
void exportCylindricalShape(const UsdPrim &prim, AtNode *node, const char *radius_name)
{
    T geom(prim);

    VtValue radius_attr;
    if (geom.GetRadiusAttr().Get(&radius_attr))
        AiNodeSetFlt(node, radius_name, UsdArnoldPrimReader::vtValueGetFloat(radius_attr));

    float height = 1.f;
    VtValue height_attr;
    if (geom.GetHeightAttr().Get(&height_attr))
        height = UsdArnoldPrimReader::vtValueGetFloat(height_attr);

    height /= 2.f;

    TfToken axis;
    geom.GetAxisAttr().Get(&axis);
    AtVector bottom(0.f, 0.f, 0.f);
    AtVector top(0.f, 0.f, 0.f);

    if (axis == UsdGeomTokens->x) {
        bottom.x = -height;
        top.x = height;
    } else if (axis == UsdGeomTokens->y) {
        bottom.y = -height;
        top.y = height;
    } else // UsdGeomTokens->z or unknown
    {
        bottom.z = -height;
        top.z = height;
    }
    AiNodeSetVec(node, "bottom", bottom.x, bottom.y, bottom.z);
    AiNodeSetVec(node, "top", top.x, top.y, top.z);
}

void UsdArnoldReadCylinder::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    AtNode *node = context.createArnoldNode("cylinder", prim.GetPath().GetText());
    
    exportCylindricalShape<UsdGeomCylinder>(prim, node, "radius");
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadCone::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    AtNode *node = context.createArnoldNode("cone", prim.GetPath().GetText());
    
    exportCylindricalShape<UsdGeomCone>(prim, node, "bottom_radius");

    const TimeSettings &time = context.getTimeSettings();
    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, time.frame))
        AiNodeSetByte(node, "visibility", 0);
}


// Note that we don't have capsule shapes in Arnold. Do we want to make a
// special case, and combine cylinders with spheres, or is it enough for now ?
void UsdArnoldReadCapsule::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    AtNode *node = context.createArnoldNode("cylinder", prim.GetPath().GetText());
    
    exportCylindricalShape<UsdGeomCapsule>(prim, node, "radius");
    const TimeSettings &time = context.getTimeSettings();
    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context);
    readArnoldParameters(prim, context, node, time, "primvars:arnold");

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, time.frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadBounds::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    if (!context.getPrimVisibility(prim, frame))
        return;

    AtNode *node = context.createArnoldNode("box", prim.GetPath().GetText());
    
    if (!prim.IsA<UsdGeomBoundable>())
        return;

    UsdGeomBoundable boundable(prim);
    VtVec3fArray extent;

    UsdGeomBoundable::ComputeExtentFromPlugins(boundable,
                                         UsdTimeCode(frame),
                                         &extent);
    
    AiNodeSetVec(node, "min", extent[0][0], extent[0][1], extent[0][2]);
    AiNodeSetVec(node, "max", extent[1][0], extent[1][1], extent[1][2]);
    exportMatrix(prim, node, time, context);

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadGenericPolygons::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    if (!context.getPrimVisibility(prim, frame))
        return;

    AtNode *node = context.createArnoldNode("polymesh", prim.GetPath().GetText());
    
    if (!prim.IsA<UsdGeomMesh>())
        return;

    UsdGeomMesh mesh(prim);

    MeshOrientation mesh_orientation;
    // Get orientation. If Left-handed, we will need to invert the vertex
    // indices
    {
        TfToken orientation_token;
        if (mesh.GetOrientationAttr().Get(&orientation_token)) {
            if (orientation_token == UsdGeomTokens->leftHanded) {
                mesh_orientation.reverse = true;
                mesh.GetFaceVertexCountsAttr().Get(&mesh_orientation.nsides_array, frame);
            }
        }
    }
    exportArray<int, unsigned char>(mesh.GetFaceVertexCountsAttr(), node, "nsides", time);

    if (!mesh_orientation.reverse) {
        // Basic right-handed orientation, no need to do anything special here
        exportArray<int, unsigned int>(mesh.GetFaceVertexIndicesAttr(), node, "vidxs", time);
    } else {
        // We can't call exportArray here because the orientation requires to
        // reverse face attributes. So we're duplicating the function here.
        VtIntArray array;
        mesh.GetFaceVertexIndicesAttr().Get(&array, frame);
        size_t size = array.size();
        if (size > 0) {
            mesh_orientation.orientFaceIndexAttribute(array);

            // Need to convert the data from int to unsigned int
            std::vector<unsigned int> arnold_vec(array.begin(), array.end());
            AiNodeSetArray(node, "vidxs", AiArrayConvert(size, 1, AI_TYPE_UINT, arnold_vec.data()));
        } else
            AiNodeResetParameter(node, "vidxs");
    } 
    exportArray<GfVec3f, GfVec3f>(mesh.GetPointsAttr(), node, "vlist", time);
    exportMatrix(prim, node, time, context);

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

void UsdArnoldReadGenericPoints::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    AtNode *node = context.createArnoldNode("points", prim.GetPath().GetText());
    
    if (!prim.IsA<UsdGeomPointBased>())
        return;
    
    UsdGeomPointBased points(prim);
    exportArray<GfVec3f, GfVec3f>(points.GetPointsAttr(), node, "points", time);

    // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
    if (!context.getPrimVisibility(prim, frame))
        AiNodeSetByte(node, "visibility", 0);
}

/** 
 *    Convert the Point Instancer node to Arnold. Since there is no such node in Arnold (yet),
 *    we need to convert it as ginstances, one for each instance. 
 *    There are however certain use case that are more complex :
 *        - a point instancer instantiating another point instancer (how to handle the recursion ?)
 *        - one of the "proto nodes" to be instantiated is a Xform in the middle of the hierarchy, and thus doesn't match 
               an existing arnold node (here we'd need to create one ginstance per leaf node below this xform)
 *
 *     A simple way to address these issues, is to check if each "proto node" exists in the Arnold scene 
 *     or not. If it doesn't, then we create a usd procedural with object_path pointing at this path. This way,
 *     each instance of this usd procedural will properly instantiate the whole contents of this path.
 **/

void UsdArnoldReadPointInstancer::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    const TimeSettings &time = context.getTimeSettings();
    float frame = time.frame;

    // If the USD primitive is hidden, we need to hide each of the nodes that are being created here
    bool isVisible = context.getPrimVisibility(prim, frame);

    UsdGeomPointInstancer pointInstancer(prim);
    
    // this will be used later to contruct the name of the instances
    std::string primName = prim.GetPath().GetText();

    // get all proto paths (i.e. input nodes to be instantiated) 
    SdfPathVector protoPaths;
    pointInstancer.GetPrototypesRel().GetTargets(&protoPaths);

    // get the usdFilePath from the reader, we will use this path later to apply when we create new usd procs
    std::string filename = context.getReader()->getFilename();
    
    // Same as above, get the eventual overrides from the reader
    const AtArray *overrides = context.getReader()->getOverrides();
            
    // get proto type index for all instances
    VtIntArray protoIndices;
    pointInstancer.GetProtoIndicesAttr().Get(&protoIndices, frame);

    
    for (size_t i = 0; i < protoPaths.size(); ++i) 
    {

        const SdfPath& protoPath = protoPaths.at(i);
        // get the proto primitive, and ensure it's properly exported to arnold,
        // since we don't control the order in which nodes are read.
        UsdPrim protoPrim = context.getReader()->getStage()->GetPrimAtPath(protoPath);
        std::string objType = (protoPrim) ? protoPrim.GetTypeName().GetText() : "";

        // I need to create a new proto node in case this primitive isn't directly translated as an Arnold AtNode.
        // As of now, this only happens for Xform and Point Instancer nodes, so I'm checking for these types,
        // and also I'm verifying if the registry is able to read nodes of this type.
        // In the future we might want to make this more robust, we could eventually add a function in
        // the primReader telling us if this primitive will generate an arnold node with the same name or not.
        bool createProto = (objType == "Xform" || objType == "PointInstancer" || 
            objType == "" || (context.getReader()->getRegistry()->getPrimReader(objType) == nullptr));
        
        if (createProto)
        {
            // There's no AtNode for this proto, we need to create a usd procedural that loads 
            // the same usd file but points only at this object path
            AtNode *node = context.createArnoldNode("usd", protoPath.GetText());
            
            AiNodeSetStr(node, "filename", filename.c_str());
            AiNodeSetStr(node, "object_path", protoPath.GetText());
            AiNodeSetFlt(node, "frame", frame); // give it the desired frame
            AiNodeSetFlt(node, "motion_start", time.motion_start);
            AiNodeSetFlt(node, "motion_end", time.motion_end);
            if (overrides)
                AiNodeSetArray(node, "overrides", AiArrayCopy(overrides));

            if (!isVisible)
                AiNodeSetByte(node, "visibility", 0);
        }        
    }
    std::vector<UsdTimeCode> times;
    if (time.motion_blur) {
        times.push_back(time.start());
        times.push_back(time.end());
    } else {
        times.push_back(frame);
    }
    std::vector<bool> pruneMaskValues = pointInstancer.ComputeMaskAtTime(frame);
    if (!pruneMaskValues.empty() && pruneMaskValues.size() != protoIndices.size())
    {
        // If the amount of prune mask elements doesn't match the amount of instances,
        // then something is wrong. We dump an error and clear the mask vector.
        AiMsgError("[usd] Point instancer %s : Mismatch in length of indices and mask", primName.c_str());
        pruneMaskValues.clear(); 
    }
    
    std::vector<VtArray<GfMatrix4d> > xformsArray;
    pointInstancer.ComputeInstanceTransformsAtTimes(&xformsArray, times, frame);

    for (size_t i = 0; i < protoIndices.size(); ++i) 
    {
        // This instance has to be pruned, let's skip it
        if (!pruneMaskValues.empty() && pruneMaskValues[i] == false)
            continue;
        
        std::vector<float> xform;
        //loop over all the motion steps and append the matrices as a big list of floats

        for (size_t t = 0; t < xformsArray.size(); ++t)
        {
            const double* matrixArray = xformsArray[t][i].GetArray();
            xform.insert(xform.end(), matrixArray, matrixArray + 16);
        }

        // construct the instance name, based on the point instancer name,
        // suffixed by the instance number
        std::string instance_name = TfStringPrintf("%s_%d", primName.c_str(), i);
        
        // create a ginstance pointing at this proto node
        AtNode *arnold_instance = context.createArnoldNode("ginstance", instance_name.c_str());
                
        AiNodeSetBool(arnold_instance, "inherit_xform", false);
        int protoId = protoIndices[i]; // which proto to instantiate

        // Add a connection from ginstance.node to the desired proto. This connection will be applied
        // after all nodes were exported to Arnold.
        if (protoId < protoPaths.size()) // safety out-of-bounds check, shouldn't happen
            context.addConnection(arnold_instance, "node", protoPaths.at(protoId).GetText(), UsdArnoldReaderContext::CONNECTION_PTR);
        AiNodeSetFlt(arnold_instance, "motion_start", time.motion_start);
        AiNodeSetFlt(arnold_instance, "motion_end", time.motion_end);     
        // set the instance xform
        AiNodeSetArray(arnold_instance, "matrix", AiArrayConvert(1,  xform.size() / 16, AI_TYPE_MATRIX, xform.data() ));        
        // Check the primitive visibility, set the AtNode visibility to 0 if it's meant to be hidden
        // Otherwise, force it to be visible to all rays, because the proto might be hidden
        if (!isVisible)
            AiNodeSetByte(arnold_instance, "visibility", 0);
        else
            AiNodeSetByte(arnold_instance, "visibility", AI_RAY_ALL); 
    }
}
void UsdArnoldReadVolume::read(const UsdPrim &prim, UsdArnoldReaderContext &context)
{
    AtNode *node = context.createArnoldNode("volume", prim.GetPath().GetText());
    UsdVolVolume volume(prim);
    const TimeSettings &time = context.getTimeSettings();
    
    UsdVolVolume::FieldMap fields = volume.GetFieldPaths();
    std::string filename;
    std::vector<std::string> grids;

    // Loop over all the fields in this volume node.
    // Note that arnold doesn't support grids from multiple vdb files, as opposed to USD volumes.
    // So we can only use the first .vdb that is found, and we'll dump a warning if needed.
    for (UsdVolVolume::FieldMap::iterator it = fields.begin(); it != fields.end(); ++it) {

        UsdPrim fieldPrim = context.getReader()->getStage()->GetPrimAtPath(it->second);
        if (!fieldPrim.IsA<UsdVolOpenVDBAsset>())
            continue;
        UsdVolOpenVDBAsset vdbAsset(fieldPrim);
        SdfAssetPath vdbPath;
        
        if (vdbAsset.GetFilePathAttr().Get(&vdbPath)) {
            VtValue filenameVal(vdbPath.GetResolvedPath());
            std::string fieldFilename = filenameVal.Get<std::string>();
            if (filename.empty())
                filename = fieldFilename;
            else if (fieldFilename != filename) {
                AiMsgWarning("[usd] %s: arnold volume nodes only support a single .vdb file. ", AiNodeGetName(node));
            }
            TfToken vdbGrid;
            if (vdbAsset.GetFieldNameAttr().Get(&vdbGrid)) {
                grids.push_back(vdbGrid);
            }
        }
    }

    // Now set the first vdb filename that was found
    AiNodeSetStr(node, "filename", AtString(filename.c_str()));

    // Set all the grids that are needed
    AtArray *gridsArray = AiArrayAllocate(grids.size(), 1, AI_TYPE_STRING);
    for (size_t i = 0; i < grids.size(); ++i) {
        AiArraySetStr(gridsArray, i, AtString(grids[i].c_str()));
    }
    AiNodeSetArray(node, "grids", gridsArray);

    exportMatrix(prim, node, time, context);
    exportPrimvars(prim, node, time);
    exportMaterialBinding(prim, node, context, false); // don't assign the default shader

    readArnoldParameters(prim, context, node, time, "primvars:arnold");
    // Check the prim visibility, set the AtNode visibility to 0 if it's hidden
    if (!context.getPrimVisibility(prim, time.frame))
        AiNodeSetByte(node, "visibility", 0);
}