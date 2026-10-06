// Wron Motion Path — arc-length parameterization of one cubic segment.
//
// Length is integrated with 5-point Gauss–Legendre quadrature on an adaptive
// subdivision of [0,1]: an interval is split until GL(a,b) and
// GL(a,m)+GL(m,b) agree within tol*(b-a), so the total error is bounded by
// `absTolerance` (in the units of the control points). The leaf intervals are
// kept as a monotone table (t_k, s_k). Inversion s -> t uses the table to
// bracket the answer, then safeguarded Newton (falls back to bisection where
// |B'| vanishes, e.g. at cusps or retracted handles).
#pragma once

#include <vector>

#include "wmp/bezier.h"

namespace wmp {

class ArcLengthTable {
public:
    void build(const Cubic& c, double absTolerance);

    double length() const { return total_; }
    // Arc length from t=0 to t (t clamped to [0,1]).
    double lengthAtT(double t) const;
    // Parameter t whose arc length from 0 is s (s clamped to [0, length]).
    double tAtLength(double s) const;
    std::size_t intervalCount() const { return t_.empty() ? 0 : t_.size() - 1; }
    const Cubic& curve() const { return c_; }

private:
    double speed(double t) const { return wmp::length(c_.d1(t)); }
    double gauss(double a, double b) const;
    void refine(double a, double b, double whole, double tol, int depth);

    Cubic c_{};
    double total_ = 0.0;
    double tol_ = 1e-9;
    std::vector<double> t_;  // breakpoints, t_[0] = 0, back() = 1
    std::vector<double> s_;  // cumulative length at breakpoints
};

}  // namespace wmp
