#include "logger/Logger.h"
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

    Matrix<double, 4> v({5.2, 3.5, 2.6, 1.2});
    auto m4 = v * transpose(v);
    EXPECT_EQ(m4.columns(), m4.rows());
    EXPECT_EQ(m4.columns(), v.rows());

    auto mk = choleskyDecompositionLL(m4);
    EXPECT_EQ(mk(0, 0), sqrt(m4(0, 0)));
}
 
TEST(MatrixTest, cholesky)
{
    math::Matrix<double, 3> v({5.6, -156000.0, 19.9});
    auto m = math::outerProduct(v);

    EXPECT_NEAR(det(math::principalSubMatrix<double, 3, 3, 2>(m)), 0.0, 0.0002);
    auto a0 = det(m);
    auto c0 = detLU(m);
    EXPECT_NEAR(c0, 0.0, 6e-17);
    EXPECT_NEAR(a0, 0.0, 6e-17);

    // should be psd, but representaion errors causes det to be slightly negative
    EXPECT_TRUE(math::isPositiveSemiDefinite(m, 0.001));
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
    EXPECT_TRUE(isPositiveSemiDefinite(m));

    ASSERT_GE(math::det(m), 0);
    auto L = choleskyDecompositionLL(m);
    auto c = L * transpose(L);
    EXPECT_FALSE(m == c);
    EXPECT_LT(math::norm(m - c), 1.E-10);
    EXPECT_TRUE(math::isValid(L));
    // should check determinant of m - c
}

TEST(MatrixTest, determinant)
{
    math::Matrix<double, 5, 5> m;

    const auto acceptableRelativeError = 0.00000001;
    for (auto k = 0u; k < 15000; ++k)
    {
        randomize(m, 1000.0);

        const auto a0 = detByCoFactors(m);
        const auto a1 = detByCoFactors(transpose(m));
        const auto b0 = det(m);
        const auto b1 = det(transpose(m));
        const auto d1 = detLU(m);
        const auto d2 = detLU(transpose(m));

        const auto absError = acceptableRelativeError * std::abs(d1 + d2) / 2;
        EXPECT_NEAR(d1, d2, absError);
        EXPECT_NEAR(b0, b1, absError);
        EXPECT_NEAR(a0, a1, absError);
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
    const auto seed = math::Matrix<double, 3, 3>::I() * 0.0000001;

    for (auto i = 0u; i < 10000; ++i)
    {
        math::Matrix<double, 3> v({10, 5, 3});
        math::randomize(v, 10000.0);
        auto m = math::outerProduct(v);

        EXPECT_TRUE(isPositiveDefinite(m + seed));

        const auto L = choleskyDecompositionLL(m);
        EXPECT_TRUE(math::isLowerTriangular(L));
        EXPECT_EQ(m, L * transpose(L));
    }
}

TEST(MatrixTest, PDtest)
{
    const auto seed = math::Matrix<double, 3, 3>::I() * 0.0000001;
    double data[][3] = {//
        {-2418.000000, 2019.000000, 471.000000},
        {2778.000000, 4342.000000, 4969.000000},
        {-4223.000000, 2884.000000, 2390.000000},
        {3809.000000, -2757.000000, -1085.000000},
        {2697.000000, -1958.000000, -3622.000000},
        {2514.000000, 3210.000000, -3374.000000},
        {3206.000000, 4751.000000, 2886.000000},
        {-1684.000000, -2378.000000, -4991.000000},
        {3262.000000, 4944.000000, -1311.000000},
        {2779.000000, -2046.000000, 1061.000000},
        {-3189.000000, -4757.000000, -1653.000000},
        {3566.000000, -2718.000000, -2750.000000},
        {-3384.000000, -4629.000000, -4016.000000},
        {3425.000000, 4602.000000, 2874.000000},
        {-1459.000000, -2307.000000, -3262.000000},
        {-3847.000000, 2437.000000, -3798.000000},
        {-2956.000000, 1891.000000, 522.000000},
        {2725.000000, -2023.000000, -3784.000000},
        {3499.000000, -2884.000000, -1906.000000},
        {-2841.000000, -4542.000000, 593.000000},
        {-2549.000000, 1967.000000, 752.000000},
        {-3158.000000, -4790.000000, -4507.000000},
        {1907.000000, -1366.000000, 2179.000000},
        {-903.000000, -1092.000000, 4329.000000},
        {-1388.000000, 983.000000, -1764.000000},
        {2166.000000, 3134.000000, 1795.000000},
        {-3913.000000, 2419.000000, -3397.000000},
        {2609.000000, 4471.000000, 2350.000000},
        {-2440.000000, -3375.000000, 4311.000000},
        {2928.000000, 4678.000000, -4407.000000},
        {1193.000000, 1756.000000, -1151.000000},
        {-3619.000000, -4628.000000, 3646.000000},
        {-3212.000000, -4704.000000, 4656.000000},
        {-1650.000000, -2381.000000, 4462.000000},
        {-2269.000000, -3327.000000, 2056.000000},
        {3430.000000, 4589.000000, -4473.000000},
        {-1006.000000, 660.000000, 1035.000000},
        {-3533.000000, -4649.000000, 1754.000000},
        {3747.000000, -2536.000000, 3672.000000},
        {-2988.000000, -4716.000000, -2542.000000},
        {3294.000000, 4731.000000, -4378.000000}};

    for (auto rawVector : data)
    {
        math::Matrix<double, 3> v({rawVector[0], rawVector[1], rawVector[2]});
        auto m = math::outerProduct(v);

        EXPECT_TRUE(isPositiveSemiDefinite(m));
        EXPECT_TRUE(isPositiveDefinite(m + seed));

        const auto L = choleskyDecompositionLL(m);
        EXPECT_TRUE(math::isLowerTriangular(L));
        EXPECT_EQ(m, L * transpose(L));
    }
}

TEST(MatrixTest, principalDeterminants)
{
    math::Matrix<double, 3> v({337, 2614, -2297});
    auto m = math::outerProduct(v);

    auto dx = principalMinors(m);
    EXPECT_EQ(dx(0), 337 * 337.0);
    EXPECT_EQ(dx(1), 0.0);
    EXPECT_EQ(dx(2), 0.0);
    EXPECT_TRUE(math::isPositiveSemiDefinite(m));
}

TEST(MatrixTest, choleskyOnSum)
{
    for (int i = 0; i < 500; ++i)
    {
        math::Matrix<double, 3> v({10, 5, 3});
        math::randomize(v, 10000.0);
        auto m = math::outerProduct(v);

        EXPECT_TRUE(math::isPositiveSemiDefinite(m));
        EXPECT_FALSE(math::isPositiveDefinite(m));

        auto sum = m;
        for (int j = 0; j < 15; ++j)
        {
            math::Matrix<double, 3> w;
            math::randomize(w, 10.0);
            auto smallDiff = math::outerProduct(w);
            sum += smallDiff;
        }
        mulDiagonal(sum, 1.000001);
        EXPECT_TRUE(math::isPositiveDefinite(sum));
        EXPECT_TRUE(math::isPositiveSemiDefinite(sum));

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

// Assume two symetric matrices A' and B' created from outer products of vector A and B
// Both A' and B are positive semi definite
// But (A' + B') is positive definite as long as vector elements a1*b2 != a2*b1
// det(A'+ B') = (a1*b2 - b1*a2)^2, which is always positive as long as inequality above is true
TEST(MatrixTest, semiDefSum2x2)
{
    {
        math::Matrix<double, 2> a({4, 3});
        math::Matrix<double, 2> b({8, 6});

        auto h = math::outerProduct(a) + math::outerProduct(b);
        EXPECT_FALSE(math::isPositiveDefinite(h));
    }

    {
        math::Matrix<double, 2> a({4, 3});
        math::Matrix<double, 2> b({7, 5});

        auto h = math::outerProduct(a) + math::outerProduct(b);
        EXPECT_TRUE(math::isPositiveDefinite(h));
    }
}

// For sum of two symmetric 3x3 matrices, the sum is positive semi definite always
// if matrices are produced by outer product
TEST(MatrixTest, semiDefSum3x3)
{
    math::Matrix<double, 3> a({4, 3, 1});
    math::Matrix<double, 3> b({8, 6, 8});

    auto h = math::outerProduct(a) + math::outerProduct(b);
    EXPECT_TRUE(math::isPositiveSemiDefinite(h));
}

TEST(MatrixTest, semiDefSum4x4)
{
    math::Matrix<double, 4> a({4, 3, 1, 13});
    math::Matrix<double, 4> b({81, 6, 8, 6});

    auto h = math::outerProduct(a) + math::outerProduct(b);
    auto d = det(h);
    EXPECT_TRUE(math::isSymmetric(h));
    EXPECT_NEAR(d, 0, 1e-20);
    auto L = math::choleskyDecompositionLL(h);
    auto ll = L * transpose(L);
    EXPECT_LT(math::norm(h - ll), 1e-7);
}

TEST(MatrixTest, semiDefSumHuge)
{
    math::Matrix<double, 14> a({4, 3, 1, 13, 56, 1, 2, 5, 4, 2, 4, 8, 3, 6});
    math::Matrix<double, 14> b({81, 6, 8, 6, 3, 8, 1, 9, 4, 2, 6, 23, 6, 4});

    auto h = math::outerProduct(a) + math::outerProduct(b);
    auto d = det(h);
    EXPECT_TRUE(math::isSymmetric(h));
    EXPECT_EQ(d, 0);
    EXPECT_TRUE(math::isPositiveDefinite(h + h.I() * 0.00001));
    auto L = math::choleskyDecompositionLL(h);
    auto ll = L * transpose(L);
    EXPECT_LT(math::norm(h - ll), 1e-7);
}

TEST(MatrixTest, inversion)
{
    math::Matrix<double, 4> a({4, 3, 1, 13});
    auto A = math::outerProduct(a);
    A += A.I() * 0.000001;

    auto P = A.I();
    auto lue = math::decomposePLU(A, P);

    math::Matrix<double, 4, 4> L;
    math::Matrix<double, 4, 4> U;
    math::Matrix<double, 4, 4> E;

    math::splitPLU(lue, L, U);

    EXPECT_LT(math::norm(P * A - L * U), 0.000001);

    auto iA = math::invertLU(lue, P);

    auto identity = A * iA;
    auto idiff = identity - identity.I();
    EXPECT_LT(math::norm(idiff), 0.00000001);
}

TEST(MatrixTest, solveEq)
{
    math::Matrix<double, 3, 3> A({{1, 1, 1}, {0, 2, 5}, {2, 5, -1}});
    math::Matrix<double, 3, 1> b({6, -4, 27});

    auto ab = augment(A, b);
    math::gaussianElimination(ab);
    auto x1 = math::solve(A, b);
    math::Matrix<double, 3, 3> P;
    auto lu = math::decomposePLU(A, P);
    auto x = math::solveLU(lu, P, b);
    math::Matrix<double, 3, 1> expectedX({5, 3, -2});
    EXPECT_EQ(x, expectedX);
    EXPECT_EQ(x1, expectedX);

    auto ia = math::invertLU(lu, P);
    auto x2 = ia * b;
    EXPECT_LT(math::norm(x2 - expectedX), 0.000000000001);
    // auto x = math::solve(lue, P, b);
}
