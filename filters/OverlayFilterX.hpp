/******************************************************************************
 * Copyright (c) 2017, Hobu Inc. <hobu.inc@gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following
 * conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
 *       names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 ****************************************************************************/

#pragma once

#include <pdal/Filter.hpp>
#include <pdal/Polygon.hpp>
#include <pdal/Streamable.hpp>

#include <ogr_api.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

// Get GDAL's forward decls if available
// otherwise make our own
#if __has_include(<gdal_fwd.h>)
#include <gdal_fwd.h>
#else
using OGRLayerH = void*;
#endif

namespace pdal
{

namespace gdal
{
class ErrorHandler;
}

#if __has_include(<gdal_fwd.h>)
typedef std::shared_ptr<std::remove_pointer<OGRDataSourceH>::type> OGRDSPtr;
typedef std::shared_ptr<std::remove_pointer<OGRFeatureH>::type> OGRFeaturePtr;
#else
typedef std::shared_ptr<void> OGRDSPtr;
typedef std::shared_ptr<void> OGRFeaturePtr;
#endif

class Arg;

// using IntOrReal = std::variant<int64_t, double>;
using IntOrReal = int64_t;

using IntOrRealList = std::vector<IntOrReal>;

struct FieldInfo
{
    std::string name;
    int index;
    Dimension::Type type;

    // FieldInfo(OGRLayerH lyr, std::string fieldName)
    // : FieldInfo(lyr, OGR_L_FindFieldIndex(lyr, fieldName.c_str(), 0))
    // {
    // }

    // FieldInfo(OGRLayerH lyr, int index) : index{index}
    FieldInfo(OGRLayerH lyr, std::string fieldName)
    {
        name = fieldName;
        index = OGR_L_FindFieldIndex(lyr, fieldName.c_str(), 0);
        std::cout << "try: " << fieldName << " = " << index << "\n";
        // TODO check index=-1 error
        auto lyrDef = OGR_L_GetLayerDefn(lyr);
        std::cout << "1 ";
        auto fieldDef = OGR_FD_GetFieldDefn(lyrDef, index);
        std::cout << "2 ";
        auto ftype = OGR_Fld_GetType(fieldDef);
        std::cout << "ftype = " << ftype << "\n";
        std::cout << "3 ";
        // auto x = OGR_FD_GetName(fieldDef);
        // std::cout << x << "\n";
        // if (x == nullptr)
        // {
        //     std::cout << "3 nullptr ";
        //     x = "nullptr";
        // }
        // else
        // {
        //     name = x;
        // }
        std::cout << "4 ";
        type = mapOGRToDimType(ftype);
        std::cout << "5 ";
    }

    IntOrReal read(const OGRFeaturePtr featurePtr) const
    {
        return read(featurePtr.get());
    }

    IntOrReal read(const OGRFeatureH feature) const
    {
        switch (type)
        {
        case Dimension::Type::Double:
            return OGR_F_GetFieldAsDouble(feature, index);

        case Dimension::Type::Signed64:
            return OGR_F_GetFieldAsInteger64(feature, index);

        default:
            // ???
            return -1;
        }
    }

    static Dimension::Type mapOGRToDimType(const OGRFieldType ogrType)
    {
        switch (ogrType)
        {
        case OGRFieldType::OFTReal:
            return Dimension::Type::Double;

        case OGRFieldType::OFTInteger64:
        case OGRFieldType::OFTInteger:
            return Dimension::Type::Signed64;

        default:
            return Dimension::Type::None;
        }
    }
};

class OverlayFilterX : public Filter, public Streamable
{

    struct PolyVal
    {
        Polygon geom;
        IntOrRealList values;
    };

public:
    OverlayFilterX() : m_ds(0) {}

    std::string getName() const
    {
        return "filters.overlayX";
    }

private:
    virtual void addArgs(ProgramArgs& args);
    virtual void spatialReferenceChanged(const SpatialReference& srs);
    virtual bool processOne(PointRef& point);
    virtual void initialize();
    virtual void prepared(PointTableRef table);
    virtual void ready(PointTableRef table);
    virtual void filter(PointView& view);

    OverlayFilterX& operator=(const OverlayFilterX&) = delete;
    OverlayFilterX(const OverlayFilterX&) = delete;

    std::vector<IntOrRealList> intersect(double x, double y,
                                         bool firstOnly) const;

    OGRDSPtr m_ds;
    std::string m_datasource;
    StringList m_columns;
    std::string m_query;
    std::string m_layer;

    StringList m_dimNames;
    Dimension::IdList m_dims;

    std::vector<FieldInfo> m_fields;
    std::vector<PolyVal> m_polygons;
    BOX2D m_bounds;
    int m_threads;
    bool m_firstOnly;
};

} // namespace pdal
