#pragma once

namespace mv
{
// IEC 60268-10 type IIa defaults: 10 ms attack time constant, 24 dB return in 2.8 s.
struct PpmConfig
{
    double attackTauSec = 0.010;
    double decayDb = 24.0;
    double decaySec = 2.8;
    double peakHoldSec = 2.0;
    double clipLinear = 0.999;
    double rmsTauSec = 0.250;
};

struct PpmMeter
{
    double level = 0; // linear amplitude
    double hold = 0;
    double holdAgeSec = 0;
    double meanSquare = 0;
    bool clip = false;
    double clipAgeSec = 0;

    void process(float const* samples, int count, double sampleRate, PpmConfig const& config);
    [[nodiscard]] double levelDbfs() const;
    [[nodiscard]] double holdDbfs() const;
    [[nodiscard]] double rmsDbfs() const;
};

double linearToDbfs(double linear);
} // namespace mv
