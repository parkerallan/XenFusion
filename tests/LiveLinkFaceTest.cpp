// Live Link Face packet decoding and the Apple -> canonical shape remap.
//
// The remap is the dangerous part: both orders contain the same 52 names, so
// getting it wrong produces a face that moves plausibly and is completely wrong
// (a blink driving the jaw). The packet is built here to the documented layout
// and decoded back, which pins the field offsets and endianness too — note the
// version is little-endian while everything after it is big.

#include "anim/FaceShapes.h"
#include "anim/LiveLinkFace.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{
    int Fail(const std::string& message)
    {
        std::cerr << "LiveLinkFaceTest: " << message << '\n';
        return 1;
    }

    void PushU32BE(std::vector<unsigned char>& out, unsigned int v)
    {
        out.push_back((unsigned char)(v >> 24)); out.push_back((unsigned char)(v >> 16));
        out.push_back((unsigned char)(v >> 8));  out.push_back((unsigned char)v);
    }
    void PushF32BE(std::vector<unsigned char>& out, float f)
    {
        unsigned int bits; std::memcpy(&bits, &f, sizeof(bits));
        PushU32BE(out, bits);
    }

    std::vector<unsigned char> BuildPacket(const std::string& subject, int frame, int fps,
                                           const float* values, int count = livelink::kPacketValues)
    {
        std::vector<unsigned char> p;
        const unsigned int version = 6;
        p.push_back((unsigned char)(version & 0xFF));          // little endian
        p.push_back((unsigned char)((version >> 8) & 0xFF));
        p.push_back((unsigned char)((version >> 16) & 0xFF));
        p.push_back((unsigned char)((version >> 24) & 0xFF));
        for (int i = 0; i < 37; ++i) p.push_back('A');         // device uuid
        PushU32BE(p, (unsigned int)subject.size());
        p.insert(p.end(), subject.begin(), subject.end());
        PushU32BE(p, (unsigned int)frame);
        PushF32BE(p, 0.25f);                                   // sub-frame
        PushU32BE(p, (unsigned int)fps);
        PushU32BE(p, 1);                                       // denominator
        p.push_back((unsigned char)count);
        for (int i = 0; i < livelink::kPacketValues; ++i) PushF32BE(p, values[i]);
        return p;
    }
}

int main()
{
    // Apple's list must name 52 distinct real ARKit shapes, or the remap has a
    // hole and some shape is never driven.
    {
        std::set<int> mapped;
        for (int i = 0; i < face::kShapeCount; ++i)
        {
            const int shape = livelink::AppleToCanonical()[i];
            if (shape == face::kShapeNone)
                return Fail(std::string("Apple order entry ") + std::to_string(i) + " ('" +
                            livelink::AppleShapeOrder()[i] + "') matches no ARKit shape");
            if (!mapped.insert(shape).second)
                return Fail(std::string("two packet slots map to ") + face::ShapeName(shape));
        }
        if (mapped.size() != (std::size_t)face::kShapeCount)
            return Fail("the remap does not cover all 52 shapes");
    }

    // The orders genuinely differ, so a straight copy would be wrong.
    if (livelink::AppleToCanonical()[0] == 0)
        return Fail("packet slot 0 should not be canonical 0 — the orders differ");
    if (livelink::AppleToCanonical()[17] != face::ShapeIndex("jawOpen"))
        return Fail("Apple index 17 must be jawOpen");
    if (livelink::AppleToCanonical()[0] != face::ShapeIndex("eyeBlinkLeft"))
        return Fail("Apple index 0 must be eyeBlinkLeft");
    if (livelink::AppleToCanonical()[51] != face::ShapeIndex("tongueOut"))
        return Fail("Apple index 51 must be tongueOut");

    // Round trip: fields survive, and each value lands on its named shape.
    float values[livelink::kPacketValues] = {};
    for (int i = 0; i < face::kShapeCount; ++i)
        values[i] = (float)i / 100.0f;              // distinct per slot
    values[55] = 12.0f; values[58] = 8.0f;          // eye yaw L/R
    values[56] = -4.0f; values[59] = -2.0f;         // eye pitch L/R

    const std::vector<unsigned char> packet = BuildPacket("iPhone", 1234, 60, values);
    livelink::Packet decoded;
    if (!livelink::Decode(packet.data(), (unsigned)packet.size(), decoded))
        return Fail("a well-formed packet failed to decode");
    if (decoded.subject != "iPhone") return Fail("subject name did not survive");
    if (decoded.frame != 1234)       return Fail("frame number did not survive");
    if (decoded.fps != 60)           return Fail("fps did not survive");
    if (std::fabs(decoded.subFrame - 0.25f) > 1e-6f) return Fail("sub-frame did not survive");

    float weights[face::kShapeCount] = {};
    livelink::ToWeights(decoded, weights, face::kShapeCount);
    for (int i = 0; i < face::kShapeCount; ++i)
    {
        const int shape = livelink::AppleToCanonical()[i];
        if (std::fabs(weights[shape] - values[i]) > 1e-5f)
            return Fail(std::string("packet slot ") + std::to_string(i) + " (" +
                        livelink::AppleShapeOrder()[i] + ") did not land on its shape");
    }

    // Malformed packets are rejected rather than half-read.
    {
        livelink::Packet ignored;
        if (livelink::Decode(packet.data(), 20, ignored))
            return Fail("a truncated packet decoded");
        std::vector<unsigned char> badCount = BuildPacket("iPhone", 1, 60, values, 52);
        if (livelink::Decode(badCount.data(), (unsigned)badCount.size(), ignored))
            return Fail("a packet claiming 52 values decoded");
        if (livelink::Decode(nullptr, 0, ignored))
            return Fail("a null packet decoded");
    }

    // Neutral calibration removes a resting face without flattening a full one.
    {
        livelink::Neutral neutral;
        float rest[face::kShapeCount] = {};
        const int brow = face::ShapeIndex("browInnerUp");
        const int jaw  = face::ShapeIndex("jawOpen");
        rest[brow] = 0.2f;

        neutral.Begin();
        for (int i = 0; i < 30; ++i) neutral.Add(rest, face::kShapeCount);
        rest[brow] = 0.9f;                       // one stray frame mid-calibration
        neutral.Add(rest, face::kShapeCount);
        if (!neutral.End()) return Fail("calibration with samples reported empty");

        float live[face::kShapeCount] = {};
        live[brow] = 0.2f; live[jaw] = 1.0f;
        neutral.Apply(live, face::kShapeCount);
        if (std::fabs(live[brow]) > 1e-4f)
            return Fail("the resting brow was not removed (median should ignore the stray frame)");
        if (live[jaw] < 0.99f)
            return Fail("a full expression was scaled down by calibration");

        livelink::Neutral empty;
        empty.Begin();
        if (empty.End()) return Fail("an empty calibration reported success");
        float untouched[face::kShapeCount] = {};
        untouched[jaw] = 0.5f;
        empty.Apply(untouched, face::kShapeCount);
        if (std::fabs(untouched[jaw] - 0.5f) > 1e-6f)
            return Fail("an invalid calibration still changed the weights");
    }

    std::cout << "LiveLinkFaceTest: ok\n";
    return 0;
}
