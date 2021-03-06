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
 * Utility functions only used in the main program.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <boost/program_options.hpp>
#include <boost/foreach.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/exception/all.hpp>
#include <boost/system/error_code.hpp>
#include <boost/filesystem.hpp>
#include <boost/ref.hpp>
#include <boost/thread/thread.hpp>
#include <memory>
#include <string>
#include <iterator>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cassert>
#include <limits>
#include "mlsgpu_core.h"
#include "options.h"
#include "mls.h"
#include "mesher.h"
#include "fast_ply.h"
#include "clh.h"
#include "statistics.h"
#include "statistics_cl.h"
#include "logging.h"
#include "provenance.h"
#include "marching.h"
#include "splat_tree_cl.h"
#include "workers.h"
#include "bucket.h"
#include "splat_set.h"
#include "decache.h"

namespace po = boost::program_options;

static void addCommonOptions(po::options_description &opts)
{
    opts.add_options()
        ("help,h",                "Show help")
        ("quiet,q",               "Do not show informational messages")
        (Option::debug,           "Show debug messages")
        (Option::responseFile,    po::value<std::string>(), "Read options from file")
        (Option::tmpDir,          po::value<std::string>(), "Directory to store temporary files");
}

static void addFitOptions(po::options_description &opts)
{
    opts.add_options()
        (Option::fitSmooth,       po::value<double>()->default_value(4.0),  "Smoothing factor")
        (Option::maxRadius,       po::value<double>(),                      "Limit influence radii")
        (Option::fitGrid,         po::value<double>()->default_value(0.01), "Spacing of grid cells")
        (Option::fitPrune,        po::value<double>()->default_value(0.02), "Minimum fraction of vertices per component")
        (Option::fitBoundaryLimit, po::value<double>()->default_value(1.0), "Tuning factor for boundary detection")
        (Option::fitShape,        po::value<Choice<MlsShapeWrapper> >()->default_value(MLS_SHAPE_SPHERE),
                                                                            "Model shape (sphere | plane)");
}

static void addStatisticsOptions(po::options_description &opts)
{
    po::options_description statistics("Statistics options");
    statistics.add_options()
        (Option::statistics,                          "Print information about internal statistics")
        (Option::statisticsFile, po::value<std::string>(), "Direct statistics to file instead of stdout (implies --statistics)")
        (Option::statisticsCL,                             "Collect timings for OpenCL commands")
        (Option::timeplot, po::value<std::string>(),       "Write timing data to file");
    opts.add(statistics);
}

static void addAdvancedOptions(po::options_description &opts)
{
    po::options_description advanced("Advanced options");
    advanced.add_options()
        (Option::levels,       po::value<int>()->default_value(6), "Levels in octree")
        (Option::subsampling,  po::value<int>()->default_value(3), "Subsampling of octree")
        (Option::maxSplit,     po::value<int>()->default_value(1024 * 1024 * 1024), "Maximum fan-out in partitioning")
        (Option::leafCells,    po::value<int>()->default_value(63), "Leaf size for initial histogram")
        (Option::deviceThreads, po::value<int>()->default_value(1), "Number of threads per device for submitting OpenCL work")
        (Option::reader,       po::value<Choice<ReaderTypeWrapper> >()->default_value(SYSCALL_READER), "File reader class (syscall | stream | mmap)")
        (Option::writer,       po::value<Choice<WriterTypeWrapper> >()->default_value(SYSCALL_WRITER), "File writer class (syscall | stream)")
#ifdef _OPENMP
        (Option::ompThreads,   po::value<int>(), "Number of threads for OpenMP")
#endif
        (Option::decache,      "Try to evict input files from OS cache for benchmarking")
        (Option::checkpoint,   po::value<std::string>(), "Checkpoint state prior to writing output")
        (Option::resume,       po::value<std::string>(), "Restart from checkpoint");
    opts.add(advanced);
}

static void addMemoryOptions(po::options_description &opts, bool isMPI)
{
    po::options_description memory("Advanced memory options");
    memory.add_options()
        (Option::memLoadSplats,   po::value<Capacity>()->default_value(256 * 1024 * 1024), "Memory for bucket merging")
        (Option::memHostSplats,   po::value<Capacity>()->default_value(512 * 1024 * 1024), "Memory for splats on the CPU")
        (Option::memBucketSplats, po::value<Capacity>()->default_value(64 * 1024 * 1024),  "Memory for splats in a single bucket")
        (Option::memMesh,         po::value<Capacity>()->default_value(512 * 1024 * 1024),  "Memory for raw mesh data on the CPU")
        (Option::memReorder,      po::value<Capacity>()->default_value(2U * 1024 * 1024 * 1024), "Memory for processed mesh data on the CPU");
    if (isMPI)
        memory.add_options()
            (Option::memGather,   po::value<Capacity>()->default_value(512 * 1024 * 1024),  "Memory for buffering raw mesh data on the slaves");
    opts.add(memory);
}

void usage(std::ostream &o, const po::options_description desc)
{
    o << "Usage: mlsgpu [options] -o output.ply input.ply [input.ply...]\n\n";
    o << desc;
}

po::variables_map processOptions(int argc, char **argv, bool isMPI)
{
    // TODO: replace cerr with thrown exception
    po::positional_options_description positional;
    positional.add(Option::inputFile, -1);

    po::options_description desc("General options");
    addCommonOptions(desc);
    addFitOptions(desc);
    addStatisticsOptions(desc);
    addAdvancedOptions(desc);
    addMemoryOptions(desc, isMPI);
    desc.add_options()
        ("output-file,o",   po::value<std::string>()->required(), "output file")
        (Option::split,     "split output across multiple files")
        (Option::splitSize, po::value<Capacity>()->default_value(100 * 1024 * 1024), "approximate size of output chunks");

    po::options_description clopts("OpenCL options");
    CLH::addOptions(clopts);
    desc.add(clopts);

    po::options_description hidden("Hidden options");
    hidden.add_options()
        (Option::inputFile, po::value<std::vector<std::string> >()->composing(), "input files");

    po::options_description all("All options");
    all.add(desc);
    all.add(hidden);

    try
    {
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv)
                  .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                  .options(all)
                  .positional(positional)
                  .run(), vm);
        if (vm.count(Option::responseFile))
        {
            const std::string &fname = vm[Option::responseFile].as<std::string>();
            std::ifstream in(fname.c_str());
            if (!in)
            {
                Log::log[Log::warn] << "Could not open `" << fname << "', ignoring\n";
            }
            else
            {
                std::vector<std::string> args;
                std::copy(std::istream_iterator<std::string>(in),
                          std::istream_iterator<std::string>(), std::back_inserter(args));
                if (in.bad())
                {
                    Log::log[Log::warn] << "Error while reading from `" << fname << "'\n";
                }
                in.close();
                po::store(po::command_line_parser(args)
                          .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
                          .options(all)
                          .positional(positional)
                          .run(), vm);
            }
        }

        po::notify(vm);

        if (vm.count(Option::help))
        {
            usage(std::cout, desc);
            std::exit(0);
        }
        /* Using ->required() on the option gives an unhelpful message */
        if (!vm.count(Option::inputFile))
        {
            std::cerr << "At least one input file must be specified.\n\n";
            usage(std::cerr, desc);
            std::exit(1);
        }

        if (vm.count(Option::statisticsCL))
        {
            Statistics::enableEventTiming();
        }
        if (vm.count(Option::tmpDir))
        {
            setTmpFileDir(vm[Option::tmpDir].as<std::string>());
        }

#ifdef _OPENMP
        int ompThreads;
        if (vm.count(Option::ompThreads))
            ompThreads = vm[Option::ompThreads].as<int>();
        else
        {
            // Subtract one to avoid starving reader/writer threads
            ompThreads = boost::thread::hardware_concurrency() - 1;
        }
        if (ompThreads <= 0)
            ompThreads = 1;
        omp_set_num_threads(ompThreads);
#endif

        return vm;
    }
    catch (po::error &e)
    {
        std::cerr << e.what() << "\n\n";
        usage(std::cerr, desc);
        std::exit(1);
    }
}

/**
 * Translate the command-line options back into the form they would be given
 * on the command line.
 */
static std::string makeOptions(const po::variables_map &vm)
{
    std::ostringstream opts;
    for (po::variables_map::const_iterator i = vm.begin(); i != vm.end(); ++i)
    {
        if (i->first == Option::inputFile)
            continue; // these are not output because some programs choke
        if (i->first == Option::responseFile)
            continue; // this is not relevant to reproducing the results
        const po::variable_value &param = i->second;
        const boost::any &value = param.value();
        if (param.empty()
            || (value.type() == typeid(std::string) && param.as<std::string>().empty()))
            opts << " --" << i->first;
        else if (value.type() == typeid(std::vector<std::string>))
        {
            BOOST_FOREACH(const std::string &j, param.as<std::vector<std::string> >())
            {
                opts << " --" << i->first << '=' << j;
            }
        }
        else
        {
            opts << " --" << i->first << '=';
            if (value.type() == typeid(std::string))
                opts << param.as<std::string>();
            else if (value.type() == typeid(double))
                opts << param.as<double>();
            else if (value.type() == typeid(int))
                opts << param.as<int>();
            else if (value.type() == typeid(unsigned int))
                opts << param.as<unsigned int>();
            else if (value.type() == typeid(std::size_t))
                opts << param.as<std::size_t>();
            else if (value.type() == typeid(Choice<MesherTypeWrapper>))
                opts << param.as<Choice<MesherTypeWrapper> >();
            else if (value.type() == typeid(Choice<WriterTypeWrapper>))
                opts << param.as<Choice<WriterTypeWrapper> >();
            else if (value.type() == typeid(Choice<ReaderTypeWrapper>))
                opts << param.as<Choice<ReaderTypeWrapper> >();
            else if (value.type() == typeid(Choice<MlsShapeWrapper>))
                opts << param.as<Choice<MlsShapeWrapper> >();
            else if (value.type() == typeid(Capacity))
                opts << param.as<Capacity>();
            else
                assert(!"Unhandled parameter type");
        }
    }
    return opts.str();
}

void writeStatistics(const po::variables_map &vm, bool force)
{
    if (force || vm.count(Option::statistics) || vm.count(Option::statisticsFile))
    {
        std::string name = "<stdout>";
        try
        {
            std::ostream *out;
            std::ofstream outf;
            if (vm.count(Option::statisticsFile))
            {
                name = vm[Option::statisticsFile].as<std::string>();
                outf.open(name.c_str());
                out = &outf;
            }
            else
            {
                out = &std::cout;
            }

            boost::io::ios_exception_saver saver(*out);
            out->exceptions(std::ios::failbit | std::ios::badbit);
            *out << "mlsgpu version: " << provenanceVersion() << '\n';
            *out << "mlsgpu variant: " << provenanceVariant() << '\n';
            *out << "mlsgpu options:" << makeOptions(vm) << '\n';
            {
                boost::io::ios_precision_saver saver2(*out);
                out->precision(15);
                *out << Statistics::Registry::getInstance();
            }
        }
        catch (std::ios::failure &e)
        {
            throw boost::enable_error_info(e)
                << boost::errinfo_file_name(name)
                << boost::errinfo_errno(errno);
        }
    }
}

static std::size_t getDeviceWorkerGroupSpare(const po::variables_map &vm)
{
    (void) vm;
    return 1;
}

static std::size_t getMeshMemoryCells(const po::variables_map &vm)
{
    const int levels = vm[Option::levels].as<int>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const std::size_t maxCells = (Grid::size_type(1U) << (levels + subsampling - 1)) - 1;
    return maxCells * maxCells * 2;
}

static std::size_t getMeshMemory(const po::variables_map &vm)
{
    return getMeshMemoryCells(vm) * Marching::MAX_CELL_BYTES;
}

static std::size_t getMeshHostMemory(const po::variables_map &vm)
{
    const std::size_t scale =
        Marching::MAX_CELL_VERTICES * (3 * sizeof(cl_float) + sizeof(cl_ulong))
        + Marching::MAX_CELL_INDICES * sizeof(cl_uint);
    return getMeshMemoryCells(vm) * scale;
}

static std::size_t getMaxHostSplats(const po::variables_map &vm)
{
    std::size_t mem = vm[Option::memHostSplats].as<Capacity>();
    return mem / sizeof(Splat);
}

std::size_t getMaxLoadSplats(const po::variables_map &vm)
{
    std::size_t mem = vm[Option::memLoadSplats].as<Capacity>();
    return mem / sizeof(Splat);
}

static std::size_t getMaxBucketSplats(const po::variables_map &vm)
{
    std::size_t mem = vm[Option::memBucketSplats].as<Capacity>();
    return mem / sizeof(Splat);
}

void validateOptions(const po::variables_map &vm, bool isMPI)
{
    const int levels = vm[Option::levels].as<int>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const std::size_t maxBucketSplats = getMaxBucketSplats(vm);
    const std::size_t maxLoadSplats = getMaxLoadSplats(vm);
    const std::size_t maxHostSplats = getMaxHostSplats(vm);
    const std::size_t maxSplit = vm[Option::maxSplit].as<int>();
    const int deviceThreads = vm[Option::deviceThreads].as<int>();
    const double pruneThreshold = vm[Option::fitPrune].as<double>();

    const std::size_t memMesh = vm[Option::memMesh].as<Capacity>();

    int maxLevels = std::min(
            std::size_t(Marching::MAX_DIMENSION_LOG2 + 1),
            std::size_t(SplatTreeCL::MAX_LEVELS));
    if (levels < 1 || levels > maxLevels)
    {
        std::ostringstream msg;
        msg << "Value of --levels must be in the range 1 to " << maxLevels;
        throw invalid_option(msg.str());
    }
    if (subsampling < MlsFunctor::subsamplingMin)
    {
        std::ostringstream msg;
        msg << "Value of --subsampling must be at least " << MlsFunctor::subsamplingMin;
        throw invalid_option(msg.str());
    }
    if (maxBucketSplats < 1)
        throw invalid_option(std::string("Value of --") + Option::memBucketSplats + " must be positive");
    if (maxLoadSplats < maxBucketSplats)
        throw invalid_option(std::string("Value of --") + Option::memLoadSplats
                             + " must be at least that of --" + Option::memBucketSplats);
    if (maxHostSplats < maxBucketSplats)
        throw invalid_option(std::string("Value of --") + Option::memHostSplats
                             + " must be at least that of --" + Option::memBucketSplats);
    if (maxSplit < 8)
        throw invalid_option(std::string("Value of --") + Option::maxSplit + " must be at least 8");
    if (subsampling > Marching::MAX_DIMENSION_LOG2 + 1 - levels)
        throw invalid_option(std::string("Sum of --") + Option::subsampling
                             + " and --" + Option::levels + " is too large");
    const std::size_t treeVerts = std::size_t(1) << (subsampling + levels - 1);
    if (treeVerts < MlsFunctor::wgs[0] || treeVerts < MlsFunctor::wgs[1])
        throw invalid_option(std::string("Sum of --") + Option::subsampling
                             + " and --" + Option::levels + " is too small");

    if (deviceThreads < 1)
        throw invalid_option(std::string("Value of --") + Option::deviceThreads + " must be at least 1");
    if (!(pruneThreshold >= 0.0 && pruneThreshold <= 1.0))
        throw invalid_option(std::string("Value of --") + Option::fitPrune + " must be in [0, 1]");

    if (memMesh < getMeshHostMemory(vm))
        throw invalid_option(std::string("Value of --") + Option::memMesh + " is too small");
    if (isMPI)
    {
        const std::size_t memGather = vm[Option::memGather].as<Capacity>();
        if (memGather < getMeshHostMemory(vm))
            throw invalid_option(std::string("Value of --") + Option::memGather + " is too small");
    }
}

void setLogLevel(const po::variables_map &vm)
{
    if (vm.count(Option::quiet))
        Log::log.setLevel(Log::warn);
    else if (vm.count(Option::debug))
        Log::log.setLevel(Log::debug);
    else
        Log::log.setLevel(Log::info);
}

CLH::ResourceUsage resourceUsage(const po::variables_map &vm)
{
    const int levels = vm[Option::levels].as<int>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const std::size_t maxBucketSplats = getMaxBucketSplats(vm);
    const int deviceThreads = vm[Option::deviceThreads].as<int>();
    const int deviceSpare = getDeviceWorkerGroupSpare(vm);

    const Grid::size_type maxCells = (Grid::size_type(1U) << (levels + subsampling - 1)) - 1;
    // TODO: get rid of device parameter
    CLH::ResourceUsage totalUsage = DeviceWorkerGroup::resourceUsage(
        deviceThreads, deviceSpare, cl::Device(),
        maxBucketSplats, maxCells,
        getMeshMemory(vm), levels);
    return totalUsage;
}

void validateDevice(const cl::Device &device, const CLH::ResourceUsage &totalUsage)
{
    const std::string deviceName = "OpenCL device `" + device.getInfo<CL_DEVICE_NAME>() + "'";
    Marching::validateDevice(device);
    SplatTreeCL::validateDevice(device);

    /* Check that we have enough memory on the device. This is no guarantee against OOM, but
     * we can at least turn down silly requests before wasting any time.
     */
    const std::size_t deviceTotalMemory = device.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>();
    const std::size_t deviceMaxMemory = device.getInfo<CL_DEVICE_MAX_MEM_ALLOC_SIZE>();
    if (totalUsage.getMaxMemory() > deviceMaxMemory)
    {
        std::ostringstream msg;
        msg << "Arguments require an allocation of " << totalUsage.getMaxMemory() << ",\n"
            << "but only " << deviceMaxMemory << " is supported.\n"
            << "Try reducing --levels or --mem-device-splats, or increasing --subsampling.";
        throw CLH::invalid_device(device, msg.str());
    }
    if (totalUsage.getTotalMemory() > deviceTotalMemory)
    {
        std::ostringstream msg;
        msg << "Arguments require device memory of " << totalUsage.getTotalMemory() << ",\n"
            << "but only " << deviceTotalMemory << " available.\n"
            << "Try reducing --levels or --mem-device-splats, or increasing --subsampling.";
        throw CLH::invalid_device(device, msg.str());
    }

    if (totalUsage.getTotalMemory() > deviceTotalMemory * 0.8)
    {
        Log::log[Log::warn] << "WARNING: More than 80% of the memory on " << deviceName << " will be used.\n";
    }
}

void prepareInputs(SplatSet::FileSet &files, const po::variables_map &vm, float smooth, float maxRadius)
{
    const std::vector<std::string> &names = vm[Option::inputFile].as<std::vector<std::string> >();
    std::vector<boost::filesystem::path> paths;
    BOOST_FOREACH(const std::string &name, names)
    {
        boost::filesystem::path base(name);
        if (is_directory(base))
        {
            boost::filesystem::directory_iterator it(base);
            while (it != boost::filesystem::directory_iterator())
            {
                if (it->path().extension() == ".ply" && !is_directory(it->status()))
                    paths.push_back(it->path());
                ++it;
            }
        }
        else
            paths.push_back(name);
    }

    const ReaderType readerType = vm[Option::reader].as<Choice<ReaderTypeWrapper> >();
    if (paths.size() > SplatSet::FileSet::maxFiles)
    {
        std::ostringstream msg;
        msg << "Too many input files (" << paths.size() << " > " << SplatSet::FileSet::maxFiles << ")";
        throw std::runtime_error(msg.str());
    }
    std::tr1::uint64_t totalSplats = 0;
    std::tr1::uint64_t totalBytes = 0;
    BOOST_FOREACH(const boost::filesystem::path &path, paths)
    {
        if (vm.count(Option::decache))
            decache(path.string());
        std::auto_ptr<FastPly::Reader> reader(new FastPly::Reader(readerType, path.string(), smooth, maxRadius));
        if (reader->size() > SplatSet::FileSet::maxFileSplats)
        {
            std::ostringstream msg;
            msg << "Too many samples in " << path << " ("
                << reader->size() << " > " << SplatSet::FileSet::maxFileSplats << ")";
            throw std::runtime_error(msg.str());
        }
        totalSplats += reader->size();
        totalBytes += reader->size() * reader->getVertexSize();
        files.addFile(reader.get());
        reader.release();
    }

    Statistics::getStatistic<Statistics::Counter>("files.scans").add(paths.size());
    Statistics::getStatistic<Statistics::Counter>("files.splats").add(totalSplats);
    Statistics::getStatistic<Statistics::Counter>("files.bytes").add(totalBytes);
}

void reportException(std::exception &e)
{
    std::cerr << '\n';

    std::string *file_name = boost::get_error_info<boost::errinfo_file_name>(e);
    int *err = boost::get_error_info<boost::errinfo_errno>(e);
    if (file_name != NULL)
        std::cerr << *file_name << ": ";
    if (err != NULL && *err != 0)
        std::cerr << boost::system::errc::make_error_code((boost::system::errc::errc_t) *err).message() << std::endl;
    else
        std::cerr << e.what() << std::endl;
}

void doComputeBlobs(
    Timeplot::Worker &tworker,
    const po::variables_map &vm,
    SplatSet::FileSet &splats,
    boost::function<void(float, unsigned int)> computeBlobs)
{
    const float spacing = vm[Option::fitGrid].as<double>();
    const float smooth = vm[Option::fitSmooth].as<double>();
    const float maxRadius = vm.count(Option::maxRadius)
        ? vm[Option::maxRadius].as<double>() : std::numeric_limits<float>::infinity();

    const int subsampling = vm[Option::subsampling].as<int>();
    const int levels = vm[Option::levels].as<int>();
    const unsigned int leafCells = vm[Option::leafCells].as<int>();
    const unsigned int block = 1U << (levels + subsampling - 1);
    const unsigned int blockCells = block - 1;
    const unsigned int microCells = std::min(leafCells, blockCells);

    prepareInputs(splats, vm, smooth, maxRadius);
    try
    {
        Timeplot::Action timer("bbox", tworker, "bbox.time");
        computeBlobs(spacing, microCells);
    }
    catch (std::length_error &e) // TODO: push this down to splat_set_impl
    {
        throw std::runtime_error("At least one input point is required");
    }
}

unsigned int postprocessGrid(const po::variables_map &vm, const Grid &grid)
{
    for (unsigned int i = 0; i < 3; i++)
    {
        double size = grid.numCells(i) * grid.getSpacing();
        Statistics::getStatistic<Statistics::Variable>(std::string("bbox") + "XYZ"[i]).add(size);
        if (grid.numVertices(i) > Marching::MAX_GLOBAL_DIMENSION)
        {
            std::ostringstream msg;
            msg << "The bounding box is too big (" << grid.numVertices(i) << " grid units).\n"
                << "Perhaps you have used the wrong units for --fit-grid?";
            throw std::runtime_error(msg.str());
        }
    }

    const bool split = vm.count(Option::split);
    const unsigned int splitSize = vm[Option::splitSize].as<Capacity>();
    unsigned int chunkCells = 0;
    if (split)
    {
        /* Determine a chunk size from splitSize. We assume that a chunk will be
         * sliced by an axis-aligned plane. This plane will cut each vertical and
         * each diagonal edge ones, thus generating 2x^2 vertices. We then
         * apply a fudge factor of 10 to account for the fact that the real
         * world is not a simple plane, and will have walls, noise, etc, giving
         * 20x^2 vertices.
         *
         * A manifold with genus 0 has two triangles per vertex; vertices take
         * 12 bytes (3 floats) and triangles take 13 (count plus 3 uints in
         * PLY), giving 38 bytes per vertex. So there are 760x^2 bytes.
         *
         * TODO: move this function to mlsgpu_core.
         */
        chunkCells = (unsigned int) ceil(sqrt(splitSize / 760.0));
        if (chunkCells == 0) chunkCells = 1;
    }
    return chunkCells;
}

void doBucket(
    Timeplot::Worker &tworker,
    const po::variables_map &vm,
    const SplatSet::FastBlobSet<SplatSet::FileSet> &splats,
    const Grid &grid,
    Grid::size_type chunkCells,
    BucketCollector &collector)
{
    Timeplot::Action bucketTimer("compute", tworker, "bucket.compute");

    const std::size_t maxBucketSplats = getMaxBucketSplats(vm);
    const std::size_t maxSplit = vm[Option::maxSplit].as<int>();
    const int subsampling = vm[Option::subsampling].as<int>();
    const int levels = vm[Option::levels].as<int>();
    const unsigned int leafCells = vm[Option::leafCells].as<int>();

    const unsigned int block = 1U << (levels + subsampling - 1);
    const unsigned int blockCells = block - 1;
    const unsigned int microCells = std::min(leafCells, blockCells);

    Bucket::bucket(splats, grid, maxBucketSplats, blockCells, chunkCells, microCells, maxSplit,
                   boost::ref(collector));
}

void setWriterComments(const po::variables_map &vm, FastPly::Writer &writer)
{
    writer.addComment("mlsgpu version: " + provenanceVersion());
    writer.addComment("mlsgpu variant: " + provenanceVariant());
    writer.addComment("mlsgpu options:" + makeOptions(vm));
}

MesherBase::Namer getNamer(const po::variables_map &vm, const std::string &out)
{
    const bool split = vm.count(Option::split);
    if (split)
        return ChunkNamer(out);
    else
        return TrivialNamer(out);
}

void setMesherOptions(const po::variables_map &vm, MesherBase &mesher)
{
    const double pruneThreshold = vm[Option::fitPrune].as<double>();
    const std::size_t memReorder = vm[Option::memReorder].as<Capacity>();
    mesher.setPruneThreshold(pruneThreshold);
    mesher.setReorderCapacity(memReorder);
}

SlaveWorkers::SlaveWorkers(
    Timeplot::Worker &tworker,
    const po::variables_map &vm,
    const std::vector<std::pair<cl::Context, cl::Device> > &devices,
    const DeviceWorkerGroup::OutputGenerator &outputGenerator)
    : tworker(tworker)
{
    const int subsampling = vm[Option::subsampling].as<int>();
    const int levels = vm[Option::levels].as<int>();
    const unsigned int numDeviceThreads = vm[Option::deviceThreads].as<int>();
    const float boundaryLimit = vm[Option::fitBoundaryLimit].as<double>();
    const MlsShape shape = vm[Option::fitShape].as<Choice<MlsShapeWrapper> >();
    const std::size_t deviceSpare = getDeviceWorkerGroupSpare(vm);

    const std::size_t maxBucketSplats = getMaxBucketSplats(vm);
    const std::size_t maxLoadSplats = getMaxLoadSplats(vm);
    const std::size_t maxHostSplats = getMaxHostSplats(vm);

    const unsigned int block = 1U << (levels + subsampling - 1);
    const unsigned int blockCells = block - 1;

    std::vector<DeviceWorkerGroup *> deviceWorkerGroupPtrs;
    for (std::size_t i = 0; i < devices.size(); i++)
    {
        DeviceWorkerGroup *dwg = new DeviceWorkerGroup(
            numDeviceThreads, deviceSpare,
            outputGenerator,
            devices[i].first, devices[i].second,
            maxBucketSplats, blockCells,
            getMeshMemory(vm),
            levels, subsampling,
            boundaryLimit, shape);
        deviceWorkerGroups.push_back(dwg);
        deviceWorkerGroupPtrs.push_back(dwg);
    }
    copyGroup.reset(new CopyGroup(deviceWorkerGroupPtrs, maxHostSplats));
    loader.reset(new BucketLoader(maxLoadSplats, *copyGroup, tworker));
}

void SlaveWorkers::start(SplatSet::FileSet &splats, const Grid &grid, ProgressMeter *progress)
{
    for (std::size_t i = 0; i < deviceWorkerGroups.size(); i++)
        deviceWorkerGroups[i].setProgress(progress);

    loader->start(splats, grid);
    copyGroup->start();
    for (std::size_t i = 0; i < deviceWorkerGroups.size(); i++)
        deviceWorkerGroups[i].start(grid);
}

void SlaveWorkers::stop()
{
    copyGroup->stop();
    for (std::size_t i = 0; i < deviceWorkerGroups.size(); i++)
        deviceWorkerGroups[i].stop();
}
