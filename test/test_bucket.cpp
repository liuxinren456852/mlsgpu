/**
 * @file
 *
 * Test code for @ref bucket.cpp.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <boost/bind.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/smart_ptr/shared_array.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/foreach.hpp>
#include <boost/ref.hpp>
#include <string>
#include <vector>
#include <iterator>
#include <algorithm>
#include <utility>
#include <limits>
#include <sstream>
#include <cstring>
#include <tr1/cstdint>
#include "testmain.h"
#include "../src/bucket.h"
#include "../src/bucket_internal.h"

using namespace std;
using namespace Bucket;
using namespace Bucket::internal;

/**
 * Create a splat with given position and radius. The other fields
 * are given arbitrary values.
 */
static Splat makeSplat(float x, float y, float z, float radius)
{
    Splat splat;
    splat.position[0] = x;
    splat.position[1] = y;
    splat.position[2] = z;
    splat.radius = radius;
    splat.normal[0] = 1.0f;
    splat.normal[1] = 0.0f;
    splat.normal[2] = 0.0f;
    return splat;
}

static bool gridsIntersect(const Grid &a, const Grid &b)
{
    for (int i = 0; i < 3; i++)
    {
        if (a.getExtent(i).second <= b.getExtent(i).first
            || b.getExtent(i).second <= a.getExtent(i).first)
            return false;
    }
    return true;
}

/**
 * Tests for Range.
 */
class TestRange : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRange);
    CPPUNIT_TEST(testConstructor);
    CPPUNIT_TEST(testAppendEmpty);
    CPPUNIT_TEST(testAppendOverflow);
    CPPUNIT_TEST(testAppendMiddle);
    CPPUNIT_TEST(testAppendEnd);
    CPPUNIT_TEST(testAppendGap);
    CPPUNIT_TEST(testAppendNewScan);
    CPPUNIT_TEST_SUITE_END();

public:
    void testConstructor();          ///< Test the constructors
    void testAppendEmpty();          ///< Appending to an empty range
    void testAppendOverflow();       ///< An append that would overflow the size
    void testAppendMiddle();         ///< Append to the middle of an existing range
    void testAppendEnd();            ///< Extend a range
    void testAppendGap();            ///< Append outside (and not adjacent) to an existing range
    void testAppendNewScan();        ///< Append with a different scan
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRange, TestSet::perBuild());

void TestRange::testConstructor()
{
    Range empty;
    Range single(3, 6);
    Range range(2, UINT64_C(0xFFFFFFFFFFFFFFF0), 0x10);

    CPPUNIT_ASSERT_EQUAL(Range::size_type(0), empty.size);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), single.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(6), single.start);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), single.size);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(2), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0xFFFFFFFFFFFFFFF0)), range.start);

    CPPUNIT_ASSERT_THROW(Range(2, UINT64_C(0xFFFFFFFFFFFFFFF0), 0x11), std::out_of_range);
}

void TestRange::testAppendEmpty()
{
    Range range;
    bool success;

    success = range.append(3, 6);
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(6), range.start);
}

void TestRange::testAppendOverflow()
{
    Range range;
    range.scan = 3;
    range.start = 0x90000000U;
    range.size = 0xFFFFFFFFU;
    bool success = range.append(3, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0xFFFFFFFFU), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0x90000000U), range.start);
}

void TestRange::testAppendMiddle()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, UINT64_C(0x12345678FFFF));
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendEnd()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10001), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendGap()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, range.start + range.size + 1);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendNewScan()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(5, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}


/**
 * Tests for @ref Bucket::internal::RangeCollector.
 */
class TestRangeCollector : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRangeCollector);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testFlush);
    CPPUNIT_TEST(testFlushEmpty);
    CPPUNIT_TEST_SUITE_END();

public:
    void testSimple();            ///< Test basic functionality
    void testFlush();             ///< Test flushing and continuing
    void testFlushEmpty();        ///< Test flushing when nothing to flush
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRangeCollector, TestSet::perBuild());

void TestRangeCollector::testSimple()
{
    vector<Range> out;

    {
        RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));
        c.append(3, 5);
        c.append(3, 6);
        c.append(3, 6);
        c.append(4, UINT64_C(0x123456781234));
        c.append(5, 2);
        c.append(5, 4);
        c.append(5, 5);
    }
    CPPUNIT_ASSERT_EQUAL(4, int(out.size()));

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), out[1].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(5), out[2].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[2].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(2), out[2].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(5), out[3].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[3].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(4), out[3].start);
}

void TestRangeCollector::testFlush()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));

    c.append(3, 5);
    c.append(3, 6);
    c.flush();

    CPPUNIT_ASSERT_EQUAL(1, int(out.size()));
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    c.append(3, 7);
    c.append(4, 0);
    c.flush();

    CPPUNIT_ASSERT_EQUAL(3, int(out.size()));

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(7), out[1].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), out[2].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[2].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0), out[2].start);
}

void TestRangeCollector::testFlushEmpty()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));
    c.flush();
    CPPUNIT_ASSERT_EQUAL(0, int(out.size()));
}


/**
 * Slow tests for Bucket::internal::RangeCollector.
 * These tests are designed to catch overflow conditions and hence necessarily
 * involve running O(2^32) operations. They are thus nightly rather than
 * per-build tests.
 */
class TestRangeBig : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRangeBig);
    CPPUNIT_TEST(testBigRange);
    CPPUNIT_TEST_SUITE_END();
public:
    void testBigRange();             ///< Throw more than 2^32 contiguous elements into a range
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRangeBig, TestSet::perNightly());

void TestRangeBig::testBigRange()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));

    for (std::tr1::uint64_t i = 0; i < UINT64_C(0x123456789); i++)
    {
        c.append(0, i);
    }
    c.flush();

    CPPUNIT_ASSERT_EQUAL(2, int(out.size()));

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(0), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0xFFFFFFFFu), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(0), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x2345678Au), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0xFFFFFFFFu), out[1].start);
}

std::ostream &operator<<(std::ostream &o, const Node &node)
{
    return o << "Node("
        << node.getCoords()[0] << ", "
        << node.getCoords()[1] << ", "
        << node.getCoords()[2] << ", " << node.getLevel() << ")";
}

/// Tests for @ref Bucket::internal::Node
class TestNode : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestNode);
    CPPUNIT_TEST(testConstructor);
    CPPUNIT_TEST(testChild);
    CPPUNIT_TEST(testToCells);
    CPPUNIT_TEST(testToMicro);
    CPPUNIT_TEST(testSize);
    CPPUNIT_TEST_SUITE_END();
public:
    void testConstructor();            ///< Test constructor
    void testChild();                  ///< Test @c child
    void testToCells();                ///< Test @c toCells (both overloads)
    void testToMicro();                ///< Test @c toMicro (both overloads)
    void testSize();                   ///< Test @c size
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestNode, TestSet::perBuild());

void TestNode::testConstructor()
{
    Node n(1, 2, 3, 4);
    const boost::array<Node::size_type, 3> &coords = n.getCoords();
    unsigned int level = n.getLevel();
    CPPUNIT_ASSERT_EQUAL(Node::size_type(1), coords[0]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(2), coords[1]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(3), coords[2]);
    CPPUNIT_ASSERT_EQUAL(4U, level);

    Node n2(&coords[0], 4);
    CPPUNIT_ASSERT_EQUAL(n, n2);
}

void TestNode::testChild()
{
    Node parent(1, 2, 3, 4);
    CPPUNIT_ASSERT_EQUAL(Node(2, 4, 6, 3), parent.child(0));
    CPPUNIT_ASSERT_EQUAL(Node(3, 4, 6, 3), parent.child(1));
    CPPUNIT_ASSERT_EQUAL(Node(2, 5, 6, 3), parent.child(2));
    CPPUNIT_ASSERT_EQUAL(Node(3, 5, 6, 3), parent.child(3));
    CPPUNIT_ASSERT_EQUAL(Node(2, 4, 7, 3), parent.child(4));
    CPPUNIT_ASSERT_EQUAL(Node(3, 4, 7, 3), parent.child(5));
    CPPUNIT_ASSERT_EQUAL(Node(2, 5, 7, 3), parent.child(6));
    CPPUNIT_ASSERT_EQUAL(Node(3, 5, 7, 3), parent.child(7));

    CPPUNIT_ASSERT_THROW(Node(1, 2, 3, 0).child(0), std::invalid_argument);
    CPPUNIT_ASSERT_THROW(Node(1, 2, 3, 1).child(8), std::invalid_argument);
}

void TestNode::testToCells()
{
    Node n(1, 2, 3, 2);
    Grid::size_type lower[3], upper[3];
    n.toCells(10, lower, upper);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 40), lower[0]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 80), lower[1]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type(120), lower[2]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 80), upper[0]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type(120), upper[1]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type(160), upper[2]);

    const float ref[3] = {0.0f, 0.0f, 0.0f};
    Grid limit(ref, 3.0f, 1000, 1075, 1000, 1075, 1000, 2000);
    n.toCells(10, lower, upper, limit);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 40), lower[0]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 75), lower[1]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type(120), lower[2]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 75), upper[0]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type( 75), upper[1]);
    CPPUNIT_ASSERT_EQUAL(Grid::size_type(160), upper[2]);
}

void TestNode::testToMicro()
{
    Node n(1, 2, 3, 2);
    Node::size_type lower[3], upper[3];
    n.toMicro(lower, upper);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 4), lower[0]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 8), lower[1]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(12), lower[2]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 8), upper[0]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(12), upper[1]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(16), upper[2]);

    Node::size_type limit[3] = {7, 7, 200};
    n.toMicro(lower, upper, limit);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 4), lower[0]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 7), lower[1]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(12), lower[2]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 7), upper[0]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type( 7), upper[1]);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(16), upper[2]);
}

void TestNode::testSize()
{
    Node n(1, 2, 3, 4);
    CPPUNIT_ASSERT_EQUAL(Node::size_type(16), n.size());
}


/// Tests for @ref Bucket::internal::forEachNode.
class TestForEachNode : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestForEachNode);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testAsserts);
    CPPUNIT_TEST_SUITE_END();

private:
    vector<Node> nodes;
    bool nodeFunc(const Node &node);

public:
    void testSimple();          ///< Test normal usage
    void testAsserts();         ///< Test the assertions of preconditions
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestForEachNode, TestSet::perBuild());

bool TestForEachNode::nodeFunc(const Node &node)
{
    nodes.push_back(node);

    Node::size_type lower[3], upper[3];
    node.toMicro(lower, upper);
    return (lower[0] <= 2 && 2 < upper[0]
        && lower[1] <= 1 && 1 < upper[1]
        && lower[2] <= 4 && 4 < upper[2]);
}

void TestForEachNode::testSimple()
{
    const Node::size_type dims[3] = {4, 4, 6};
    forEachNode(dims, 4, boost::bind(&TestForEachNode::nodeFunc, this, _1));
    /* Note: the recursion order of forEachNode is not defined, so this
     * test is constraining the implementation. It should be changed
     * if necessary.
     */
    CPPUNIT_ASSERT_EQUAL(15, int(nodes.size()));
    CPPUNIT_ASSERT_EQUAL(Node(0, 0, 0, 3), nodes[0]);
    CPPUNIT_ASSERT_EQUAL(Node(0, 0, 0, 2), nodes[1]);
    CPPUNIT_ASSERT_EQUAL(Node(0, 0, 1, 2), nodes[2]);
    CPPUNIT_ASSERT_EQUAL(Node(0, 0, 2, 1), nodes[3]);
    CPPUNIT_ASSERT_EQUAL(Node(1, 0, 2, 1), nodes[4]);
    CPPUNIT_ASSERT_EQUAL(Node(2, 0, 4, 0), nodes[5]);
    CPPUNIT_ASSERT_EQUAL(Node(3, 0, 4, 0), nodes[6]);
    CPPUNIT_ASSERT_EQUAL(Node(2, 1, 4, 0), nodes[7]);
    CPPUNIT_ASSERT_EQUAL(Node(3, 1, 4, 0), nodes[8]);
    CPPUNIT_ASSERT_EQUAL(Node(2, 0, 5, 0), nodes[9]);
    CPPUNIT_ASSERT_EQUAL(Node(3, 0, 5, 0), nodes[10]);
    CPPUNIT_ASSERT_EQUAL(Node(2, 1, 5, 0), nodes[11]);
    CPPUNIT_ASSERT_EQUAL(Node(3, 1, 5, 0), nodes[12]);
    CPPUNIT_ASSERT_EQUAL(Node(0, 1, 2, 1), nodes[13]);
    CPPUNIT_ASSERT_EQUAL(Node(1, 1, 2, 1), nodes[14]);
}

// Not expected to ever be called - just to give a legal function pointer
static bool dummyNodeFunc(const Node &node)
{
    (void) node;
    return false;
}

void TestForEachNode::testAsserts()
{
    const Node::size_type dims[3] = {4, 4, 6};
    CPPUNIT_ASSERT_THROW(forEachNode(dims, 100, dummyNodeFunc), std::invalid_argument);
    CPPUNIT_ASSERT_THROW(forEachNode(dims, 0, dummyNodeFunc), std::invalid_argument);
    CPPUNIT_ASSERT_THROW(forEachNode(dims, 3, dummyNodeFunc), std::invalid_argument);
}

/**
 * Test code for @ref Bucket::internal::forEachSplat.
 */
class TestForEachSplat : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestForEachSplat);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST_SUITE_END();
private:
    typedef pair<Range::scan_type, Range::index_type> Id;
    typedef StdVectorCollection<Splat> Collection;

    std::vector<std::vector<Splat> > splatData;
    boost::ptr_vector<Collection> splats;

    void splatFunc(Range::scan_type scan, Range::index_type id, const Splat &splat, vector<Id> &out);
public:
    virtual void setUp();

    void testSimple();
    void testEmpty();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestForEachSplat, TestSet::perBuild());

void TestForEachSplat::splatFunc(Range::scan_type scan, Range::index_type id,
                                 const Splat &splat, vector<Id> &out)
{
    // Check that the ID information we're given matches what we encoded into the splats
    CPPUNIT_ASSERT_EQUAL(scan, Range::scan_type(splat.position[0]));
    CPPUNIT_ASSERT_EQUAL(id, Range::index_type(splat.position[1]));

    out.push_back(Id(scan, id));
}

void TestForEachSplat::setUp()
{
    CppUnit::TestFixture::setUp();
    int size = 100000;
    int nFiles = 5;

    splats.reserve(nFiles);
    splatData.resize(nFiles);
    for (int i = 0; i < nFiles; i++)
    {
        splatData[i].resize(size);
        for (int j = 0; j < size; j++)
            splatData[i][j] = makeSplat(i, j, 0.0f, 1.0f);
        splats.push_back(new Collection(splatData[i]));
    }
}

void TestForEachSplat::testSimple()
{
    vector<Id> expected, actual;
    vector<Range> ranges;

    ranges.push_back(Range(0, 0));
    ranges.push_back(Range(0, 2, 3));
    ranges.push_back(Range(1, 2, 3));
    ranges.push_back(Range(2, 100, 40000)); // Large range to test buffering

    BOOST_FOREACH(const Range &range, ranges)
    {
        for (Range::index_type i = 0; i < range.size; ++i)
        {
            expected.push_back(Id(range.scan, range.start + i));
        }
    }

    forEachSplat(splats, ranges.begin(), ranges.end(),
                 boost::bind(&TestForEachSplat::splatFunc, this, _1, _2, _3, boost::ref(actual)));
    CPPUNIT_ASSERT_EQUAL(expected.size(), actual.size());
    for (size_t i = 0; i < actual.size(); i++)
    {
        CPPUNIT_ASSERT_EQUAL(expected[i].first, actual[i].first);
        CPPUNIT_ASSERT_EQUAL(expected[i].second, actual[i].second);
    }
}

void TestForEachSplat::testEmpty()
{
    vector<Range> ranges;
    vector<Id> actual;

    forEachSplat(splats, ranges.begin(), ranges.end(),
                 boost::bind(&TestForEachSplat::splatFunc, this, _1, _2, _3, boost::ref(actual)));
    CPPUNIT_ASSERT(actual.empty());
}

/// Test for @ref Bucket::bucket.
class TestBucket : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestBucket);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testDensityError);
    CPPUNIT_TEST(testMultiLevel);
    CPPUNIT_TEST(testFlat);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST_SUITE_END();

private:
    struct Block
    {
        Grid grid;
        Range::index_type numSplats;
        vector<Range> ranges;
        std::tr1::uint64_t done;
    };

    typedef StdVectorCollection<Splat> Collection;
    std::vector<std::vector<Splat> > splatData;
    boost::ptr_vector<Collection> splats;

    void setupSimple();

    void validate(const boost::ptr_vector<Collection> &splats, const Grid &fullGrid,
                  const vector<Block> &blocks, std::size_t maxSplats, Grid::size_type maxCells);

    static void bucketFunc(
        vector<Block> &blocks,
        const boost::ptr_vector<Collection> &splats,
        Range::index_type numSplats,
        RangeConstIterator first,
        RangeConstIterator last,
        const Grid &grid,
        const Recursion &recursionState);

public:
    void testSimple();            ///< Test basic usage
    void testDensityError();      ///< Test that @ref Bucket::DensityError is thrown correctly
    void testMultiLevel();        ///< Test recursion of @c bucketRecurse works
    void testFlat();              ///< Top level already meets the requirements
    void testEmpty();             ///< Edge case with zero splats inside the grid
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestBucket, TestSet::perBuild());

void TestBucket::bucketFunc(
    vector<Block> &blocks,
    const boost::ptr_vector<Collection> &splats,
    Range::index_type numSplats,
    RangeConstIterator first,
    RangeConstIterator last,
    const Grid &grid,
    const Recursion &recursionState)
{
    (void) splats;
    blocks.push_back(Block());
    Block &block = blocks.back();
    block.numSplats = numSplats;
    block.grid = grid;
    block.done = recursionState.cellsDone;
    copy(first, last, back_inserter(block.ranges));
}

void TestBucket::validate(
    const boost::ptr_vector<Collection> &splats,
    const Grid &fullGrid,
    const vector<Block> &blocks,
    std::size_t maxSplats,
    Grid::size_type maxCells)
{
    // TODO: also need to check that the blocks are no smaller than necessary

    /* To check that we haven't left out any part of a splat, we add up the
     * areas of the intersections with the blocks and check that it adds up to
     * the full bounding box of the splat.
     */
    vector<vector<double> > areas(splats.size());
    for (std::size_t i = 0; i < splats.size(); i++)
    {
        areas[i].resize(splats[i].size());
    }

    /* First validate each individual block */
    BOOST_FOREACH(const Block &block, blocks)
    {
        CPPUNIT_ASSERT(block.numSplats <= maxSplats);
        CPPUNIT_ASSERT(block.grid.numCells(0) <= maxCells);
        CPPUNIT_ASSERT(block.grid.numCells(1) <= maxCells);
        CPPUNIT_ASSERT(block.grid.numCells(2) <= maxCells);
        CPPUNIT_ASSERT(block.numSplats > 0);
        /* The grid must be a subgrid of the original */
        CPPUNIT_ASSERT_EQUAL(fullGrid.getSpacing(), block.grid.getSpacing());
        for (int i = 0; i < 3; i++)
        {
            CPPUNIT_ASSERT_EQUAL(fullGrid.getReference()[i], block.grid.getReference()[i]);
            pair<int, int> fullExtent = fullGrid.getExtent(i);
            pair<int, int> extent = block.grid.getExtent(i);
            CPPUNIT_ASSERT(fullExtent.first <= extent.first);
            CPPUNIT_ASSERT(fullExtent.second >= extent.second);
        }

        Range::index_type numSplats = 0;
        /* Checks that
         * - The splat count must be correct
         * - There must be no empty ranges
         * - The splat IDs should be increasing and ranges should be properly coalesced.
         * - The splats all intersect the block.
         *
         * At the same time, we accumulate the intersection area.
         */
        float worldLower[3];
        float worldUpper[3];
        block.grid.getVertex(0, 0, 0, worldLower);
        block.grid.getVertex(
            block.grid.numCells(0),
            block.grid.numCells(1),
            block.grid.numCells(2), worldUpper);
        for (std::size_t i = 0; i < block.ranges.size(); i++)
        {
            const Range &range = block.ranges[i];
            numSplats += range.size;
            CPPUNIT_ASSERT(range.size > 0);
            if (i > 0)
            {
                const Range &prev = block.ranges[i - 1];
                // This will fail for input files with >2^32 points, but we aren't testing those
                // yet.
                CPPUNIT_ASSERT(range.scan > prev.scan
                               || (range.scan == prev.scan && range.start > prev.start + prev.size));
            }

            vector<Splat> buffer(range.size);
            splats[range.scan].read(range.start, range.start + range.size, &buffer[0]);
            for (Range::index_type j = range.start; j < range.start + range.size; j++)
            {
                const Splat &splat = buffer[j - range.start];
                double area = 1.0;
                for (int k = 0; k < 3; k++)
                {
                    float lower = splat.position[k] - splat.radius;
                    float upper = splat.position[k] + splat.radius;
                    lower = max(lower, worldLower[k]);
                    upper = min(upper, worldUpper[k]);
                    CPPUNIT_ASSERT(lower <= upper);
                    area *= (upper - lower);
                }
                areas[range.scan][j] += area;
            }
        }
        CPPUNIT_ASSERT_EQUAL(numSplats, block.numSplats);
    }

    /* Check that the blocks do not overlap */
    for (std::size_t b1 = 0; b1 < blocks.size(); b1++)
        for (std::size_t b2 = b1 + 1; b2 < blocks.size(); b2++)
        {
            CPPUNIT_ASSERT(!gridsIntersect(blocks[b1].grid, blocks[b2].grid));
        }

    /* Check that each splat is fully covered */
    for (Range::scan_type scan = 0; scan < splats.size(); scan++)
    {
        for (Range::index_type id = 0; id < splats[scan].size(); id++)
        {
            Splat splat;
            splats[scan].read(id, id + 1, &splat);
            double area = 8.0 * splat.radius * splat.radius * splat.radius;
            CPPUNIT_ASSERT_DOUBLES_EQUAL(area, areas[scan][id], 1e-6);
        }
    }
}

void TestBucket::setupSimple()
{
    /* To make this easy to visualise, all splats are placed on a single Z plane.
     * This plane is along a major boundary, so each block can be expected to
     * appear twice (once on each side of the boundary).
     *
     * To plot the points for debugging, run gnuplot over the following (the coordinates
     * are in grid space):
set xrange [0:16]
set yrange [0:20]
set size square
set xtics 4
set ytics 4
set grid
plot '-' with points
4 8
12 6.8
12.8 4.8
12.8 7.2
14.8 7.2
14 6.4
4.8 14.8
5.2 14.8
4.8 15.2
5.2 15.2
6.8 12.8
7.2 13.2
10 18
e
pause -1
     */
    const float z = 10.0f;

    splatData.resize(3);

    splatData[0].push_back(makeSplat(10.0f, 20.0f, z, 2.0f));
    splatData[0].push_back(makeSplat(30.0f, 17.0f, z, 1.0f));
    splatData[0].push_back(makeSplat(32.0f, 12.0f, z, 1.0f));
    splatData[0].push_back(makeSplat(32.0f, 18.0f, z, 1.0f));
    splatData[0].push_back(makeSplat(37.0f, 18.0f, z, 1.0f));
    splatData[0].push_back(makeSplat(35.0f, 16.0f, z, 3.0f));

    splatData[1].push_back(makeSplat(12.0f, 37.0f, z, 1.0f));
    splatData[1].push_back(makeSplat(13.0f, 37.0f, z, 1.0f));
    splatData[1].push_back(makeSplat(12.0f, 38.0f, z, 1.0f));
    splatData[1].push_back(makeSplat(13.0f, 38.0f, z, 1.0f));
    splatData[1].push_back(makeSplat(17.0f, 32.0f, z, 1.0f));

    splatData[2].push_back(makeSplat(18.0f, 33.0f, z, 1.0f));

    splatData[2].push_back(makeSplat(25.0f, 45.0f, z, 4.0f));

    splats.clear();
    splats.reserve(splatData.size());
    for (std::size_t i = 0; i < splatData.size(); i++)
        splats.push_back(new Collection(splatData[i]));
}

void TestBucket::testSimple()
{
    setupSimple();

    // The grid is set up so that the origin is at (0, 0, 0)
    const float ref[3] = {-10.0f, 0.0f, 10.0f};
    Grid grid(ref, 2.5f, 4, 20, 0, 20, -4, 4);
    vector<Block> blocks;
    const int maxSplats = 5;
    const int maxCells = 8;
    const int maxSplit = 1000000;
    bucket(splats, grid, maxSplats, maxCells, maxSplit,
           boost::bind(&TestBucket::bucketFunc, boost::ref(blocks), _1, _2, _3, _4, _5, _6));
    validate(splats, grid, blocks, maxSplats, maxCells);

    // 11 was found by inspecting the output and checking the
    // blocks by hand
    CPPUNIT_ASSERT_EQUAL(11, int(blocks.size()));
}

void TestBucket::testDensityError()
{
    setupSimple();

    const float ref[3] = {-10.0f, 0.0f, 10.0f};
    Grid grid(ref, 2.5f, 4, 20, 0, 20, -4, 4);
    vector<Block> blocks;
    const int maxSplats = 1;
    const int maxCells = 8;
    const int maxSplit = 1000000;
    CPPUNIT_ASSERT_THROW(
        bucket(splats, grid, maxSplats, maxCells, maxSplit,
           boost::bind(&TestBucket::bucketFunc, boost::ref(blocks), _1, _2, _3, _4, _5, _6)),
        DensityError);
}

void TestBucket::testFlat()
{
    setupSimple();

    const float ref[3] = {-10.0f, 0.0f, 10.0f};
    Grid grid(ref, 2.5f, 4, 20, 0, 20, -4, 4);
    vector<Block> blocks;
    const int maxSplats = 15;
    const int maxCells = 32;
    const int maxSplit = 1000000;
    bucket(splats, grid, maxSplats, maxCells, maxSplit,
           boost::bind(&TestBucket::bucketFunc, boost::ref(blocks), _1, _2, _3, _4, _5, _6));
    validate(splats, grid, blocks, maxSplats, maxCells);

    CPPUNIT_ASSERT_EQUAL(1, int(blocks.size()));
}

void TestBucket::testEmpty()
{
    const float ref[3] = {-10.0f, 0.0f, 10.0f};
    Grid grid(ref, 2.5f, 4, 20, 0, 20, -4, 4);
    vector<Block> blocks;
    const int maxSplats = 5;
    const int maxCells = 8;
    const int maxSplit = 1000000;
    bucket(splats, grid, maxSplats, maxCells, maxSplit,
           boost::bind(&TestBucket::bucketFunc, boost::ref(blocks), _1, _2, _3, _4, _5, _6));
    CPPUNIT_ASSERT(blocks.empty());
}

void TestBucket::testMultiLevel()
{
    setupSimple();

    const float ref[3] = {-10.0f, 0.0f, 10.0f};
    Grid grid(ref, 2.5f, 4, 20, 0, 20, -4, 4);
    vector<Block> blocks;
    const int maxSplats = 5;
    const int maxCells = 8;
    const int maxSplit = 8;
    bucket(splats, grid, maxSplats, maxCells, maxSplit,
           boost::bind(&TestBucket::bucketFunc, boost::ref(blocks), _1, _2, _3, _4, _5, _6));
    validate(splats, grid, blocks, maxSplats, maxCells);

    // 11 was found by inspecting the output and checking the
    // blocks by hand
    CPPUNIT_ASSERT_EQUAL(11, int(blocks.size()));
}
