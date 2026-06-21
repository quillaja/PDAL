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

#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <iostream>
#include <thread>
#include <vector>

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
    args.add("dimension", "Dimensions to assign from columns", m_dimNames).setPositional();
    args.add("datasource", "OGR-readable datasource for Polygon or Multipolygon data", m_datasource)
        .setPositional();
    args.add("column", "OGR datasource columns from which to read the attribute.", m_columns);
    args.add("query",
             "OGR SQL query to execute on the datasource to fetch geometry and "
             "attributes",
             m_query);
    args.add("layer", "Datasource layer to use", m_layer);
    args.addSynonym("layer", "lyr_name");
    args.add("bounds", "Bounds to limit query using with OGR_L_SetSpatialFilter", m_bounds);
    args.add("threads", "Number of threads used to run this filter", m_threads, 1);
    args.add("firstonly", "stop at first point-poly intersection", m_firstOnly, true);
}

void OverlayFilterX::initialize()
{
    gdal::registerDrivers();
}

void OverlayFilterX::prepared(PointTableRef table)
{
    for (const auto& name : m_dimNames)
    {
        auto id = table.layout()->findDim(name);
        if (id == Dimension::Id::Unknown)
            throwError("Dimension '" + name + "' not found.");
        m_dims.push_back(id);
    }
    log()->get(LogLevel::Info) << "m_dims: " << m_dims.size() << "\n";

    if (m_dims.size() != m_columns.size())
        throwError("Number of dimensions and number of columns are not equal!");

    if (m_threads < 1)
        throwError("Number of threads should be positive.");
}

void OverlayFilterX::ready(PointTableRef table)
{
    Datasource<int64_t> ds{m_datasource, m_bounds};

    if (!m_query.empty())
        m_table = ds.loadQuery(m_query, m_columns);
    else if (!m_layer.empty())
        m_table = ds.loadLayer(m_layer, m_columns);
    else
        m_table = ds.loadIndex(0, m_columns);
}

void OverlayFilterX::spatialReferenceChanged(const SpatialReference& srs)
{
    if (srs.empty())
        return;
    for (auto& poly : m_table.geom)
    {
        auto ok = poly.transform(srs);
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

std::vector<size_t> OverlayFilterX::intersect(double x, double y, bool firstOnly) const
{
    std::vector<size_t> rowIndicies;
    for (size_t i = 0; i < m_table.geom.size(); ++i)
    {
        if (m_table.geom[i].contains(x, y))
        {
            rowIndicies.push_back(i);
            if (firstOnly)
                break;
        }
    }
    return rowIndicies;
}

point_count_t appendCopy(PointViewPtr targetView, const PointView& srcView, PointId srcId)
{
    auto end = targetView->size();
    for (const auto& id : targetView->dims())
    {
        // stupid. float != int ffs
        targetView->setField(id, end, srcView.getFieldAs<double>(id, srcId));
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
                for (PointId id = start; id < end; id++)
                {
                    double x = view.getFieldAs<double>(Dimension::Id::X, id);
                    double y = view.getFieldAs<double>(Dimension::Id::Y, id);

                    // traverse the table, assign or create points
                    auto rowIndices = intersect(x, y, m_firstOnly);
                    for (const auto& row : rowIndices)
                    {
                        // the first polygon intersected must go to the
                        // existing point. the remaining polygons cause
                        // additional points to be created.
                        std::unique_ptr<PointRef> toWrite; // only way i could make it work
                        if (row == rowIndices[0])
                        {
                            toWrite = std::make_unique<PointRef>(view, id);
                        }
                        else
                        {
                            auto idxAppened = appendCopy(addedPoints[t], view, id);
                            toWrite = std::make_unique<PointRef>(*addedPoints[t], idxAppened);
                        }

                        auto attribs = m_table.getAttributes(row);
                        for (size_t i = 0; i < m_dims.size(); ++i)
                        {
                            // the indices for the dimensions and feature column
                            // should be aligned.
                            auto targetDim = m_dims[i];
                            auto dataForDim = attribs[m_columns[i]];
                            toWrite->setField(targetDim, dataForDim);
                        }
                    }
                }
            },
            t * chunk_size, (t + 1) == m_threads ? npoints : (t + 1) * chunk_size, t);
    }

    for (auto& t : threadList)
        t.join();

    for (const auto& add : addedPoints)
        view.append(*add);
}

} // namespace pdal
