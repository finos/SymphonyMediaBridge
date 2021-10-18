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

    bool operator==(const Matrix<T, M, N>& other) const { return 0 == std::memcmp(&_m, &other._m, sizeof(_m)); }

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

    void setColumn(uint32_t j, const Matrix<T, M, 1>& v)
    {
        for (uint32_t k = 0; k < M; ++k)
        {
            _m[k][j] = v(k);
        }
    }

    constexpr int columns() const { return N; }
    constexpr int rows() const { return M; }

private:
    T _m[M][N];
};

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
                    sum += (lowerTriangular(i, k) * lowerTriangular(i, k));
                }

                auto d = m(i, i) - sum;
                lowerTriangular(j, j) = std::sqrt(std::abs(d));
            }
            else
            {
                for (uint32_t k = 0; k < j; k++)
                {
                    sum += (lowerTriangular(i, k) * lowerTriangular(j, k));
                }
                auto d = m(i, j) - sum;
                if (lowerTriangular(j, j) == 0)
                {
                    assert(std::abs(d) < 1E-20);
                }
                lowerTriangular(i, j) = (lowerTriangular(j, j) != 0 ? d / lowerTriangular(j, j) : 0);
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

} // namespace math
