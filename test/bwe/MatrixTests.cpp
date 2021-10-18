#include "logger/Logger.h"
#include "math/Matrix.h"
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
using namespace math;
TEST(MatrixTest, mul)
{
    double y[4][3] = {{1.0, 2.0, 3.0}, {2.0, 3.0, 4.0}, {1.0, 3.5, 0.4}, {1.0, 3.0, 4.0}};
    Matrix<double, 4, 3> m(y);
    Matrix<double, 4, 3> myCopy(m);
    EXPECT_TRUE(m == myCopy);

    Matrix<double, 4, 3> a;
    a = m;
    EXPECT_TRUE(m == a);

    auto m2 = m * 5.6;

    auto m3 = m * transpose(m2);
    EXPECT_EQ(m3.columns(), m2.rows());

    Matrix<double, 4, 1> v({5.2, 3.5, 2.6, 1.2});
    auto m4 = v * transpose(v);
    EXPECT_EQ(m4.columns(), m4.rows());
    EXPECT_EQ(m4.columns(), v.rows());

    auto mk = choleskyDecompositionLL(m4);
    EXPECT_EQ(mk(0, 0), sqrt(m4(0, 0)));
}

TEST(MatrixTest, cholesky)
{
    math::Matrix<double, 3> v({5.6, -156000.0, 19.9});
    auto m = v * math::transpose(v);

    auto L = choleskyDecompositionLL(m);
    EXPECT_EQ(L.getColumn(0), v);
    auto c = L * transpose(L);
    EXPECT_EQ(m, c);
}

TEST(MatrixTest, choleskyStability)
{
    // This particular matrix contains numbers that cause error propagation
    // and collapsing decomposition into NaN
    math::Matrix<double, 3, 3> m({{6.255351392536535E-28, 5.5034655381268649E-19, -3.9095946203353344E-29},
        {5.5034655381268649E-19, 102615.51002615986, -3.4396659613292905E-20},
        {-3.9095946203353344E-29, -3.4396659613292905E-20, 2.443496637709584E-30}});

    auto L = choleskyDecompositionLL(m);
    auto c = L * transpose(L);
    EXPECT_FALSE(m == c);
    EXPECT_TRUE(math::isValid(L));
    // should check determinant of m - c
}