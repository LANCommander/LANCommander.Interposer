namespace LANCommander.Interposer.Relay.Protocol
{
    /// <summary>
    /// Constants of relay wire protocol version 1. The native side is
    /// <c>dll\src\relay_protocol.h</c>; the two must agree byte for byte, and
    /// <c>NetworkRelay.md</c> in the documentation is the reference for both.
    /// <para>
    /// Every integer is big-endian; a string is a u16 byte count followed by UTF-8.
    /// TCP carries frames (<c>u16 length, u8 type, body</c>, where length counts the
    /// type byte). UDP carries packets (<c>u32 clientId, u32 udpToken, u8 type, body</c>).
    /// </para>
    /// </summary>
    public static class RelayProtocol
    {
        /// <summary>The protocol version a client sends in <see cref="FrameType.Hello"/>.</summary>
        public const byte Version = 1;

        /// <summary>The default TCP and UDP port.</summary>
        public const int DefaultPort = 41300;

        /// <summary>The largest game datagram relayed; anything larger is dropped.</summary>
        public const int MaxDatagram = 60000;

        /// <summary>The largest chunk of tunnelled stream data in one frame.</summary>
        public const int MaxStreamChunk = 16384;

        /// <summary>The virtual address a datagram is sent to for a broadcast.</summary>
        public const uint Broadcast = 0xFFFFFFFF;

        /// <summary>
        /// Set on every stream id the server assigns, so they never collide with the
        /// ids a client picks for the streams it opens.
        /// </summary>
        public const uint ServerStreamBit = 0x80000000;

        /// <summary>
        /// Size of a datagram body's header: proto, peer address, peer port, local
        /// port, IPX packet type.
        /// </summary>
        public const int DatagramHeaderSize = 1 + 4 + 2 + 2 + 1;

        /// <summary>Size of a UDP packet's header: client id, token, type.</summary>
        public const int UdpHeaderSize = 4 + 4 + 1;

        /// <summary>
        /// Set in the optional flags byte of <see cref="FrameType.Hello"/> by a client
        /// that can use direct paths, and so may be sent <see cref="FrameType.PeerPath"/>.
        /// </summary>
        public const byte HelloFlagDirect = 0x01;
    }

    /// <summary>Relay message types. Values are part of the wire protocol.</summary>
    public enum FrameType : byte
    {
        /// <summary>
        /// C→S: u8 version, str network, str ticket, str player, u32 requested address,
        /// then optionally u8 flags (<see cref="RelayProtocol.HelloFlagDirect"/>), u32 local
        /// IPv4 address, u16 local UDP port.
        /// </summary>
        Hello = 0x01,
        /// <summary>S→C: u32 clientId, u32 udpToken, u32 address, u32 netmask, u16 udpPort, u32 ipxNetwork.</summary>
        Welcome = 0x02,
        /// <summary>S→C: u8 <see cref="RejectCode"/>, str message. The connection is then closed.</summary>
        Reject = 0x03,
        /// <summary>S→C: u32 clientId, u32 address, str name.</summary>
        PeerJoined = 0x04,
        /// <summary>S→C: u32 clientId.</summary>
        PeerLeft = 0x05,
        /// <summary>Either way: u32 nonce.</summary>
        Ping = 0x06,
        /// <summary>Either way: u32 nonce, echoed.</summary>
        Pong = 0x07,
        /// <summary>
        /// S→C: u32 clientId, u32 pairKey, u8 count, then count × (u32 address, u16 port),
        /// best first: where another member might be reached directly, and the key the
        /// two of them sign their direct packets with.
        /// </summary>
        PeerPath = 0x08,
        /// <summary>Either way, TCP or UDP: a datagram body.</summary>
        Datagram = 0x10,
        /// <summary>Either way: u32 streamId, u32 peerAddress, u16 peerPort, u16 localPort.</summary>
        StreamOpen = 0x20,
        /// <summary>Either way: u32 streamId.</summary>
        StreamAccept = 0x21,
        /// <summary>Either way: u32 streamId, u8 <see cref="StreamRejectReason"/>.</summary>
        StreamReject = 0x22,
        /// <summary>Either way: u32 streamId, then the data.</summary>
        StreamData = 0x23,
        /// <summary>Either way: u32 streamId. The sender has finished sending.</summary>
        StreamClose = 0x24,
        /// <summary>C→S: the client is leaving.</summary>
        Leave = 0x30,
        /// <summary>C→S over UDP: register or refresh the client's UDP endpoint.</summary>
        UdpHello = 0x41,
        /// <summary>S→C over UDP: the endpoint is registered.</summary>
        UdpHelloAck = 0x42,
        /// <summary>C→C over UDP, never seen by the server: u32 nonce. Opens and keeps a direct path.</summary>
        PeerProbe = 0x43,
        /// <summary>C→C over UDP, never seen by the server: u32 nonce, echoed.</summary>
        PeerProbeAck = 0x44,
    }

    /// <summary>The transport a relayed datagram stands for.</summary>
    public enum RelayDatagramProtocol : byte
    {
        /// <summary>A UDP datagram; the ports are UDP ports.</summary>
        Udp = 0,
        /// <summary>An IPX datagram; the ports are IPX socket numbers.</summary>
        Ipx = 1,
    }

    /// <summary>Why a <see cref="FrameType.Hello"/> was refused.</summary>
    public enum RejectCode : byte
    {
        /// <summary>The client speaks a protocol version the server does not.</summary>
        Protocol = 1,
        /// <summary>The authenticator refused the ticket.</summary>
        Unauthorized = 2,
        /// <summary>The network has no free addresses or has reached its member limit.</summary>
        NetworkFull = 3,
        /// <summary>The Hello was malformed.</summary>
        BadRequest = 4,
    }

    /// <summary>Why a <see cref="FrameType.StreamOpen"/> was refused.</summary>
    public enum StreamRejectReason : byte
    {
        /// <summary>Nobody in the network has the destination address.</summary>
        NoSuchPeer = 1,
        /// <summary>The destination refused it, usually because nothing listens on the port.</summary>
        Refused = 2,
        /// <summary>The client has too many streams open.</summary>
        Limit = 3,
    }
}
