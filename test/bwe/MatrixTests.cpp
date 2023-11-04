#include "math/Matrix.h"
#include <cmath>
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
    const auto m = math::outerProduct(v);

    EXPECT_EQ(det2(math::principalSubMatrix<double, 3, 3, 2>(m)), 0.0);
    EXPECT_EQ(det(math::principalSubMatrix<double, 3, 3, 2>(m)), 0.0);
    EXPECT_EQ(det(m), 0.0);
    EXPECT_EQ(det2(m), 0.0);

    EXPECT_TRUE(math::isPositiveSemiDefinite2(m));
    EXPECT_TRUE(math::isPositiveSemiDefinite(m));
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

    ASSERT_TRUE(math::isSymmetric(m));
    EXPECT_TRUE(isPositiveSemiDefinite2(m));
    auto mine = std::abs(math::min(m));
    auto maxe = std::abs(math::max(m));

    auto mf = 0.001000;
    double detti = math::det(m);
    auto d2 = math::det2(m);
    ASSERT_GE(math::det(m), 0);
    auto L = choleskyDecompositionLL(m);
    auto c = L * transpose(L);
    EXPECT_FALSE(m == c);
    auto d = m - c;
    auto nrm = math::norm(m - c);
    EXPECT_LT(math::norm(m - c), 1.E-10);
    EXPECT_TRUE(math::isValid(L));
    // should check determinant of m - c
}

#define EXPECT_ERROR_LT(a, b, relativeError)                                                                           \
    {                                                                                                                  \
        if (std::min(std::abs(a), std::abs(b)) == 0)                                                                   \
        {                                                                                                              \
            EXPECT_LT(std::max(std::abs(a), std::abs(b)), relativeError);                                              \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            EXPECT_GT(std::min(std::abs(a), std::abs(b)) / std::max(std::abs(a), std::abs(b)), 1.0 - relativeError);   \
        }                                                                                                              \
    }

TEST(MatrixTest, determinant)
{
    math::Matrix<double, 5, 5> m;

    const auto acceptableRelativeError = 0.000000001;
    for (auto k = 0u; k < 15000; ++k)
    {
        randomize(m, 10000.0);

        const auto d1 = det2(m);
        const auto d2 = det2(transpose(m));
        const auto d3 = det(m);
        const auto d4 = det(transpose(m));

        if (d2 != d4)
        {
            if (det(m) == 0 || det(transpose(m)) == 0)
            {
            }
            EXPECT_ERROR_LT(det(m), det(transpose(m)), acceptableRelativeError);
        }
        if (d3 != d2)
        {
            EXPECT_ERROR_LT(det(m), det2(transpose(m)), acceptableRelativeError);
        }
        if (d1 != d4)
        {
            EXPECT_ERROR_LT(det2(m), det(transpose(m)), acceptableRelativeError);
        }
        if (d1 != d2)
        {
            const auto a1 = det2(m);
            const auto a2 = det2(transpose(m));
            const auto a3 = det(m);
            const auto a4 = det(transpose(m));
            const auto nd1 = std::nextafter(d1, d2);
            EXPECT_ERROR_LT(d1, d2, acceptableRelativeError);
        }
    }
}

TEST(MatrixTest, submatrix)
{
    math::Matrix<double, 5, 5> m;
    math::randomize(m, 1000.0);

    auto s2 = math::principalSubMatrix<double, m.rows(), m.columns(), 2>(m);
    EXPECT_EQ(m(0, 0), s2(0, 0));
    EXPECT_EQ(m(0, 1), s2(0, 1));
    EXPECT_EQ(m(1, 0), s2(1, 0));
    EXPECT_EQ(m(1, 1), s2(1, 1));
}

TEST(MatrixTest, posDefininte)
{
    math::Matrix<double, 3, 3> m2({{10.0, 5., 2.}, {5., 3., 2.}, {2., 2., 3.}});
    ASSERT_TRUE(isSymmetric(m2));
    EXPECT_TRUE(isPositiveDefinite(m2));
}

TEST(MatrixTest, cholesky3)
{
    math::Matrix<double, 3> v({10, 5, 3});
    auto m = math::outerProduct(v);

    ASSERT_TRUE(math::isSymmetric(m));
    EXPECT_TRUE(math::isPositiveSemiDefinite(m));

    auto L = choleskyDecompositionLL(m);
    auto c = L * transpose(L);
    EXPECT_EQ(m, c);
}

TEST(MatrixTest, choleskyOuterProduct)
{
    for (auto i = 0u; i < 10000; ++i)
    {
        math::Matrix<double, 3> v({10, 5, 3});
        math::randomize(v, 10000.0);
        auto m = math::outerProduct(v);

        const auto pd = isPositiveDefinite2(m);
        const auto psd = isPositiveSemiDefinite2(m);
        EXPECT_TRUE(psd);
        EXPECT_FALSE(pd);
        const auto L = choleskyDecompositionLL(m);
        EXPECT_TRUE(math::isLowerTriangular(L));
        EXPECT_EQ(m, L * transpose(L));
    }
}

TEST(MatrixTest, choleskyOnSum)
{
    for (int i = 0; i < 500; ++i)
    {
        math::Matrix<double, 3> v({10, 5, 3});
        math::randomize(v, 10000.0);
        auto m = math::outerProduct(v);

        auto sum = m;
        for (int j = 0; j < 100; ++j)
        {
            math::Matrix<double, 3> w;
            math::randomize(w, 10.0);
            auto smallDiff = math::outerProduct(w);
            sum += smallDiff;
        }

        EXPECT_TRUE(math::isPositiveSemiDefinite2(sum));
        auto L = math::choleskyDecompositionLL(sum);
        EXPECT_TRUE(math::isLowerTriangular(L));
        EXPECT_TRUE(math::isSymmetric(sum));
        if (sum != L * transpose(L))
        {
            const auto d = sum - L * transpose(L);
            const auto nrm = math::norm(d);
            EXPECT_LT(nrm, 1.E-8);
        }
    }
}
