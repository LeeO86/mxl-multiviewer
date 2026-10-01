#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace mv
{
namespace
{
std::string nameFor(std::string const& seed, std::string const& tail)
{
    return uuidV5(kUuidNamespaceUrl, "mxl-multiviewer/" + seed + "/" + tail);
}
} // namespace

std::string NmosIds::videoReceiver(int input) const
{
    return nameFor(seed, "in/" + std::to_string(input) + "/video");
}

std::string NmosIds::audioReceiver(int input) const
{
    return nameFor(seed, "in/" + std::to_string(input) + "/audio");
}

std::string NmosIds::videoSource(int head) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/video/source");
}

std::string NmosIds::videoSender(int head) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/video/sender");
}

std::string NmosIds::videoFlow(int head, std::string const& formatToken) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/video/flow/" + formatToken);
}

std::string NmosIds::audioSource(int head) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/audio/source");
}

std::string NmosIds::audioSender(int head) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/audio/sender");
}

std::string NmosIds::audioFlow(int head, int channels) const
{
    return nameFor(seed, "out/" + std::to_string(head) + "/audio/flow/" + std::to_string(channels));
}

NmosIds makeNmosIds(std::string const& seed)
{
    NmosIds ids;
    ids.seed = seed;
    ids.node = nameFor(seed, "node");
    ids.device = nameFor(seed, "device");
    ids.domain = nameFor(seed, "domain");
    return ids;
}
} // namespace mv
