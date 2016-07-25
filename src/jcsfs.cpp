#include "jcsfs.h"

// Utility structures and functions

inline double scipy_stats_hypergeom_pmf(const int k, const int M, const int n, const int N)
{
    // scipy.stats.hypergeom.pmf(k, M, n, N) = choose(n, k) * choose(M - n, N - k) 
    // gsl_ran_hypergeometric_pdf(unsigned int k, unsigned int n1, unsigned int n2, unsigned int t) = C(n_1, k) C(n_2, t - k) / C(n_1 + n_2, t)
    const int gsl_n1 = n;
    const int gsl_k = k;
    const int gsl_n2 = M - n;
    const int gsl_t = N;
    return gsl_ran_hypergeometric_pdf(gsl_k, gsl_n1, gsl_n2, gsl_t);
}

template <typename T>
Vector<T> undistinguishedSFS(const Matrix<T> &csfs)
{
    int n = csfs.cols() - 1;
    // Total sample size of csfs is n + 2
    Vector<T> ret(n + 2 - 1);
    ret.setZero();
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < n + 1; ++b)
            if (1 <= a + b < n + 2)
                ret(a + b - 1) += csfs(a, b);
    return ret;
}

ParameterVector shiftParams(const ParameterVector &model1, const double shift)
{
    const std::vector<adouble> &a = model1[0];
    const std::vector<adouble> &s = model1[1];
    adouble zero = 0. * a[0];
    std::vector<adouble> cs(s.size() + 1);
    cs[0] = zero;
    std::partial_sum(s.begin(), s.end(), cs.begin() + 1);
    cs.back() = INFINITY;
    adouble tshift = zero + shift;
    int ip = std::distance(cs.begin(), std::upper_bound(cs.begin(), cs.end(), tshift)) - 1;
    std::vector<adouble> sp(s.begin() + ip, s.end());
    sp[ip] = cs[ip + 1] - shift;
    sp.back() = zero + 1.0;
    std::vector<adouble> ap(a.begin() + ip, a.end());
    return {ap, sp};
}

ParameterVector truncateParams(const ParameterVector params, const double truncationTime)
{
    const std::vector<adouble> &a = params[0];
    const std::vector<adouble> &s = params[1];
    T zero = s[0] * 0;
    std::vector<adouble> cs(s.size() + 1);
    cs[0] = zero;
    std::partial_sum(s.begin(), s.end(), cs.begin() + 1);
    cs.back() = INFINITY;
    T tt = zero + truncationTime;
    int ip = std::distance(cs.begin(), std::upper_bound(cs.begin(), cs.end(), tt)) - 1;
    std::vector<adouble> sp(s.begin(), s.begin() + ip + 2);
    sp[ip + 1] = split - cs[ip];
    std::vector<adouble> ap(a.begin(), a.begin() + ip + 2);
    ap.back() = 1e-8; // crash the population to get truncated times.
    return {ap, sp};
}

// Private class methods

template <typename T>
std::map<int, OnePopConditionedSFS<T> > JointCSFS<T>::make_csfs()
{
    std::map<int, OnePopConditionedSFS<T> > ret;
    for (const int &n : {n1, n1 + n2, n1 + n2 - 1, n2 - 2})
        ret.emplace(n, n);
    return ret;
}

template <typename T>
Eigen::DiagonalMatrix<double, Eigen::Dynamic, Eigen::Dynamic> JointCSFS<T>::make_S2()
{
    Vector<double> S2v(n1 + 2);
    for (int i = 0; i < n1 + 2; ++i)
        S2(i) = (double)i / (double)(n1 + 1);
    return S2v.asDiagonal();
}

template <typename T>
Eigen::DiagonalMatrix<double, Eigen::Dynamic, Eigen::Dynamic> JointCSFS<T>::make_S0()
{
    Eigen::DiagonalMatrix<double, Eigen::Dynamic, Eigen::Dynamic> I(n1 + 2);
    I.setIdentity();
    return I - S2;
}

template <typename T>
Vector<T> JointCSFS<T>::jcsfs_helper_tau_below_split(const int m, const T weight)
{
    double t1 = hidden_states[m], t2 = hidden_states[m + 1];
    assert(t1 < t2 <= split);
    assert(a1 == 2);
    const PiecewiseConstantRateFunction<T> eta(params1, {});

    const ParameterVector params1_trunc = truncateParams(params1, split);
    const PiecewiseConstantRateFunction<T> eta1_trunc(params1_trunc, {t1, t2});
    const Matrix<T> trunc_csfs = csfs.at(n1).compute(eta1_trunc)[0];
    for (int i = 0; i < a1 + 1; ++i)
        for (int j = 0; j < n1 + 1; ++j)
            tensorRef(m, i, j, 0) = weight * trunc_csfs(i, j);

    const ParameterVector params1_shift = shiftParams(params1, split);
    const PiecewiseConstantRateFunction<T> eta1_shift(params1_shift, {0., np.inf});
    Matrix<T> sfs_above_split = undistinguishedSFS(csfs.at(n1 + n2 - 1).compute(eta1_shift)[0]);
    Matrix<double> eMn10_avg(n1 + 2, n1 + 1), eMn12_avg(n1 + 2, n1 + 1);
    for (int k = 0; k < K; ++k)
    {
        // FIXME do something with seeding.
        T t = eta.random_time(t1, t2, 1);
        T Rt = eta.R(t);
        Matrix<adouble> A = Mn1.expM(Rts1 - Rt);
        Matrix<adouble> B = Mn10.expM(Rt);
        eMn10_avg += (A.lazyProduct(S0)).leftCols(n1 + 1) * B;
        eMn12_avg += (A.lazyProduct(S2)).rightCols(n1 + 1) * B.reverse();
    }
    eMn10_avg /= (double)K;
    eMn12_avg /= (double)K;
    // Now moran down
    for (int b1 = 0; b1 < n1 + 1; ++b1)
        for (int b2 = 0; b2 < n2 + 1; ++b2)
            for (int nseg = 1; nseg < n1 + n2 + 1; ++nseg)
                for (int np1 = std::max(nseg - n2, 0), np1 < std::min(nges, n1) + 1; ++np1)
                {
                    int np2 = nseg - np1;
                    double h = scipy_stats_hypergeom_pmf(np1, n1 + n2, nseg, n1);
                    tensorRef(m, 0, b1, b2) += weight * h * sfs_above_split(nseg - 1) * eMn10_avg(np1, b1) * eMn2(np2, b2);
                    tensorRef(m, 2, b1, b2) += weight * h * sfs_above_split(nseg - 1) * eMn12_avg(np1, b1) * eMn2(np2, b2);
                }
}

template <typename T>
void JointCSFS<T>::jcsfs_helper_tau_above_split(const int m, const T weight)
{
    double t1 = hidden_states[m], t2 = hidden_states[m + 1];
    assert(split <= t1 < t2);
    assert(a1 == 2);

    // Shift eta1 back by split units in time 
    PiecewiseConstantRateFunction<T> shifted_eta1(shiftParams(params1, split), {t1 - split, t2 - split});
    Vector<T> rsfs = undistinguishedSFS(csfs.at(n1 + n2).compute(shifted_eta1)[0]);

    for (int b1 = 0; b1 < n1 + 1; ++b1)
        for (int b2 = 0; b2 < n2 + 1; ++b2)
            for (int nseg = 0; nseg < n1 + n2 + 1; ++nseg)
                for (int np1 = std::max(nseg - n2, 0), nseg < std::min(nseg, n1) + 1; ++nseg)
                {
                    int np2 = nseg - np1;
                    // scipy.stats.hypergeom.pmf(np1, n1 + n2, nseg, n1)  = choose(nseg, np1) * choose(n1 + n2 - nseg, n1 - np1)
                    // gsl_ran_hypergeometric_pdf(unsigned int k, unsigned int n1, unsigned int n2, unsigned int t) 
                    double h = scipy_stats_hypergeom_pmf(np1, n1 + n2, nseg, n1);
                    for (int i = 0; i < 3; ++i)
                    {
                        int ind = i * (n1 + 1) * (n2 + 1) + b1 * (n2 + 1) + b2;
                        tensorRef(m, i, b1, b2) += weight * h * rsfs(i, nseg) * eMn1[i](np1, b1) * eMn2(np2, b2);
                    }
                }
     
    // pop 1, below split
    Matrix<T> sfs_below = csfs.at(n1).compute(*eta1)[0];
    for (int i = 0; i < a1 + 1; ++i)
        for (int j = 0; j < n1 + 1; ++j)
            tensorRef(m, i, j, 0) += weight * sfs_below(i, j);

    // pop2, below split
    if (n2 == 1)
        tensorRef(m, 0, 0, 1) += weight * split;
    if (n2 > 1)
    {
        ParameterVector params2_trunc = truncateParams(params2, split);
        const PiecewiseConstantRateFunction<T> eta2_trunc(params2_trunc, {0., np.inf});
        Vector<T> rsfs_below_2 = undistinguishedSFS(csfs.at(n2 - 2).compute(eta2_trunc)[0]);
        assert(rsfs_below_2.size() == n2 - 1);
        for (int i = 0; i < n2 - 1; ++i)
            tensorRef(m, 0, 0, i + 1) += weight * rsfs_below_2(i);
    }
}

template <typename T>
std::vector<Matrix<T> > JointCSFS<T>::compute(const PiecewiseConstantRateFunction&)
{
    return J;
}


// Public class methods
template <typename T>
void JointCSFS<T>::pre_compute(
        const ParameterVector params1, 
        const ParameterVector params2, 
        double split)
{
    this->split = split;
    this->params1 = params1;
    this->params2 = params2;
    std::vector<std::future<void> > res;
    eta1.reset(new PiecewiseConstantRateFunction<T>(params1, {split - 1e-6, split + 1e-6}));
    eta2.reset(new PiecewiseConstantRateFunction<T>(params2, {}));
    T Rts1 = eta1->R(split), Rts2 = eta2->R(split);
    eMn1[0] = Mn10.expM(Rts1);
    eMn1[1] = Mn11.expM(Rts1);
    eMn1[2] = eMn1[0].reverse();
    eMn2 = Mn2.expM(Rts2);
    T zero = params[0][0] * 0.;
    T one = zero + 1.;
    for (int m = 0; m < M; ++m)
    {
        J[m].setZero();
        double t1 = hidden_states[m], t2 = hidden_states[m + 1];
        if (t1 < t2 and t2 <= split)
            results.emplace(tp.enqueue([this, m, one] { 
                jcsfs_helper_tau_below_split(m, one); 
            }));
        else if (split <= t1 and t1 < t2)
            results.emplace(tp.enqueue([this, m, one] { 
                jcsfs_helper_tau_above_split(m, one); 
            }));
        else
        {
            T eR1t1 = exp(-eta1->R(zero + t1)), 
              eR1t2 = exp(-eta1->R(zero + t2));
            T w = (exp(-Rts1) - eR1t2) / (eR1t1 - eR1t2);
            results.emplace(tp.enqueue([this, m, w] { 
                jcsfs_helper_tau_above_split(m, w);
                jcsfs_helper_tau_below_split(m, one - w);
            }));
        }
    }
    for (auto &&res : results)
        res.wait();
}

// Instantiate necessary templates
template class JointCSFS<double>;
template class JointCSFS<adouble>;

