/**
 * @file
 *
 * PLY file format loading and saving routines optimized for just the
 * operations desired in mlsgpu.
 */

#ifndef FAST_PLY_H
#define FAST_PLY_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <string>
#include <cstddef>
#include <stdexcept>
#include <istream>
#include <fstream>
#include <ostream>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>
#include <tr1/cstdint>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/ref.hpp>
#include "errors.h"

class Splat;
class TestFastPlyReader;

namespace FastPly
{

enum WriterType
{
    MMAP_WRITER,
    STREAM_WRITER
};

/**
 * Wrapper around WriterType for use with @ref Choice.
 */
class WriterTypeWrapper
{
public:
    typedef WriterType type;
    static std::map<std::string, WriterType> getNameMap();
};

/**
 * An exception that is thrown when an invalid PLY file is encountered.
 * This is used to signal all errors in a PLY file (including early end-of-file),
 * except for low-level I/O errors in parsing the header.
 */
class FormatError : public std::runtime_error
{
public:
    FormatError(const std::string &msg) : std::runtime_error(msg) {}
};

/**
 * Class for quickly reading a subset of PLY files.
 * It only supports the following:
 * - Binary files, endianness matching the host.
 * - Only the "vertex" element is loaded.
 * - The "vertex" element must be the first element in the file.
 * - The x, y, z, nx, ny, nz, radius elements must all be present and FLOAT32.
 * - The vertex element must not contain any lists.
 * - It must be possible to mmap the entire file (thus, a 64-bit
 *   address space is needed to handle very large files).
 *
 * In addition to memory-mapping a file, it can also accept an existing
 * memory range (this is mainly provided to simplify testing).
 */
class Reader
{
    friend class ::TestFastPlyReader;
public:
    /// Size capable of holding maximum supported file size
    typedef boost::iostreams::mapped_file_source::size_type size_type;
    typedef Splat value_type;

    /**
     * Construct from a file.
     * @param filename         File to open.
     * @param smooth           Scale factor applied to radii as they're read.
     * @throw std::ios_base::failure if the file could not be opened
     * @throw FormatError if the file was malformed
     */
    explicit Reader(const std::string &filename, float smooth);

    /**
     * Construct from an existing memory range.
     * This is primarily intended for testing this class.
     * @param data             Start of memory region.
     * @param size             Bytes in memory region.
     * @param smooth           Scale factor applied to radii as they're read.
     * @throw FormatError if the file was malformed
     * @note The memory range must not be deleted or modified until the object
     * is destroyed.
     */
    Reader(const char *data, size_type size, float smooth);

    /// Number of vertices in the file
    size_type size() const { return vertexCount; }

    /**
     * Copy out a contiguous selection of the vertices.
     * @param first,last      %Range of vertices to copy.
     * @param out             Target of copy.
     * @return The output iterator after the copy.
     * @pre @a first &lt;= @a last &lt;= @ref size().
     */
    template<typename OutputIterator>
    OutputIterator read(size_type first, size_type last, OutputIterator out) const;
private:
    /// The memory mapping, if constructed from a filename; otherwise @c NULL.
    boost::scoped_ptr<boost::iostreams::mapped_file_source> mapping;

    /// Scale factor for radii
    float smooth;

    /// Pointer to the start of the whole file.
    const char *filePtr;
    /// Pointer to the first vertex.
    const char *vertexPtr;

    /// The properties found in the file.
    enum Property
    {
        X, Y, Z,
        NX, NY, NZ,
        RADIUS
    };
    static const unsigned int numProperties = 7;
    size_type vertexSize;              ///< Bytes per vertex
    size_type vertexCount;             ///< Number of vertices
    size_type offsets[numProperties];  ///< Byte offsets of each property within a vertex

    void readHeader(std::istream &in); ///< Does the heavy lifting of parsing the header

    /// Implementation of @ref read for the general case
    template<typename OutputIterator>
    OutputIterator read(size_type first, size_type last, OutputIterator out, boost::false_type) const;

    /// Implementation of @ref read for the special case of reading into raw memory
    value_type *read(size_type first, size_type last, value_type *out, boost::true_type) const;
};

/// Common code shared by @ref MmapWriter and @ref StreamWriter
class WriterBase
{
public:
    /// Size capable of holding maximum supported file size
    typedef std::tr1::uintmax_t size_type;

    virtual ~WriterBase();

    /**
     * Determines whether @ref open has been successfully called.
     */
    bool isOpen() const;

    /**
     * Add a comment to be written by @ref open.
     * @pre @ref open has not yet been successfully called.
     */
    void addComment(const std::string &comment);

    /**
     * Set the number of vertices that will be in the file.
     * @pre @ref open has not yet been successfully called.
     */
    void setNumVertices(size_type numVertices);

    /**
     * Set the number of indices that will be in the file.
     * @pre @ref open has not yet been successfully called.
     */
    void setNumTriangles(size_type numTriangles);

    /**
     * Create the file and write the header.
     * @pre @ref open has not yet been successfully called.
     */
    virtual void open(const std::string &filename) = 0;

    /**
     * Allocate storage in memory and write the header to it.
     * This version is primarily aimed at testing, to avoid
     * writing to file and reading back in.
     *
     * The memory is allocated with <code>new[]</code>, and
     * the caller is responsible for freeing it with <code>delete[]</code>.
     */
    virtual std::pair<char *, size_type> open() = 0;

    /**
     * Flush all data to the file and close it.
     *
     * After doing this, it is possible to open a new file, although the
     * comments will not be reset.
     */
    virtual void close() = 0;

    /**
     * Write a range of vertices.
     * @param first          Index of first vertex to write.
     * @param count          Number of vertices to write.
     * @param data           Array of <code>float[3]</code> values.
     * @pre @a first + @a count <= @a numVertices.
     */
    virtual void writeVertices(size_type first, size_type count, const float *data) = 0;

    /**
     * Write a range of triangles.
     * @param first          Index of first triangle to write.
     * @param count          Number of triangles to write.
     * @param data           Array of <code>uint32_t[3]</code> values containing indices.
     * @pre @a first + @a count <= @a numTriangles.
     */
    virtual void writeTriangles(size_type first, size_type count, const std::tr1::uint32_t *data) = 0;

    /**
     * Whether the class supports writing the data out-of-order.
     */
    virtual bool supportsOutOfOrder() const = 0;

protected:
    /// Bytes per vertex
    static const size_type vertexSize = 3 * sizeof(float);
    /// Bytes per triangle
    static const size_type triangleSize = 1 + 3 * sizeof(std::tr1::uint32_t);

    WriterBase();

    size_type getNumVertices() const;
    size_type getNumTriangles() const;

    /// Returns the header based on stored values
    std::string makeHeader();

    /// Sets the flag indicating whether the file is open
    void setOpen(bool open);

private:
    /// Storage for comments until they can be written by @ref open.
    std::vector<std::string> comments;
    size_type numVertices;              ///< Number of vertices (defaults to zero)
    size_type numTriangles;             ///< Number of triangles (defaults to zero)
    bool isOpen_;                       ///< Whether the file has been opened
};

/**
 * PLY file writer that only supports one format.
 * The supported format has:
 *  - Binary format with host endianness;
 *  - Vertices with x, y, z as 32-bit floats (no normals);
 *  - Faces with 32-bit unsigned integer indices;
 *  - 3 indices per face;
 *  - Arbitrary user-provided comments.
 * At present, the entire file is memory-mapped, which may significantly
 * limit the file size when using a 32-bit address space.
 *
 * Writing a file is done in phases:
 *  -# Set comments with @ref addComment and indicate the number of
 *     vertices and indices with @ref setNumVertices and @ref setNumTriangles.
 *  -# Write the header using @ref open.
 *  -# Use @ref writeVertices and @ref writeTriangles to write the data.
 *
 * The requirement for knowing the number of vertices and indices up front is a
 * limitation of the PLY format. If it is not possible to know this up front, you
 * will need to dump the vertices and indices to raw temporary files and stitch
 * it all together later.
 *
 * The final phase (writing of vertices and indices) is thread-safe, provided
 * that each thread is writing to a disjoint section of the file.
 *
 * @bug Due to the way Boost creates the file, it will have the executable bit
 * set on POSIX systems.
 */
class MmapWriter : public WriterBase
{
public:
    /// Constructor
    MmapWriter();

    virtual void open(const std::string &filename);
    virtual std::pair<char *, size_type> open();
    virtual void close();
    virtual void writeVertices(size_type first, size_type count, const float *data);
    virtual void writeTriangles(size_type first, size_type count, const std::tr1::uint32_t *data);
    virtual bool supportsOutOfOrder() const;

private:
    char *vertexPtr;                    /// Pointer to storage for the first vertex
    char *trianglePtr;                  /// Pointer to storage for the first triangle

    /**
     * The memory mapping backed by the output file. When using the in-memory
     * version, the pointer is NULL.
     */
    boost::scoped_ptr<boost::iostreams::mapped_file_sink> mapping;
};

/**
 * PLY file writer that only supports one format.
 * This class has exactly the same interface as @ref MmapWriter, and allows for
 * out-of-order writing. The advantage over @ref MmapWriter is that it does not
 * require a large virtual address space. However, it is potentially less
 * efficient.
 */
class StreamWriter : public WriterBase
{
public:
    /// Constructor
    StreamWriter() {}

    virtual void open(const std::string &filename);
    virtual std::pair<char *, size_type> open();
    virtual void close();
    virtual void writeVertices(size_type first, size_type count, const float *data);
    virtual void writeTriangles(size_type first, size_type count, const std::tr1::uint32_t *data);
    virtual bool supportsOutOfOrder() const;

private:
    /// Code shared by both @c open methods.
    void openCommon(const std::string &header);

    /**
     * Output stream. It is wrapped in a smart pointer because the type depends
     * on which open function was used.
     */
    boost::scoped_ptr<std::ostream> file;

    /// Position in file where vertices start
    boost::iostreams::stream_offset vertexOffset;
    /// Position in file where triangles start
    boost::iostreams::stream_offset triangleOffset;
};

/**
 * Factory function to create a new writer of the specified type.
 */
WriterBase *createWriter(WriterType type);


template<typename OutputIterator>
OutputIterator Reader::read(size_type first, size_type last, OutputIterator out, boost::false_type) const
{
    MLSGPU_ASSERT(first <= last && last <= size(), std::out_of_range);

    const size_type bufferSize = 8192;
    value_type buffer[bufferSize];
    while (first < last)
    {
        size_type size = std::min(bufferSize, last - first);
        read(first, first + size, buffer);
        out = std::copy(buffer, buffer + size, out);
        first += size;
    }
    return out;
}

template<typename OutputIterator>
OutputIterator Reader::read(size_type first, size_type last, OutputIterator out) const
{
    return read(first, last, out, boost::is_pointer<OutputIterator>());
}

} // namespace FastPly

#endif /* FAST_PLY_H */
