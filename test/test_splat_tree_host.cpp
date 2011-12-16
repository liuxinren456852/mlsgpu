/**
 * @file
 *
 * Test code for @ref SplatTreeHost.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <vector>
#include <cstddef>
#include "testmain.h"
#include "test_splat_tree.h"
#include "../src/splat.h"
#include "../src/grid.h"
#include "../src/splat_tree_host.h"

class TestSplatTreeHost : public TestSplatTree
{
    CPPUNIT_TEST_SUB_SUITE(TestSplatTreeHost, TestSplatTree);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual void build(
        std::size_t &numLevels,
        std::vector<SplatTree::command_type> &commands,
        std::vector<SplatTree::command_type> &start,
        const std::vector<Splat> &splats, const Grid &grid);
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestSplatTreeHost, TestSet::perBuild());

void TestSplatTreeHost::build(
    std::size_t &numLevels,
    std::vector<SplatTree::command_type> &commands,
    std::vector<SplatTree::command_type> &start,
    const std::vector<Splat> &splats,
    const Grid &grid)
{
    SplatTreeHost tree(splats, grid);
    numLevels = tree.getNumLevels();
    commands = tree.getCommands();
    start = tree.getStart();
}
