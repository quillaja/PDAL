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
#include <string>
#include <unordered_map>

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

template <typename T = int64_t> struct FieldInfo
{
    std::string name;
    int index;
    Dimension::Type type;

    FieldInfo(OGRLayerH lyr, std::string fieldName)
        : FieldInfo(lyr, OGR_L_FindFieldIndex(lyr, fieldName.c_str(), 0))
    {
    }

    FieldInfo(OGRLayerH lyr, int index) : index{index}
    {
        // TODO check index=-1 error
        auto lyrDef = OGR_L_GetLayerDefn(lyr);
        auto fieldDef = OGR_FD_GetFieldDefn(lyrDef, index);
        auto ftype = OGR_Fld_GetType(fieldDef);
        type = mapOGRToDimType(ftype);
        name = OGR_Fld_GetNameRef(fieldDef);
    }

    T read(const OGRFeaturePtr featurePtr) const
    {
        return read(featurePtr.get());
    }

    T read(const OGRFeatureH feature) const
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

template <typename T = int64_t, typename GEOM = Polygon> struct Table
{
    struct Field
    {
        FieldInfo<T> meta;
        std::vector<T> values;
    };

    std::vector<GEOM> geom;
    std::unordered_map<std::string, Field> attributes;

    GEOM getGeom(const size_t row)
    {
        return geo[row];
    }
    std::unordered_map<std::string, T> getAttributes(const size_t row)
    {
        std::unordered_map<std::string, T> data;
        for (const auto& [name, attrib] : attributes)
            data[name] = attrib.values[row];
    }
};

template <typename T = int64_t> class Datasource
{
public:
    Datasource() = delete;
    Datasource(const Datasource&) = delete;

    Datasource(const std::string& source) : datasource{OGROpen(source.c_str(), 0, nullptr)} {};
    Datasource(const std::string& source, const BOX2D& bounds) : Datasource(source), bounds{bounds}
    {
    }

    ~Datasource()
    {
        OGR_DS_Destroy(datasource);
    }

    Table<T> loadLayer(const std::string& layerName, const std::vector<std::string>& fields)
    {
        load(OGR_DS_GetLayerByName(datasource, layerName.c_str()), fields);
    }
    Table<T> loadQuery(const std::string& query, const std::vector<std::string>& fields)
    {

        load(OGR_DS_ExecuteSQL(datasource, query.c_str(), 0, 0), fields);
    }

    Table<T> loadIndex(const uint32_t layerIndex, const std::vector<std::string>& fields)
    {
        load(OGR_DS_GetLayer(datasource, layerIndex), fields);
    }

private:
    OGRDataSourceH datasource;
    BOX2D bounds;

    Table<T> load(const OGRLayerH lyr, const std::vector<std::string>& fields)
    {
        Table<T> t;

        if (bounds.valid())
            OGR_L_SetSpatialFilterRect(lyr, bounds.minx, bounds.miny, bounds.maxx, bounds.maxy);

        // fill attribute metadata
        for (const auto& field : fields)
            t.attributes[field].meta = FieldInfo{lyr, field};

        auto srs = getSrs(lyr);

        for (auto feature = OGR_L_GetNextFeature(lyr); feature; feature = OGR_L_GetNextFeature(lyr))
        {
            // read attributes
            for (const auto& field : fields)
            {
                auto value = t.attributes[field].meta.read(feature); // goofy
                t.attributes[field].values.push_back(value);
            }
            // read geometry
            {
                auto geom = OGR_F_GetGeometryRef(feature);
                auto poly = Polygon(geom, srs);
                poly.initGrids();
                t.geom.push_back(poly);
            }
            // dispose
            OGR_F_Destroy(feature);
        }

        return t;
    }

    SpatialReference getSrs(OGRLayerH lyr)
    {
        auto srs_h = OGR_L_GetSpatialRef(lyr);
        char* c_wktstr = nullptr;
        OSRExportToWkt(srs_h, &c_wktstr);
        if (c_wktstr == nullptr)
            throwError("bad srs");
        const std::string srs_wkt{c_wktstr};
        CPLFree(c_wktstr);
        return SpatialReference{srs_wkt};
    }
};

class OverlayFilterX : public Filter, public Streamable
{

    struct PolyVal
    {
        Polygon geom;
        std::vector<int64_t> values;
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

    std::vector<std::vector<int64_t>> intersect(double x, double y, bool firstOnly) const;

    OGRDSPtr m_ds;
    std::string m_datasource;
    StringList m_columns;
    std::string m_query;
    std::string m_layer;

    StringList m_dimNames;
    Dimension::IdList m_dims;

    std::vector<FieldInfo<int64_t>> m_fields;
    std::vector<PolyVal> m_polygons;
    BOX2D m_bounds;
    int m_threads;
    bool m_firstOnly;
};

} // namespace pdal
