// Wron Motion Path — 2D vector helpers.
#pragma once

#include <cmath>

namespace wmp {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(double s) const { return {x / s, y / s}; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(double s) { x *= s; y *= s; return *this; }
    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    constexpr bool operator!=(const Vec2& o) const { return !(*this == o); }
};

constexpr Vec2 operator*(double s, const Vec2& v) { return {v.x * s, v.y * s}; }
constexpr double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
constexpr double lengthSq(const Vec2& v) { return dot(v, v); }
inline double length(const Vec2& v) { return std::hypot(v.x, v.y); }
inline bool isFinite(const Vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }
constexpr Vec2 lerp(const Vec2& a, const Vec2& b, double t) { return a + (b - a) * t; }

// Returns v / |v|, or {0,0} when |v| <= eps.
inline Vec2 normalizeOrZero(const Vec2& v, double eps = 0.0) {
    const double len = length(v);
    if (!(len > eps)) return {0.0, 0.0};
    return v / len;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double degToRad(double d) { return d * (kPi / 180.0); }
constexpr double radToDeg(double r) { return r * (180.0 / kPi); }

}  // namespace wmp
