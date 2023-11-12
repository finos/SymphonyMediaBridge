#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace math
{

// default is column vector
template <typename T, uint32_t M, uint32_t N = 1>
class Matrix
{
    static_assert(M > 0 && N > 0);

public:
    Matrix()
    {
        for (uint32_t c = 0; c < N; ++c)
        {
            for (uint32_t r = 0; r < M; ++r)
            {
                _m[r][c] = 0;
            }
        }
    }

    explicit Matrix(const T (&other)[M][N])
    {
        for (uint32_t c = 0; c < N; ++c)
        {
            for (uint32_t r = 0; r < M; ++r)
            {
                _m[r][c] = other[r][c];
            }
        }
    }

    explicit Matrix(const T (&vector)[M])
    {
        static_assert(N == 1, "Can only create column vector Matrix<T,M,1> from array");
        for (uint32_t i = 0; i < M; ++i)
        {
            _m[i][0] = vector[i];
        }
    }

    Matrix(const Matrix<T, M, N>& other) { std::memcpy(&_m, &other._m, sizeof(_m)); }

    Matrix& operator=(const Matrix<T, M, N>& other)
    {
        std::memcpy(&_m, &other._m, sizeof(_m));
        return *this;
    }

    bool operator==(const Matrix<T, M, N>& other) const
    {
        for (uint32_t i = 0; i < M; ++i)
        {
            for (uint32_t j = 0; j < N; ++j)
            {
                if (_m[i][j] != other._m[i][j])
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool operator!=(const Matrix<T, M, N>& other) const { return !(*this == other); }

    Matrix& operator+=(const Matrix<T, M, N>& m2)
    {
        for (uint32_t i = 0; i < M; ++i)
        {
            for (uint32_t j = 0; j < N; ++j)
            {
                _m[i][j] = _m[i][j] + m2(i, j);
            }
        }
        return *this;
    }

    Matrix& operator*=(const T value)
    {
        for (uint32_t i = 0; i < M; ++i)
        {
            for (uint32_t j = 0; j < N; ++j)
            {
                _m[i][j] = _m[i][j] * value;
            }
        }
        return *this;
    }

    Matrix operator-() const
    {
        Matrix<T, M, N> n;
        for (uint32_t i = 0; i < M; ++i)
        {
            for (uint32_t j = 0; j < N; ++j)
            {
                n(i, j) = -_m[i][j];
            }
        }
        return n;
    }

    inline T& operator()(uint32_t r, uint32_t c = 0)
    {
        assert(r < M && c < N);
        return _m[r][c];
    }
    inline const T& operator()(uint32_t r, uint32_t c = 0) const
    {
        assert(r < M && c < N);
        return _m[r][c];
    }

    Matrix<T, M, 1> getColumn(uint32_t j) const
    {
        Matrix<T, M, 1> v;
        for (uint32_t k = 0; k < M; ++k)
        {
            v(k) = _m[k][j];
        }
        return v;
    }

    Matrix<T, 1, N> getRow(uint32_t i) const
    {
        Matrix<T, 1, N> v;
        for (uint32_t k = 0; k < N; ++k)
        {
            v(0, k) = _m[i][k];
        }
        return v;
    }

    void setColumn(uint32_t j, const Matrix<T, M, 1>& v)
    {
        for (uint32_t k = 0; k < M; ++k)
        {
            _m[k][j] = v(k);
        }
    }

    constexpr int columns() const { return N; }
    constexpr int rows() const { return M; }

    static Matrix I()
    {
        static_assert(M == N);
        Matrix m;
        for (auto i = 0u; i < M; ++i)
        {
            m(i, i) = 1.0;
        }
        return m;
    }

    T absSum() const
    {
        T s = 0;
        for (auto i = 0u; i < M; ++i)
        {
            for (auto j = 0u; j < M; ++j)
            {
                s += std::abs(_m[i][j]);
            }
        }
        return s;
    }

private:
    T _m[M][N];
};

template <typename T, uint32_t M>
using Vector = Matrix<T, M, 1>;

template <typename T, uint32_t M, uint32_t N, uint32_t P>
Matrix<T, M, P> operator*(const Matrix<T, M, N>& m1, const Matrix<T, N, P>& m2)
{
    Matrix<T, M, P> r;
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = 0; j < P; ++j)
        {
            T sum_ij = 0;
            for (uint32_t k = 0; k < N; ++k)
            {
                sum_ij += m1(i, k) * m2(k, j);
            }
            r(i, j) = sum_ij;
        }
    }
    return r;
}

// more efficient for columnVector * rowVector
template <typename T, uint32_t N>
Matrix<T, N, N> outerProduct(const Matrix<T, N, 1>& m1, const Matrix<T, 1, N>& m2)
{
    Matrix<T, N, N> r;
    for (uint32_t i = 0; i < N; ++i)
    {
        for (uint32_t j = 0; j <= i; ++j)
        {
            r(i, j) = m1(i, 0) * m2(0, j);
            r(j, i) = r(i, j);
        }
    }
    return r;
}

template <typename T, uint32_t N>
Matrix<T, N, N> outerProduct(const Matrix<T, N, 1>& m1)
{
    Matrix<T, N, N> r;
    for (uint32_t i = 0; i < N; ++i)
    {
        for (uint32_t j = 0; j <= i; ++j)
        {
            r(i, j) = m1(i, 0) * m1(j, 0);
            r(j, i) = r(i, j);
        }
    }
    return r;
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, M, N> operator+(const Matrix<T, M, N>& m1, const Matrix<T, M, N>& m2)
{
    Matrix<T, M, N> r;
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = 0; j < N; ++j)
        {
            r(i, j) = m1(i, j) + m2(i, j);
        }
    }
    return r;
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, M, N> operator-(const Matrix<T, M, N>& m1, const Matrix<T, M, N>& m2)
{
    Matrix<T, M, N> r;
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = 0; j < N; ++j)
        {
            r(i, j) = m1(i, j) - m2(i, j);
        }
    }
    return r;
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, N, M> transpose(const Matrix<T, M, N>& m)
{
    Matrix<T, N, M> r;
    for (uint32_t i = 0; i < N; ++i)
    {
        for (uint32_t j = 0; j < M; ++j)
        {
            r(i, j) = m(j, i);
        }
    }
    return r;
}

// Will sometimes round of small numbers to 0 to avoid div zero and sqrt(negative)
// For some matrices M, L*transpose(L) != M.
template <typename T, uint32_t M>
Matrix<T, M, M> choleskyDecompositionLL(const Matrix<T, M, M>& m)
{
    Matrix<T, M, M> lowerTriangular;

    for (uint32_t i = 0; i < M; i++)
    {
        for (uint32_t j = 0; j <= i; j++)
        {
            T sum = 0;
            if (j == i)
            {
                for (uint32_t k = 0; k < j; k++)
                {
                    sum += lowerTriangular(i, k) * lowerTriangular(i, k);
                }

                const auto d = m(i, i) - sum;
                lowerTriangular(j, j) = std::sqrt(std::abs(d));
            }
            else
            {
                for (uint32_t k = 0; k < j; k++)
                {
                    sum += lowerTriangular(i, k) * lowerTriangular(j, k);
                }
                const auto d = m(i, j) - sum;
                if (lowerTriangular(j, j) != 0)
                {
                    lowerTriangular(i, j) = d / lowerTriangular(j, j);
                }
                else
                {
                    assert(std::abs(d) < 1E-10);
                    lowerTriangular(i, j) = 0;
                }
            }
        }
    }
    return lowerTriangular;
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, M, N> operator*(const Matrix<T, M, N>& m, T scalar)
{
    Matrix<T, M, N> r;
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = 0; j < N; ++j)
        {
            r(i, j) = m(i, j) * scalar;
        }
    }
    return r;
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, M, N> operator*(T scalar, const Matrix<T, M, N>& m)
{
    return m * scalar;
}

template <typename T, uint32_t M, uint32_t N>
bool isValid(const Matrix<T, M, N>& m)
{
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = 0; j < N; ++j)
        {
            if (std::isnan(m(i, j)))
            {
                return false;
            }
        }
    }
    return true;
}

// Frobenius norm ov matrix, and "norm" of vector
template <typename T, uint32_t M, uint32_t N>
T norm(const Matrix<T, M, N>& m)
{
    T n = 0;
    for (auto i = 0u; i < M; ++i)
    {
        for (auto j = 0u; j < N; ++j)
        {
            n += m(i, j) * m(i, j);
        }
    }
    return std::sqrt(n);
}

template <typename T, uint32_t M, uint32_t N>
T min(const Matrix<T, M, N>& m)
{
    T n = m(0, 0);
    for (auto i = 0u; i < M; ++i)
    {
        for (auto j = 0u; j < N; ++j)
        {
            n = std::min(n, m(i, j));
        }
    }
    return n;
}

template <typename T, uint32_t M, uint32_t N>
T max(const Matrix<T, M, N>& m)
{
    T n = m(0, 0);
    for (auto i = 0u; i < M; ++i)
    {
        for (auto j = 0u; j < N; ++j)
        {
            n = std::max(n, m(i, j));
        }
    }
    return n;
}

template <typename T, uint32_t M>
bool isSymmetric(const Matrix<T, M, M>& m)
{
    for (auto i = 1u; i < M; ++i)
    {
        for (auto j = 0u; j < i; ++j)
        {
            if (m(i, j) != m(j, i))
            {
                return false;
            }
        }
    }
    return true;
}

template <typename T, uint32_t M>
void makeSymmetric(Matrix<T, M, M>& m)
{
    for (auto i = 1u; i < M; ++i)
    {
        for (auto j = 0u; j < i; ++j)
        {
            if (m(i, j) != m(j, i))
            {
                const auto v = (m(i, j) + m(j, i)) / 2.0;
                m(i, j) = v;
                m(j, i) = v;
            }
        }
    }
}

// swaps selected row with a lower row to make sure smallest absolute value is at row cr column cr.
// Multiplies the swapped row with -1 to preserve determinant
template <typename T, uint32_t M, uint32_t N>
void pivotRows(Matrix<T, M, N>& m, uint32_t cr)
{
    static_assert(std::is_signed<T>::value);
    static_assert(N >= M);
    T candidateValue = std::abs(m(cr, cr));
    auto candidatePos = cr;
    for (auto i = cr + 1; i < M; ++i)
    {
        auto v = std::abs(m(i, cr));
        if (v > candidateValue)
        {
            candidatePos = i;
            candidateValue = v;
        }
    }

    if (candidatePos == cr)
    {
        return;
    }

    bool signIsDifferent = std::signbit(m(cr, cr)) != std::signbit(m(candidatePos, cr));
    for (auto j = 0u; j < N; ++j)
    {
        std::swap(m(cr, j), m(candidatePos, j));
        if (signIsDifferent)
        {
            m(cr, j) *= -1;
        }
        else
        {
            m(candidatePos, j) *= -1;
        }
    }

    return;
}

template <typename T, uint32_t M, uint32_t N>
void gaussianElimination(Matrix<T, M, N>& m)
{
    // along diagonal
    for (auto i = 0u; i < M - 1; ++i)
    {
        pivotRows(m, i);

        if (m(i, i) == 0)
        {
            continue;
        }

        for (auto r = i + 1; r < M; ++r)
        {
            const auto f = m(r, i) / m(i, i);
            m(r, i) = 0;
            for (auto j = i + 1; j < N; ++j)
            {
                m(r, j) -= m(i, j) * f;
            }
        }
    }
}

template <typename T, uint32_t M, uint32_t N>
Matrix<T, M - 1, N - 1> eleminateRowColumn(const Matrix<T, M, N>& m, uint32_t r, uint32_t c)
{
    Matrix<T, M - 1, N - 1> em;

    uint32_t a = 0;
    for (auto i = 0u; i < M; ++i)
    {
        if (i == r)
        {
            continue;
        }

        uint32_t b = 0;
        for (auto j = 0u; j < N; ++j)
        {
            if (j == c)
            {
                continue;
            }
            em(a, b) = m(i, j);
            ++b;
        }
        ++a;
    }

    return em;
}

template <typename T>
T det(const Matrix<T, 1, 1>& m)
{
    return m(0, 0);
}

template <typename T>
T det(const Matrix<T, 2, 2>& m)
{
    const auto a = m(0, 0) * m(1, 1);
    const auto b = m(0, 1) * m(1, 0);

    return a - b;
}

// determinant using Gaussian elemination, O(n2)
template <typename T, uint32_t M>
T det(Matrix<T, M, M> m)
{
    gaussianElimination(m);

    T a = 1;
    for (auto i = 1u; i < M; ++i)
    {
        a *= m(i, i);
    }

    return a;
}

template <typename T, uint32_t M, uint32_t N, uint32_t R>
Matrix<T, R, R> principalSubMatrix(const Matrix<T, M, N>& m)
{
    static_assert(R < M && R < N);
    Matrix<T, R, R> psm;
    for (auto i = 0u; i < R; ++i)
    {
        for (auto j = 0u; j < R; ++j)
        {
            psm(i, j) = m(i, j);
        }
    }
    return psm;
}

template <typename T, uint32_t M, uint32_t R>
T leadingPrincipalMinor(const Matrix<T, M, M>& m)
{
    return det(principalSubMatrix<T, M, M, R>(m));
}

namespace
{

} // namespace

template <typename T>
bool isPositiveDefinite(const Matrix<T, 1, 1>& m)
{
    return m(0, 0) > 0;
}

template <typename T>
bool isPositiveDefinite(const Matrix<T, 2, 2>& m)
{
    return m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0) > 0;
}

// m is positive definite if all leading principal minors are positive
// If we do Gaussian elimination first, all principal minors are positive if diagonal elements are positive
template <typename T, uint32_t M>
bool isPositiveDefinite(const Matrix<T, M, M>& m)
{
    if (!isSymmetric(m))
    {
        return false;
    }

    if (isPositiveDefinite(principalSubMatrix<T, M, M, M - 1>(m)))
    {
        auto d = m;
        gaussianElimination(d);
        auto p = d(0, 0);
        for (auto i = 1u; i < M; ++i)
        {
            p *= d(i, i);
        }

        return p > 0;
    }

    return false;
}

template <typename T>
bool isPositiveSemiDefinite(const Matrix<T, 1, 1>& m)
{
    return m(0, 0) >= 0;
}

template <typename T>
bool isPositiveSemiDefinite(const Matrix<T, 2, 2>& m)
{
    if (!isSymmetric(m) || m(0, 0) < 0)
    {
        return false;
    }
    auto d1 = m(0, 0) * m(1, 1);
    auto d2 = m(0, 1) * m(1, 0);

    auto dt = d1 - d2;
    return dt >= 0;
}

// check is positive semi definite by Gaussian elimination
template <typename T, uint32_t M>
bool isPositiveSemiDefinite(const Matrix<T, M, M>& m)
{
    if (!isSymmetric(m))
    {
        return false;
    }

    if (isPositiveSemiDefinite(principalSubMatrix<T, M, M, M - 1>(m)))
    {
        auto d = m;
        gaussianElimination(d);
        auto p = d(0, 0);
        for (auto i = 1u; i < M; ++i)
        {
            p *= d(i, i);
        }

        return p >= 0;
    }

    return false;
}

template <typename T, uint32_t M, uint32_t N>
void randomizePositive(Matrix<T, M, N>& m, T maxValue)
{
    for (auto i = 0u; i < M; ++i)
    {
        for (auto j = 0u; j < N; ++j)
        {
            m(i, j) = maxValue * (rand() % 10000) / 10000.0;
        }
    }
}

template <typename T, uint32_t M, uint32_t N>
void randomize(Matrix<T, M, N>& m, T valueRange)
{
    for (auto i = 0u; i < M; ++i)
    {
        for (auto j = 0u; j < N; ++j)
        {
            m(i, j) = valueRange * (rand() % 10000) / 10000.0 - valueRange / 2;
        }
    }
}

template <typename T, uint32_t M>
bool isLowerTriangular(const Matrix<T, M, M>& m)
{
    for (uint32_t i = 0; i < M; ++i)
    {
        for (uint32_t j = i + 1; j < M; ++j)
        {
            if (m(i, j) != 0)
            {
                return false;
            }
        }
    }
    return true;
}

template <typename T, uint32_t M>
void mulDiagonal(Matrix<T, M, M>& m, T scalar)
{
    for (auto i = 0u; i < M; ++i)
    {
        m(i, i) *= scalar;
    }
}
} // namespace math
