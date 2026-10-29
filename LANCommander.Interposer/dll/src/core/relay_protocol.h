#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Relay wire protocol, version 1. The managed side lives in
// LANCommander.Interposer.Relay\Protocol\ and the two must agree byte for byte;
// LANCommander.Interposer.Documentation\NetworkRelay.md is the reference.
//
// Every integer is big-endian. A string is a u16 byte count followed by UTF-8.
//
// TCP (control + tunnelled streams) carries frames:
//   u16 length   -- bytes that follow, type included
//   u8  type
//   ... body
//
// UDP (game datagrams) carries packets:
//   u32 clientId
//   u32 udpToken -- issued in Welcome; proves the packet came from that client
//   u8  type
//   ... body
//
// Datagram and stream bodies use "peer" and "local" from the point of view of
// whoever is reading them. A client sending names the destination as the peer
// and its own port as local; the server rewrites them so the receiving client
// sees the sender as the peer and its own port as local. That symmetry is what
// lets both sides share one encoder.
// ---------------------------------------------------------------------------

namespace relay
{
    constexpr uint8_t  kProtocolVersion   = 1;
    constexpr uint16_t kDefaultPort       = 41300;

    // A game datagram larger than this is dropped rather than relayed. It keeps
    // a datagram inside one TCP frame when it has to fall back to TCP.
    constexpr size_t   kMaxDatagram       = 60000;

    // Stream data is cut into chunks no larger than this.
    constexpr size_t   kMaxStreamChunk    = 16384;

    constexpr uint32_t kBroadcast         = 0xFFFFFFFFu;

    // Stream ids the server assigns to streams it delivers have the top bit set,
    // so they never collide with ids a client picks for streams it opens.
    constexpr uint32_t kServerStreamBit   = 0x80000000u;

    enum Proto : uint8_t
    {
        ProtoUdp = 0,
        ProtoIpx = 1,
    };

    enum FrameType : uint8_t
    {
        Hello        = 0x01, // C->S  u8 version, str network, str ticket, str player, u32 requestedVip
        Welcome      = 0x02, // S->C  u32 clientId, u32 udpToken, u32 vip, u32 netmask, u16 udpPort, u32 ipxNetwork
        Reject       = 0x03, // S->C  u8 code, str message
        PeerJoined   = 0x04, // S->C  u32 clientId, u32 vip, str name
        PeerLeft     = 0x05, // S->C  u32 clientId
        Ping         = 0x06, // both  u32 nonce
        Pong         = 0x07, // both  u32 nonce
        Datagram     = 0x10, // both  datagram body
        StreamOpen   = 0x20, // both  u32 streamId, u32 peerVip, u16 peerPort, u16 localPort
        StreamAccept = 0x21, // both  u32 streamId
        StreamReject = 0x22, // both  u32 streamId, u8 reason
        StreamData   = 0x23, // both  u32 streamId, bytes
        StreamClose  = 0x24, // both  u32 streamId -- sender has finished sending
        Leave        = 0x30, // C->S  (empty)

        UdpHello     = 0x41, // C->S over UDP, empty -- registers/refreshes the client's UDP endpoint
        UdpHelloAck  = 0x42, // S->C over UDP, empty
    };

    // Datagram body:
    //   u8  proto (Proto)
    //   u32 peerVip     -- kBroadcast for a broadcast, on the way in
    //   u16 peerPort    -- UDP port or IPX socket number
    //   u16 localPort
    //   u8  ipxType     -- IPX packet type; 0 for UDP
    //   ... payload
    constexpr size_t kDatagramHeader = 1 + 4 + 2 + 2 + 1;

    enum RejectCode : uint8_t
    {
        RejectProtocol     = 1,
        RejectUnauthorized = 2,
        RejectNetworkFull  = 3,
        RejectBadRequest   = 4,
    };

    enum StreamRejectReason : uint8_t
    {
        StreamNoSuchPeer = 1,
        StreamRefused    = 2,
        StreamLimit      = 3,
    };

    // ---- encoding helpers ------------------------------------------------

    class Writer
    {
    public:
        std::string buf;

        void U8(uint8_t v)   { buf.push_back(static_cast<char>(v)); }
        void U16(uint16_t v) { U8(static_cast<uint8_t>(v >> 8)); U8(static_cast<uint8_t>(v)); }
        void U32(uint32_t v) { U16(static_cast<uint16_t>(v >> 16)); U16(static_cast<uint16_t>(v)); }
        void Bytes(const void* p, size_t n) { buf.append(static_cast<const char*>(p), n); }

        void Str(const std::string& s)
        {
            const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
            U16(static_cast<uint16_t>(n));
            Bytes(s.data(), n);
        }
    };

    class Reader
    {
    public:
        Reader(const uint8_t* p, size_t n) : m_p(p), m_n(n) {}

        bool Ok() const          { return m_ok; }
        size_t Remaining() const { return m_ok ? m_n - m_pos : 0; }
        const uint8_t* Cursor() const { return m_p + m_pos; }

        uint8_t U8()
        {
            if (!Need(1)) return 0;
            return m_p[m_pos++];
        }

        uint16_t U16()
        {
            if (!Need(2)) return 0;
            uint16_t v = static_cast<uint16_t>((m_p[m_pos] << 8) | m_p[m_pos + 1]);
            m_pos += 2;
            return v;
        }

        uint32_t U32()
        {
            const uint32_t hi = U16();
            const uint32_t lo = U16();
            return (hi << 16) | lo;
        }

        std::string Str()
        {
            const uint16_t n = U16();
            if (!Need(n)) return {};
            std::string s(reinterpret_cast<const char*>(m_p + m_pos), n);
            m_pos += n;
            return s;
        }

    private:
        bool Need(size_t n)
        {
            if (!m_ok || m_n - m_pos < n)
            {
                m_ok = false;
                return false;
            }
            return true;
        }

        const uint8_t* m_p;
        size_t         m_n;
        size_t         m_pos = 0;
        bool           m_ok  = true;
    };

    // Wrap a body as one TCP frame.
    inline std::string Frame(FrameType type, const std::string& body)
    {
        Writer w;
        w.U16(static_cast<uint16_t>(body.size() + 1));
        w.U8(type);
        w.buf += body;
        return w.buf;
    }
}
