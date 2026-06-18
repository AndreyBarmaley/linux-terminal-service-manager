// Google Gemini AI

#include <unordered_set>
#include <string_view>
#include <stdexcept>

#include <gtest/gtest.h>

#include "ltsm_xcb_types.h"
#include "ltsm_application.h"

using namespace LTSM;
using namespace LTSM::XCB;

TEST(XCBExceptionTest, ThrowAndCatch) {
    EXPECT_THROW({
        throw xcb_error("connection failed");
    }, xcb_error);

    EXPECT_THROW({
        throw xcb_error("runtime fail");
    }, std::runtime_error);

    try {
        throw xcb_error_busy("resource locked");
    } catch (const xcb_error& e) {
        EXPECT_STREQ(e.what(), "resource locked");
    } catch (...) {
        FAIL() << "Should have been caught as xcb_error";
    }
}

TEST(XCBPointTest, PropertiesAndOperators) {
    Point p1;
    EXPECT_EQ(p1.x, -1);
    EXPECT_EQ(p1.y, -1);
    EXPECT_FALSE(p1.isValid());

    Point p2(10, 20);
    EXPECT_TRUE(p2.isValid());

    EXPECT_TRUE(p2 == Point(10, 20));
    EXPECT_TRUE(p2 != p1);

    Point p3 = p2 + Point(5, 5);
    EXPECT_EQ(p3, Point(15, 25));

    Point p4 = p2 - Point(5, 5);
    EXPECT_EQ(p4, Point(5, 15));
}

TEST(XCBSizeTest, PropertiesAndComparisons) {
    Size s1;
    EXPECT_EQ(s1.width, 0);
    EXPECT_EQ(s1.height, 0);
    EXPECT_TRUE(s1.isEmpty());

    Size s2(100, 200);
    EXPECT_FALSE(s2.isEmpty());

    Size s3(50, 50);
    Size s4(10, 300);
    EXPECT_TRUE(s4 > s3);
    EXPECT_TRUE(s3 < s4);
    EXPECT_TRUE(s2 == Size(100, 200));

    s2.reset();
    EXPECT_TRUE(s2.isEmpty());
}

TEST(XCBPointIteratorTest, IncrementsAndLimits) {
    Size limit(2, 2);
    PointIterator it(0, 0, limit);

    EXPECT_TRUE(it.isValid());

    EXPECT_NO_THROW(++it);
    
    PointIterator out_of_bounds(2, 2, limit);
    EXPECT_FALSE(out_of_bounds.isValid());
}

TEST(XCBRegionTest, ConstructionAndAssignment) {
    Region r1;
    EXPECT_EQ(r1.x, -1);
    EXPECT_EQ(r1.width, 0);

    Region r2(Point(10, 10), Size(100, 50));
    EXPECT_EQ(r2.topLeft(), Point(10, 10));
    EXPECT_EQ(r2.toSize(), Size(100, 50));

    Region r3;
    r3.assign(r2);
    EXPECT_EQ(r3, r2);

    r3.assign(0, 0, 10, 10);
    EXPECT_EQ(r3, Region(0, 0, 10, 10));
}

TEST(XCBRegionTest, GeometryOperations) {
    Region r1(0, 0, 50, 50);
    Region r2(25, 25, 50, 50);

    EXPECT_TRUE(Region::intersects(r1, r2));

    Region result;
    bool intersects = Region::intersection(r1, r2, &result);
    EXPECT_TRUE(intersects);
    EXPECT_EQ(result, Region(25, 25, 25, 25));

    Region r3(0, 0, 10, 10);
    r3.join(10, 10, 10, 10);
    EXPECT_TRUE(r3.isValid());
}

TEST(XCBRegionTest, ModificationOperators) {
    Region r(10, 10, 100, 100);
    Point offset(5, 5);

    Region shifted_plus = r + offset;
    EXPECT_EQ(shifted_plus.topLeft(), Point(15, 15));
    EXPECT_EQ(shifted_plus.toSize(), Size(100, 100));

    Region shifted_minus = r - offset;
    EXPECT_EQ(shifted_minus.topLeft(), Point(5, 5));
}

TEST(XCBRegionTest, DivisionMethods) {
    Region r(0, 0, 100, 100);
    Size blockSize(50, 50);

    std::list<Region> blocks = r.divideBlocks(blockSize);
    EXPECT_FALSE(blocks.empty());

    std::list<Region> counts = r.divideCounts(2, 2);
    EXPECT_FALSE(counts.empty());
}

TEST(XCBHasherTest, UnorderedSetIntegration) {
    std::unordered_set<Region, HasherRegion> region_set;
    
    Region r1(10, 20, 30, 40);
    Region r2(50, 60, 70, 80);

    region_set.insert(r1);
    region_set.insert(r2);

    EXPECT_EQ(region_set.size(), 2);
    EXPECT_EQ(region_set.count(r1), 1);
}

TEST(XCBRegionPixelTest, DataAccess) {
    Region r(0, 0, 640, 480);
    uint32_t color = 0xFF00FF00; // Green ARGB

    RegionPixel pixel_node(r, color);

    EXPECT_EQ(pixel_node.pixel(), color);
    EXPECT_EQ(pixel_node.region(), r);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_xcbtypes.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
