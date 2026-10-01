#include "ops/metrics.hpp"

#include <algorithm>
#include <sstream>

namespace mv
{
std::string Metrics::labelText(Labels const& labels)
{
    if (labels.empty())
    {
        return {};
    }
    std::ostringstream out;
    out << '{';
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << labels[i].first << "=\"" << labels[i].second << '"';
    }
    out << '}';
    return out.str();
}

std::string Metrics::key(std::string const& name, Labels const& labels)
{
    auto sorted = labels;
    std::sort(sorted.begin(), sorted.end());
    return name + labelText(sorted);
}

void Metrics::inc(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    series_[key(name, labels)].value += value;
}

void Metrics::set(std::string const& name, Labels const& labels, double value)
{
    std::lock_guard lock{mutex_};
    auto& series = series_[key(name, labels)];
    series.gauge = true;
    series.value = value;
}

void Metrics::observe(std::string const& name, Labels const& labels, double seconds)
{
    std::lock_guard lock{mutex_};
    auto& hist = histograms_[key(name, labels)];
    if (hist.buckets.empty())
    {
        hist.buckets.assign(bounds_.size(), 0);
    }
    ++hist.count;
    hist.sum += seconds;
    for (std::size_t i = 0; i < bounds_.size(); ++i)
    {
        if (seconds <= bounds_[i])
        {
            ++hist.buckets[i];
        }
    }
}

std::string Metrics::render() const
{
    std::lock_guard lock{mutex_};
    std::ostringstream out;
    for (auto const& [name, series] : series_)
    {
        out << "mxl_multiviewer_" << name << ' ' << series.value << '\n';
    }
    for (auto const& [name, hist] : histograms_)
    {
        auto const base = name.substr(0, name.find('{'));
        auto const labels = name.find('{') == std::string::npos ? std::string{} : name.substr(name.find('{'));
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < bounds_.size(); ++i)
        {
            cumulative += hist.buckets[i];
            // buckets were stored as cumulative-or-not? observe increments every bucket the sample falls into,
            // which is already "le" cumulative if we increment all buckets >= value. We increment all bounds the
            // sample is <=, so each bucket count is already cumulative. Don't sum again.
        }
        (void)cumulative;
        for (std::size_t i = 0; i < bounds_.size(); ++i)
        {
            out << "mxl_multiviewer_" << base << "_bucket{le=\"" << bounds_[i] << "\"";
            if (!labels.empty())
            {
                out << ',' << labels.substr(1, labels.size() - 2);
            }
            out << "} " << hist.buckets[i] << '\n';
        }
        out << "mxl_multiviewer_" << base << "_sum" << labels << ' ' << hist.sum << '\n';
        out << "mxl_multiviewer_" << base << "_count" << labels << ' ' << hist.count << '\n';
    }
    return out.str();
}
} // namespace mv
