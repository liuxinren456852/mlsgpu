/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Moving least squares implementation.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <CL/cl.hpp>
#include <stdexcept>
#include <algorithm>
#include <boost/math/constants/constants.hpp>
#include "errors.h"
#include "mls.h"
#include "clh.h"
#include "misc.h"
#include "statistics.h"

std::map<std::string, MlsShape> MlsShapeWrapper::getNameMap()
{
    std::map<std::string, MlsShape> ans;
    ans["plane"] = MLS_SHAPE_PLANE;
    ans["sphere"] = MLS_SHAPE_SPHERE;
    return ans;
}

const Grid::size_type MlsFunctor::wgs[3] = {8, 8, 8};
const int MlsFunctor::subsamplingMin = 3; // must be at least log2 of highest wgs

MlsFunctor::MlsFunctor(const cl::Context &context, MlsShape shape)
    : kernelTime(Statistics::getStatistic<Statistics::Variable>("kernel.mls.processCorners.time"))
{
    // These would ideally be static assertions, but C++ doesn't allow that
    MLSGPU_ASSERT((1U << subsamplingMin) >= *std::max_element(wgs, wgs + 3), std::length_error);

    std::map<std::string, std::string> defines;
    defines["WGS_X"] = boost::lexical_cast<std::string>(wgs[0]);
    defines["WGS_Y"] = boost::lexical_cast<std::string>(wgs[1]);
    defines["WGS_Z"] = boost::lexical_cast<std::string>(wgs[2]);
    defines["FIT_SPHERE"] = shape == MLS_SHAPE_SPHERE ? "1" : "0";
    defines["FIT_PLANE"] = shape == MLS_SHAPE_PLANE ? "1" : "0";

    cl::Program program = CLH::build(context, "kernels/mls.cl", defines);
    kernel = cl::Kernel(program, "processCorners");

    setBoundaryLimit(1.0f);
}

void MlsFunctor::set(const Grid::difference_type offset[3],
                     const cl::Buffer &splats,
                     const cl::Buffer &commands,
                     const cl::Buffer &start,
                     unsigned int subsamplingShift)
{
    cl_int3 offset3 = {{ offset[0], offset[1], offset[2] }};

    kernel.setArg(1, splats);
    kernel.setArg(2, commands);
    kernel.setArg(3, start);
    kernel.setArg(4, 3 * subsamplingShift);
    kernel.setArg(5, offset3);
}

void MlsFunctor::set(const Grid::difference_type offset[3],
                     const SplatTreeCL &tree, unsigned int subsamplingShift)
{
    set(offset, tree.getSplats(), tree.getCommands(), tree.getStart(), subsamplingShift);
}

const Grid::size_type *MlsFunctor::alignment() const
{
    return wgs;
}

void MlsFunctor::enqueue(
    const cl::CommandQueue &queue,
    const cl::Image2D &distance,
    const Marching::Swathe &swathe,
    const std::vector<cl::Event> *events,
    cl::Event *event)
{
    Grid::size_type width = roundUp(swathe.width, wgs[0]);
    Grid::size_type height = roundUp(swathe.height, wgs[1]);

    MLSGPU_ASSERT(swathe.zStride >= height, std::invalid_argument);
    MLSGPU_ASSERT(swathe.zFirst <= swathe.zLast, std::invalid_argument);
    MLSGPU_ASSERT(swathe.zFirst % wgs[2] == 0, std::invalid_argument);
    MLSGPU_ASSERT(distance.getImageInfo<CL_IMAGE_WIDTH>() >= width, std::length_error);
    MLSGPU_ASSERT(distance.getImageInfo<CL_IMAGE_HEIGHT>() >= swathe.zStride * (swathe.zLast + 1) + swathe.zBias, std::length_error);

    kernel.setArg(0, distance);
    kernel.setArg(6, cl_uint(swathe.zStride));
    kernel.setArg(7, cl_int(swathe.zBias));

    const std::size_t wgs3 = wgs[0] * wgs[1] * wgs[2];
    const std::size_t blocks[3] =
    {
        width / wgs[0],
        height / wgs[1],
        divUp(swathe.zLast - swathe.zFirst + 1, wgs[2])
    };

    CLH::enqueueNDRangeKernel(queue,
                              kernel,
                              cl::NDRange(0, 0, swathe.zFirst),
                              cl::NDRange(wgs3 * blocks[0], blocks[1], blocks[2]),
                              cl::NDRange(wgs3, 1, 1),
                              events, event, &kernelTime);
}

void MlsFunctor::setBoundaryLimit(float limit)
{
    // This is computed theoretically based on the weight function, and assuming a
    // uniform distribution of samples and a straight boundary
    const float boundaryScale = (sqrt(6.0f) * 512) / (693 * boost::math::constants::pi<float>());
    const float gamma = boundaryScale * limit;
    kernel.setArg(8, 1.0f - gamma * gamma);
}
