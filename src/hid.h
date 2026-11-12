// Finding the Commander Core and talking to it through Windows' own HID
// API (no Corsair software needed).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "protocol.h"

namespace cooler {

struct HidInfo {
    std::wstring path;
    uint16_t pid = 0;
    bool hasPump = true;
    std::string name;
};

// FindCoolers lists Commander Core / Core XT / ST control interfaces.
std::vector<HidInfo> FindCoolers();

class Link {
public:
    explicit Link(const std::wstring& path);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    bool Open() const;
    const std::string& Error() const { return error_; }

    // Send writes a request and waits for its answer.
    bool Send(const Bytes& command, const Bytes& data, Packet& reply);
    bool Send(const Bytes& command, const Bytes& data = {}) {
        Packet r;
        return Send(command, data, r);
    }
    std::optional<Bytes> ReadEndpoint(const Endpoint& e);
    bool WriteEndpoint(const Endpoint& e, const Bytes& data);

private:
    bool writePacket(const Packet& p);
    bool readPacket(Packet& p, unsigned ms);
    void* handle_;
    void* event_;
    size_t inLen_ = 0, outLen_ = 0;
    std::string error_;
};

// IcueRunning reports whether Corsair's own app is running: two programs
// driving the controller at once trip each other up.
bool IcueRunning();

}  // namespace cooler
