#include "media/ppm.hpp"

#include <algorithm>
#include <cmath>

namespace mv
{
double linearToDbfs(double linear)
{
    if (linear <= 1e-12)
    {
        return -120.0;
    }
    return std::max(-120.0, 20.0 * std::log10(linear));
}

double PpmMeter::levelDbfs() const
{
    return linearToDbfs(level);
}

double PpmMeter::holdDbfs() const
{
    return linearToDbfs(hold);
}

double PpmMeter::rmsDbfs() const
{
    return linearToDbfs(std::sqrt(meanSquare));
}

void PpmMeter::process(float const* samples, int count, double sampleRate, PpmConfig const& config)
{
    if (samples == nullptr || count <= 0 || sampleRate <= 0)
    {
        return;
    }
    double const attack = 1.0 - std::exp(-1.0 / (sampleRate * std::max(1e-6, config.attackTauSec)));
    double const decay = std::pow(10.0, -((config.decayDb / std::max(1e-6, config.decaySec)) / 20.0) / sampleRate);
    double const rmsCoeff = 1.0 - std::exp(-1.0 / (sampleRate * std::max(1e-6, config.rmsTauSec)));
    double const dt = 1.0 / sampleRate;
    for (int i = 0; i < count; ++i)
    {
        double const amplitude = std::fabs(static_cast<double>(samples[i]));
        if (amplitude >= level)
        {
            level += (amplitude - level) * attack;
        }
        else
        {
            level *= decay;
        }
        if (amplitude >= hold)
        {
            hold = amplitude;
            holdAgeSec = 0;
        }
        else
        {
            holdAgeSec += dt;
            if (holdAgeSec >= config.peakHoldSec)
            {
                hold *= decay;
            }
        }
        meanSquare += (amplitude * amplitude - meanSquare) * rmsCoeff;
        if (amplitude >= config.clipLinear)
        {
            clip = true;
            clipAgeSec = 0;
        }
        else if (clip)
        {
            clipAgeSec += dt;
            if (clipAgeSec > 2.0)
            {
                clip = false;
            }
        }
    }
}
} // namespace mv
