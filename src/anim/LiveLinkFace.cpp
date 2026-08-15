#include "anim/LiveLinkFace.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX          // windows.h defines max/min as macros; <algorithm> loses
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

#include <algorithm>
#include <cstring>

namespace livelink
{
    namespace
    {
        unsigned int LoadU32BE(const unsigned char* p)
        {
            return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
                   ((unsigned int)p[2] << 8)  |  (unsigned int)p[3];
        }

        unsigned int LoadU32LE(const unsigned char* p)
        {
            return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
                   ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
        }

        float LoadF32BE(const unsigned char* p)
        {
            const unsigned int bits = LoadU32BE(p);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        // Winsock is refcounted per process; the engine has no other user, so
        // the first receiver starts it and it stays up for the session.
        bool EnsureWinsock()
        {
            static bool started = false;
            if (started) return true;
            WSADATA data;
            started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            return started;
        }
    }

    const char* const* AppleShapeOrder()
    {
        static const char* const kOrder[face::kShapeCount] =
        {
            "eyeBlinkLeft", "eyeLookDownLeft", "eyeLookInLeft", "eyeLookOutLeft",
            "eyeLookUpLeft", "eyeSquintLeft", "eyeWideLeft",
            "eyeBlinkRight", "eyeLookDownRight", "eyeLookInRight", "eyeLookOutRight",
            "eyeLookUpRight", "eyeSquintRight", "eyeWideRight",
            "jawForward", "jawLeft", "jawRight", "jawOpen",
            "mouthClose", "mouthFunnel", "mouthPucker", "mouthLeft", "mouthRight",
            "mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft", "mouthFrownRight",
            "mouthDimpleLeft", "mouthDimpleRight", "mouthStretchLeft", "mouthStretchRight",
            "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
            "mouthPressLeft", "mouthPressRight", "mouthLowerDownLeft", "mouthLowerDownRight",
            "mouthUpperUpLeft", "mouthUpperUpRight",
            "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
            "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
            "noseSneerLeft", "noseSneerRight", "tongueOut"
        };
        return kOrder;
    }

    const int* AppleToCanonical()
    {
        static int table[face::kShapeCount];
        static bool built = false;
        if (!built)
        {
            for (int i = 0; i < face::kShapeCount; ++i)
                table[i] = face::ShapeIndex(AppleShapeOrder()[i]);
            built = true;
        }
        return table;
    }

    bool Decode(const unsigned char* data, unsigned int size, Packet& out)
    {
        if (data == nullptr || size < 45)
            return false;

        const unsigned int nameLength = LoadU32BE(data + 41);
        if (nameLength > 256)
            return false;
        const unsigned int nameEnd = 45 + nameLength;
        // 17 bytes of frame/rate header, then the values.
        if ((unsigned long long)nameEnd + 17 + (unsigned long long)kPacketValues * 4 > size)
            return false;

        out = Packet();
        out.subject.assign((const char*)(data + 45), nameLength);
        (void)LoadU32LE(data);                     // version, unused
        out.frame    = (int)LoadU32BE(data + nameEnd);
        out.subFrame = LoadF32BE(data + nameEnd + 4);
        out.fps      = (int)LoadU32BE(data + nameEnd + 8);
        const unsigned int count = data[nameEnd + 16];
        if (count != (unsigned int)kPacketValues)
            return false;

        const unsigned char* values = data + nameEnd + 17;
        for (int i = 0; i < kPacketValues; ++i)
            out.values[i] = LoadF32BE(values + i * 4);
        return true;
    }

    void ToWeights(const Packet& packet, float* out, unsigned int outCount)
    {
        if (out == nullptr) return;
        for (unsigned int i = 0; i < outCount; ++i) out[i] = 0.0f;
        const int* remap = AppleToCanonical();
        for (int i = 0; i < face::kShapeCount; ++i)
        {
            const int shape = remap[i];
            if (shape < 0 || (unsigned int)shape >= outCount) continue;
            out[shape] = std::clamp(packet.values[i], 0.0f, 1.0f);
        }
    }

    // --- Neutral calibration --------------------------------------------------

    void Neutral::Begin()
    {
        m_samples.clear();
        m_collecting = true;
    }

    void Neutral::Add(const float* weights, unsigned int count)
    {
        if (!m_collecting || weights == nullptr || count != (unsigned int)face::kShapeCount)
            return;
        m_samples.push_back(std::vector<float>(weights, weights + count));
    }

    bool Neutral::End()
    {
        m_collecting = false;
        if (m_samples.empty())
            return false;
        for (int shape = 0; shape < face::kShapeCount; ++shape)
        {
            std::vector<float> column;
            column.reserve(m_samples.size());
            for (const std::vector<float>& sample : m_samples)
                column.push_back(sample[(std::size_t)shape]);
            // Median, not mean: one stray frame mid-calibration should not
            // shift the whole baseline.
            std::sort(column.begin(), column.end());
            m_baseline[shape] = column[column.size() / 2];
        }
        m_valid = true;
        return true;
    }

    void Neutral::Clear()
    {
        m_samples.clear();
        m_collecting = false;
        m_valid = false;
        for (int shape = 0; shape < face::kShapeCount; ++shape)
            m_baseline[shape] = 0.0f;
    }

    void Neutral::Apply(float* weights, unsigned int count) const
    {
        if (!m_valid || weights == nullptr) return;
        for (unsigned int shape = 0; shape < count && shape < (unsigned int)face::kShapeCount; ++shape)
        {
            const float base = m_baseline[shape];
            // Rescale what is left so a full expression still reaches 1.
            const float span = std::max(1e-3f, 1.0f - base);
            weights[shape] = std::clamp((weights[shape] - base) / span, 0.0f, 1.0f);
        }
    }

    // --- Receiver -------------------------------------------------------------

    Receiver::~Receiver() { Close(); }

    bool Receiver::Open(unsigned short port)
    {
        Close();
        m_error.clear();
        if (!EnsureWinsock())
        {
            m_error = "Winsock startup failed";
            return false;
        }

        SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == INVALID_SOCKET)
        {
            m_error = "could not create a UDP socket";
            return false;
        }

        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(handle, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR)
        {
            // Almost always another receiver already holding the port.
            m_error = "port " + std::to_string(port) + " is already in use";
            closesocket(handle);
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(handle, FIONBIO, &nonBlocking);
        m_socket = (unsigned long long)handle;
        return true;
    }

    void Receiver::Close()
    {
        if (m_socket != ~0ull)
        {
            closesocket((SOCKET)m_socket);
            m_socket = ~0ull;
        }
        m_haveLatest = false;
        m_lastPacketTime = -1.0;
        m_rate = 0.0f;
        m_rateCount = 0;
        m_datagrams = 0;
        m_rejected = 0;
        m_lastRejectSize = 0;
        m_lastSender.clear();
    }

    std::vector<std::string> LocalAddresses()
    {
        std::vector<std::string> out;
        EnsureWinsock();

        // FirstGatewayAddress stays null unless gateways are asked for, and a
        // machine with this many virtual adapters overflows a fixed buffer.
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
        std::vector<unsigned char> storage(16 * 1024);
        IP_ADAPTER_ADDRESSES* adapters = NULL;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            ULONG size = (ULONG)storage.size();
            adapters = (IP_ADAPTER_ADDRESSES*)&storage[0];
            const ULONG result = GetAdaptersAddresses(AF_INET, flags, NULL, adapters, &size);
            if (result == NO_ERROR)
                break;
            adapters = NULL;
            if (result != ERROR_BUFFER_OVERFLOW)
                return out;
            storage.resize(size);
        }
        if (adapters == NULL)
            return out;

        std::vector<std::string> fallback;
        for (IP_ADAPTER_ADDRESSES* a = adapters; a != NULL; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp)
                continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != NULL; u = u->Next)
            {
                const sockaddr_in* in = (const sockaddr_in*)u->Address.lpSockaddr;
                if (in->sin_family != AF_INET)
                    continue;
                char host[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
                // 169.254.* is a failed DHCP lease, never reachable from a phone.
                if (std::strncmp(host, "169.254.", 8) == 0)
                    continue;
                // A gateway separates the real network from the pile of virtual
                // adapters a dev machine accumulates, but if that leaves nothing
                // it is better to list every candidate than to claim there are none.
                if (a->FirstGatewayAddress != NULL)
                    out.push_back(host);
                else
                    fallback.push_back(host);
            }
        }
        return out.empty() ? fallback : out;
    }

    void Receiver::Poll(double now)
    {
        if (m_socket == ~0ull)
            return;

        // Drain the queue and keep the newest: a frame behind is a frame of
        // latency, and stale packets are worthless for a live mirror.
        unsigned char buffer[2048];
        for (;;)
        {
            sockaddr_in from = {};
            int fromSize = (int)sizeof(from);
            const int received = recvfrom((SOCKET)m_socket, (char*)buffer,
                (int)sizeof(buffer), 0, (sockaddr*)&from, &fromSize);
            if (received <= 0)
                break;

            ++m_datagrams;
            char host[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
            m_lastSender = host;

            Packet packet;
            if (!Decode(buffer, (unsigned int)received, packet))
            {
                // Bytes are arriving but are not Live Link. Keeping the size
                // and sender turns a silent nothing into something nameable.
                ++m_rejected;
                m_lastRejectSize = (unsigned int)received;
                continue;
            }
            m_latest = packet;
            m_haveLatest = true;
            m_lastPacketTime = now;
            ++m_rateCount;

            if (m_neutral.SampleCount() < 600)   // guard a calibration left running
            {
                float weights[face::kShapeCount];
                ToWeights(packet, weights, face::kShapeCount);
                m_neutral.Add(weights, face::kShapeCount);
            }
        }

        if (m_rateWindowStart <= 0.0)
            m_rateWindowStart = now;
        if (now - m_rateWindowStart >= 0.5)
        {
            m_rate = (float)(m_rateCount / (now - m_rateWindowStart));
            m_rateCount = 0;
            m_rateWindowStart = now;
        }
    }

    bool Receiver::Receiving(double now) const
    {
        return m_lastPacketTime >= 0.0 && (now - m_lastPacketTime) < 1.0;
    }

    bool Receiver::Weights(float* out, unsigned int outCount) const
    {
        if (!m_haveLatest || out == nullptr)
            return false;
        ToWeights(m_latest, out, outCount);
        m_neutral.Apply(out, outCount);
        return true;
    }

    void Receiver::EyeAngles(float& yaw, float& pitch) const
    {
        // 55..57 left eye yaw/pitch/roll, 58..60 right. Averaged: the eyes
        // converge, and a face layer drives them as a pair.
        yaw = pitch = 0.0f;
        if (!m_haveLatest) return;
        yaw   = (m_latest.values[55] + m_latest.values[58]) * 0.5f;
        pitch = (m_latest.values[56] + m_latest.values[59]) * 0.5f;
    }
}
