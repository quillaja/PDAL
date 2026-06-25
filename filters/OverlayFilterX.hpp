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

#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

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

struct FieldInfo
{
    std::string name;
    int index;
    OGRFieldType type;
};

Dimension::Type mapOGRToDimType(const OGRFieldType ogrType)
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

template <typename T = int64_t, typename GEOM = Polygon> struct Table
{

    std::vector<GEOM> geom;
    std::unordered_map<std::string, FieldInfo> meta;
    std::unordered_map<std::string, std::vector<T>> attributes;

    GEOM getGeom(const size_t row)
    {
        return geo[row];
    }
    std::unordered_map<std::string, T> getAttributes(const size_t row)
    {
        std::unordered_map<std::string, T> data;
        for (const auto& [name, attrib] : attributes)
            data[name] = attrib[row];
        return data;
    }

    void print() const
    {
        std::vector<std::string> cols;
        size_t rows{0};
        size_t colW{0};
        for (const auto& [name, data] : attributes)
        {
            cols.push_back(name);
            colW = (std::max)(colW, name.size());
            rows = data.size();
        }

        for (const auto& col : cols)
            std::cout << std::setw(colW) << col;
        std::cout << "\n";
        for (size_t row = 0; row < rows; ++row)
        {
            for (const auto& col : cols)
                std::cout << std::setw(colW) << attributes.at(col)[row];
            std::cout << "\n";
        }
    }
};

class Datasource
{
public:
    Datasource() = delete;
    Datasource(const Datasource&) = delete;
    Datasource operator=(const Datasource&) = delete;

    Datasource(const std::string& source) : Datasource(source, BOX2D{}) {};
    Datasource(const std::string& source, const BOX2D& bounds)
        : datasource{OGROpen(source.c_str(), 0, nullptr)}, bounds{bounds}
    {
    }

    ~Datasource()
    {
        OGR_DS_Destroy(datasource);
    }

    std::vector<std::string> getLayerNames() const
    {
        std::vector<std::string> layers;
        size_t count = OGR_DS_GetLayerCount(datasource);
        for (size_t i = 0; i < count; i++)
        {
            auto lyr = OGR_DS_GetLayer(datasource, i);
            layers.push_back(OGR_L_GetName(lyr));
        }
        return layers;
    }

    template <typename T = int64_t>
    Table<T> loadLayer(const std::string& layerName, const std::vector<std::string>& fields) const
    {
        return load<T>(OGR_DS_GetLayerByName(datasource, layerName.c_str()), fields);
    }
    template <typename T = int64_t>
    Table<T> loadQuery(const std::string& query, const std::vector<std::string>& fields) const
    {

        return load<T>(OGR_DS_ExecuteSQL(datasource, query.c_str(), 0, 0), fields);
    }
    template <typename T = int64_t>
    Table<T> loadIndex(const uint32_t layerIndex, const std::vector<std::string>& fields) const
    {
        return load<T>(OGR_DS_GetLayer(datasource, layerIndex), fields);
    }

private:
    OGRDataSourceH datasource;
    BOX2D bounds;

    FieldInfo getMeta(const OGRLayerH lyr, const std::string& fieldName) const
    {
        return getMeta(lyr, OGR_L_FindFieldIndex(lyr, fieldName.c_str(), 0));
    }

    FieldInfo getMeta(const OGRLayerH lyr, int index) const
    {
        // TODO check index=-1 error
        auto lyrDef = OGR_L_GetLayerDefn(lyr);
        auto fieldDef = OGR_FD_GetFieldDefn(lyrDef, index);
        return FieldInfo{
            .name = OGR_Fld_GetNameRef(fieldDef),
            .index = index,
            .type = OGR_Fld_GetType(fieldDef),
        };
    }

    template <typename T> T read(const OGRFeatureH feature, const FieldInfo& fieldMeta) const
    {
        switch (fieldMeta.type)
        {
        case OGRFieldType::OFTReal:
            return static_cast<T>(OGR_F_GetFieldAsDouble(feature, fieldMeta.index));

        case OGRFieldType::OFTInteger64:
        case OGRFieldType::OFTInteger:
            return static_cast<T>(OGR_F_GetFieldAsInteger64(feature, fieldMeta.index));

            // case OGRFieldType::OFTWideString:
            //     return static_cast<T>(OGR_F_GetFieldAsString(feature, index));

            // case OGRFieldType::OFTDate:
            // case OGRFieldType::OFTDateTime:
            //     return static_cast<T>(OGR_F_GetFieldAsDateTime(feature, index));

        default:
            // ???
            throw std::exception("don't know what to do with that field");
        }
    }

    template <typename T>
    Table<T> load(const OGRLayerH lyr, const std::vector<std::string>& fields) const
    {
        Table<T> t;

        if (bounds.valid())
            OGR_L_SetSpatialFilterRect(lyr, bounds.minx, bounds.miny, bounds.maxx, bounds.maxy);

        // fill attribute metadata
        for (const auto& field : fields)
            t.meta[field] = getMeta(lyr, field);

        auto srs = getSrs(lyr);

        for (auto feature = OGR_L_GetNextFeature(lyr); feature; feature = OGR_L_GetNextFeature(lyr))
        {
            // read attributes
            for (const auto& field : fields)
            {
                T value{read<T>(feature, t.meta[field])};
                t.attributes[field].push_back(value);
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

    SpatialReference getSrs(OGRLayerH lyr) const
    {
        auto srs_h = OGR_L_GetSpatialRef(lyr);
        char* c_wktstr = nullptr;
        OSRExportToWkt(srs_h, &c_wktstr);
        if (c_wktstr == nullptr)
            throw std::exception("bad srs");
        const std::string srs_wkt{c_wktstr};
        CPLFree(c_wktstr);
        return SpatialReference{srs_wkt};
    }
};

class OverlayFilterX : public Filter, public Streamable
{

public:
    OverlayFilterX() {}

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

    std::vector<size_t> intersect(double x, double y, bool firstOnly) const;

    Table<int64_t, Polygon> m_table;

    std::string m_datasource;
    std::string m_query;
    std::string m_layer;

    // all 3 are index-aligned
    StringList m_columns;
    StringList m_dimNames;
    Dimension::IdList m_dims;

    BOX2D m_bounds;
    int m_threads;
    bool m_firstOnly;
};

} // namespace pdal
