#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// YIN (de Cheveigné & Kawahara, 2002) : fréquence fondamentale en Hz, 0 si silence ou pas de note nette.
// fMin 28 Hz couvre le si grave d'une basse 5 cordes (30,9 Hz).
inline float yinPitch(const std::vector<float>& x, double sr, float fMin = 28.f, float fMax = 1400.f)
{
    const int tauMax = (int) (sr / fMin), tauMin = std::max(2, (int) (sr / fMax));
    const int w = (int) x.size() - tauMax - 1;
    if (w < 64)
        return 0.f;
    double energy = 0;
    for (int i = 0; i < w; ++i)
        energy += x[(size_t) i] * x[(size_t) i];
    if (energy / w < 1e-7)   // sous ~ -70 dBFS
        return 0.f;
    std::vector<float> d((size_t) tauMax + 2, 1.f);
    double run = 0;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        double s = 0;
        for (int i = 0; i < w; ++i)
        {
            const double t = x[(size_t) i] - x[(size_t) (i + tau)];
            s += t * t;
        }
        run += s;
        d[(size_t) tau] = run > 0 ? (float) (s * tau / run) : 1.f;   // différence normalisée cumulative
    }
    int tau = -1;
    for (int t = tauMin; t <= tauMax; ++t)
        if (d[(size_t) t] < 0.15f)
        {
            while (t + 1 <= tauMax && d[(size_t) t + 1] < d[(size_t) t])
                ++t;
            tau = t;
            break;
        }
    if (tau < 0)
        return 0.f;
    const float a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
    const float den = a - 2.f * b + c;
    const float shift = std::abs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;   // interpolation parabolique
    return (float) (sr / (tau + shift));
}
