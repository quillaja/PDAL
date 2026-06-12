/******************************************************************************
 * Copyright (c) 2017, Hobu Inc., info@hobu.co
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

#include "OverlayFilterX.hpp"

#include <thread>
#include <vector>

#include <ogr_api.h>

#include <pdal/Polygon.hpp>
#include <pdal/private/gdal/GDALUtils.hpp>
#include <pdal/private/gdal/SpatialRef.hpp>
#include <pdal/util/ProgramArgs.hpp>

namespace pdal
{

static StaticPluginInfo const s_info{
    "filters.overlayX",
    "Assign values to a dimension based on the extent of an OGR-readable data "
    "source or an OGR SQL query.",
    "None"};

CREATE_STATIC_STAGE(OverlayFilterX, s_info)

void OverlayFilterX::addArgs(ProgramArgs& args)
{
    args.add("dimension", "Dimension on which to filter", m_dimName)
        .setPositional();
    args.add("datasource",
             "OGR-readable datasource for Polygon or Multipolygon data",
             m_datasource)
        .setPositional();
    args.add("column",
             "OGR datasource column from which to read the attribute.",
             m_column);
    args.add("query",
             "OGR SQL query to execute on the datasource to fetch geometry and "
             "attributes",
             m_query);
    args.add("layer", "Datasource layer to use", m_layer);
    args.addSynonym("layer", "lyr_name");
    args.add("bounds",
             "Bounds to limit query using with OGR_L_SetSpatialFilter",
             m_bounds);
    args.add("threads", "Number of threads used to run this filter", m_threads,
             1);
    args.add("firstonly", "stop at first point-poly intersection", m_firstOnly,
             true);
}

void OverlayFilterX::initialize()
{
    gdal::registerDrivers();
}

void OverlayFilterX::prepared(PointTableRef table)
{
    m_dim = table.layout()->findDim(m_dimName);
    if (m_dim == Dimension::Id::Unknown)
        throwError("Dimension '" + m_dimName + "' not found.");
    if (m_threads < 1)
        throwError("Number of threads should be positive.");
}

void OverlayFilterX::ready(PointTableRef table)
{
    m_ds = OGRDSPtr(OGROpen(m_datasource.c_str(), 0, 0),
                    [](OGRDSPtr::element_type* p)
                    {
                        if (p)
                            ::OGR_DS_Destroy(p);
                    });
    if (!m_ds)
        throwError("Unable to open data source '" + m_datasource + "'");

    if (!m_query.empty())
        m_lyr = OGR_DS_ExecuteSQL(m_ds.get(), m_query.c_str(), 0, 0);
    else if (!m_layer.empty())
        m_lyr = OGR_DS_GetLayerByName(m_ds.get(), m_layer.c_str());
    else
        m_lyr = OGR_DS_GetLayer(m_ds.get(), 0);

    if (!m_lyr)
        throwError("Unable to select layer '" + m_layer + "'");

    if (!m_bounds.empty())
    {
        pdal::Polygon g(m_bounds.toWKT());
        OGR_L_SetSpatialFilter(m_lyr, g.getOGRHandle());
    }

    auto featureDeleter = [](OGRFeaturePtr::element_type* p)
    {
        if (p)
            ::OGR_F_Destroy(p);
    };
    OGRFeaturePtr feature =
        OGRFeaturePtr(OGR_L_GetNextFeature(m_lyr), featureDeleter);

    int field_index(1); // default to first column if nothing was set
    if (m_column.size())
    {
        field_index = OGR_F_GetFieldIndex(feature.get(), m_column.c_str());
        if (field_index == -1)
            throwError("No column name '" + m_column + "' was found.");
    }

    gdal::SpatialRef sref;
    sref.setFromLayer(m_lyr);
    SpatialReference layerSrs(sref.wkt());

    do
    {
        OGRGeometryH geom = OGR_F_GetGeometryRef(feature.get());
        int64_t fieldVal =
            OGR_F_GetFieldAsInteger64(feature.get(), field_index);

        m_polygons.push_back({Polygon(geom, layerSrs), fieldVal});

        feature = OGRFeaturePtr(OGR_L_GetNextFeature(m_lyr), featureDeleter);
    } while (feature);

    // Initialise m_grids, otherwise this will lead to a race condition when
    // using threading.
    for (const auto& poly : m_polygons)
    {
        poly.geom.initGrids();
    }
}

void OverlayFilterX::spatialReferenceChanged(const SpatialReference& srs)
{
    if (srs.empty())
        return;
    for (auto& poly : m_polygons)
    {
        auto ok = poly.geom.transform(srs);
        if (!ok)
            throwError(ok.what());
    }
}

bool OverlayFilterX::processOne(PointRef& point)
{
    // for (const auto &poly : m_polygons)
    // {
    //     double x = point.getFieldAs<double>(Dimension::Id::X);
    //     double y = point.getFieldAs<double>(Dimension::Id::Y);
    //     if (poly.geom.contains(x, y))
    //     {
    //         point.setField(m_dim, poly.val);
    //         return true;
    //     }
    // }
    return true;
}

std::vector<int64_t> OverlayFilterX::intersect(double x, double y,
                                               bool firstOnly) const
{
    std::vector<int64_t> data;
    for (const auto& poly : m_polygons)
    {
        if (poly.geom.contains(x, y))
        {
            data.push_back(poly.val);
            if (firstOnly)
                break;
        }
    }
    return data;
}

point_count_t appendCopy(PointViewPtr targetView, PointRef srcPoint)
{
    auto end = targetView->size();
    for (const auto& id : targetView->dims())
    {
        // stupid. float != int ffs
        targetView->setField(id, end, srcPoint.getFieldAs<double>(id));
    }
    return end;
}

void OverlayFilterX::filter(PointView& view)
{
    point_count_t npoints = view.size();
    point_count_t chunk_size = npoints / m_threads;
    if (npoints % m_threads)
        chunk_size++;
    std::vector<std::thread> threadList(m_threads);

    std::vector<PointViewPtr> addedPoints;
    for (size_t i = 0; i < m_threads; ++i)
        addedPoints.push_back(view.makeNew());

    for (int t = 0; t < m_threads; t++)
    {
        threadList[t] = std::thread(
            [&](const PointId start, const PointId end, int t)
            {
                PointRef point(view, start);

                for (PointId id = start; id < end; id++)
                {
                    point.setPointId(id);
                    double x = point.getFieldAs<double>(Dimension::Id::X);
                    double y = point.getFieldAs<double>(Dimension::Id::Y);

                    auto data = intersect(x, y, m_firstOnly);
                    for (auto it = data.begin(); it != data.end(); ++it)
                    {
                        if (it == data.begin())
                        {
                            point.setField(m_dim, *it);
                        }
                        else
                        {
                            auto idxAppened = appendCopy(addedPoints[t], point);
                            addedPoints[t]->setField(m_dim, idxAppened, *it);
                        }
                    }
                }
            },
            t * chunk_size,
            (t + 1) == m_threads ? npoints : (t + 1) * chunk_size, t);
    }

    for (auto& t : threadList)
        t.join();

    for (const auto& add : addedPoints)
        view.append(*add);
}

} // namespace pdal
