#pragma once

// Receives ARKit face tracking from Epic's Live Link Face iOS app over UDP.
//
// The phone measures 52 blendshapes with the TrueDepth camera and streams them
// on port 11111. Nothing has to be installed on this machine — the engine just
// opens a socket — and the data is measured rather than inferred, which a
// webcam tracker cannot match on the subtle shapes a close-up needs.
//
// Packet layout (version is LITTLE endian, everything after it is BIG):
//   i32 version | char[37] uuid | i32 nameLen | char[nameLen] name |
//   i32 frame | f32 subFrame | i32 fps | i32 denominator | u8 count (61) |
//   61 * f32
// Values 0..51 are the ARKit shapes in APPLE's order, which is not ours;
// 52..60 are head yaw/pitch/roll then per-eye yaw/pitch/roll.

#include "anim/FaceShapes.h"

#include <string>
#include <vector>

namespace livelink
{
    const unsigned short kDefaultPort = 11111;
    const int kPacketValues = 61;   // 52 shapes + head yaw/pitch/roll + 2 eyes

    // One decoded packet, still in Apple's order.
    struct Packet
    {
        std::string  subject;
        int          frame = 0;
        float        subFrame = 0.0f;
        int          fps = 60;
        float        values[kPacketValues] = {};
    };

    // Apple's blendshape order, index-aligned with the packet. Public so the
    // remap can be checked against it in a test.
    const char* const* AppleShapeOrder();   // 52 entries

    // Packet index -> our canonical index (FaceShapes.h), built by name so a
    // typo fails loudly at startup instead of driving the wrong shape.
    const int* AppleToCanonical();          // 52 entries, -1 if unmatched

    // Decodes one datagram. False for anything that does not add up — a short
    // packet, a bad blendshape count, a name length that runs off the end.
    bool Decode(const unsigned char* data, unsigned int size, Packet& out);

    // Addresses of adapters that have a gateway, i.e. the ones actually on a
    // network. This is what has to be typed into the phone, and guessing it
    // from a machine full of virtual adapters is the usual reason nothing
    // arrives.
    std::vector<std::string> LocalAddresses();

    // Fills `out` (kShapeCount weights) from a packet, in our order.
    void ToWeights(const Packet& packet, float* out, unsigned int outCount);

    // A resting face does not read as zero: without subtracting it the
    // character wears the actor's resting expression for the whole take.
    // Samples are accumulated while calibrating, then reduced to a per-shape
    // median that Apply() removes and rescales against.
    class Neutral
    {
    public:
        void Begin();
        void Add(const float* weights, unsigned int count);
        // False when nothing was sampled; the calibration is then left alone.
        bool End();
        void Clear();

        bool Valid() const { return m_valid; }
        unsigned int SampleCount() const { return (unsigned int)m_samples.size(); }
        void Apply(float* weights, unsigned int count) const;

    private:
        std::vector<std::vector<float>> m_samples;
        float m_baseline[face::kShapeCount] = {};
        bool  m_collecting = false;
        bool  m_valid = false;
    };

    // The socket. Non-blocking, drained once per frame — at 60 Hz a thread buys
    // nothing. Engine-only (Winsock); the console never sees this.
    class Receiver
    {
    public:
        ~Receiver();

        bool Open(unsigned short port = kDefaultPort);
        void Close();
        bool IsOpen() const { return m_socket != ~0ull; }

        // Reads every queued datagram and keeps the newest. `now` is the
        // engine clock, used for the staleness and rate readouts.
        void Poll(double now);

        // True while packets are still arriving (nothing for ~1s = the phone
        // stopped, moved network, or the app was backgrounded).
        bool Receiving(double now) const;
        float PacketsPerSecond() const { return m_rate; }
        const std::string& Subject() const { return m_latest.subject; }
        const std::string& Error() const { return m_error; }

        // "Nothing arrived" and "something arrived and I threw it away" are
        // opposite faults, so they are counted apart. Rejects carry the sender
        // and size because that is what identifies the wrong app or mode.
        unsigned int DatagramCount() const { return m_datagrams; }
        unsigned int RejectedCount() const { return m_rejected; }
        unsigned int LastRejectSize() const { return m_lastRejectSize; }
        const std::string& LastSender() const { return m_lastSender; }

        // Latest weights in our order, neutral applied. True when a packet has
        // ever arrived.
        bool Weights(float* out, unsigned int outCount) const;
        // Eye yaw/pitch in degrees, measured rather than inferred.
        void EyeAngles(float& yaw, float& pitch) const;

        Neutral& Calibration() { return m_neutral; }

    private:
        unsigned long long m_socket = ~0ull;  // SOCKET, kept out of the header
        Packet m_latest;
        bool   m_haveLatest = false;
        double m_lastPacketTime = -1.0;
        double m_rateWindowStart = 0.0;
        int    m_rateCount = 0;
        float  m_rate = 0.0f;
        unsigned int m_datagrams = 0;
        unsigned int m_rejected = 0;
        unsigned int m_lastRejectSize = 0;
        std::string m_lastSender;
        std::string m_error;
        Neutral m_neutral;
    };
}
