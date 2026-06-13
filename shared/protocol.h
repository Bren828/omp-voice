// ============================================================
//  VoiceChat — Shared Wire Protocol  (single source of truth)
//  File: shared/protocol.h
//
//  Included by EVERY module: /core, /relay, /samp, /omp, /client.
//  Nothing here may depend on a platform API or on SA-MP/open.mp.
//  Only fixed-width integers and POD structs that go on the wire.
//
//  Design notes
//  ------------
//  * Mixing model = HYBRID:
//      - Proximity voice is relayed per-speaker (opaque Opus) with a
//        server-computed volume/pan/occlusion; the CLIENT mixes it.
//      - Filtered channels (radio/phone) are decoded, filtered and
//        mixed PER-LISTENER on the SERVER into one bus stream, then
//        re-encoded; the client just plays the bus.
//  * Identity = cryptographic TOKEN, not source IP. The server (.dll)
//    mints a token at connect, ships it to the client over the already
//    authenticated channel, the client presents it in the UDP handshake,
//    and the relay binds the observed socket to that playerid.
//  * Every relay<->client datagram (after handshake) is wrapped in an
//    AEAD envelope (see shared/crypto.h): 24-byte nonce + ciphertext +
//    16-byte MAC. The structs below describe the PLAINTEXT payload.
// ============================================================
#pragma once

#include <cstdint>
#include <cstddef>

namespace vc {

// ── Versioning ───────────────────────────────────────────────
// Bump MAJOR on any incompatible wire change. Client and relay
// exchange this in the handshake and refuse mismatched MAJOR.
constexpr uint16_t PROTOCOL_VERSION_MAJOR = 2;
constexpr uint16_t PROTOCOL_VERSION_MINOR = 0;

// ── Fixed sizes ──────────────────────────────────────────────
constexpr int      MAX_PLAYERS        = 1000;      // hard cap (id space)
constexpr uint16_t INVALID_PLAYER     = 0xFFFF;
constexpr uint16_t INVALID_CHANNEL    = 0xFFFF;

constexpr int      SAMPLE_RATE        = 48000;     // relay-side DSP rate
constexpr int      CLIENT_SAMPLE_RATE = 24000;     // capture/playback rate
constexpr int      VOICE_CHANNELS     = 1;         // mono
constexpr int      FRAME_MS           = 20;        // Opus frame duration
constexpr int      MAX_OPUS_BYTES     = 1276;      // worst-case Opus frame
constexpr int      AUDIO_MTU          = 1400;      // payload ceiling per pkt

constexpr int      TOKEN_BYTES        = 32;        // crypto token length
constexpr int      SESSION_KEY_BYTES  = 32;        // XChaCha20-Poly1305 key
constexpr int      MAX_NAME_LEN       = 24;        // SA-MP MAX_PLAYER_NAME (G2 UI)

// J6 speakerphone: the relay broadcasts a phone-holder's call audio into their
// PROXIMITY as a synthetic speaker so bystanders hear it "out of the phone".
// The synthetic ProximityDown.speakerId = holderId | this bit, so it never
// collides with the holder's own mic stream (real ids are < MAX_PLAYERS).
constexpr uint16_t SPEAKERPHONE_ID_BIT = 0x8000;

// ── Default ports (override via voice.ini) ──────────────────
constexpr uint16_t DEFAULT_PORT_CONTROL = 7778;    // .dll  -> .exe  (loopback IPC)
constexpr uint16_t DEFAULT_PORT_AUDIO   = 7779;    // .asi <-> .exe  (audio/handshake)
constexpr uint16_t DEFAULT_PORT_CMD     = 7780;    // .dll  -> .asi  (legacy cmd bridge)

// ============================================================
//  Packet type tags (first plaintext byte of every datagram)
// ============================================================
enum class PktType : uint8_t {
    // ---- client <-> relay (audio plane, AEAD-wrapped) ----
    Handshake      = 0x01,  // C->R  first packet, carries token
    HandshakeAck   = 0x02,  // R->C  accept + effective limits
    HandshakeNak   = 0x03,  // R->C  reject (bad/expired token, version)
    AudioUp        = 0x10,  // C->R  one Opus frame from the speaker
    ProximityDown  = 0x11,  // R->C  one speaker's proximity voice
    BusDown        = 0x12,  // R->C  pre-mixed filtered bus (radio/phone)
    PlayerGone     = 0x13,  // R->C  a speaker left audible set (stop stream)
    PlayerName     = 0x14,  // R->C  id->name roster entry (overlay UI, G2)
    Ping           = 0x1A,  // C->R  keepalive / RTT probe
    Pong           = 0x1B,  // R->C  keepalive reply
    Bye            = 0x1F,  // C->R  graceful disconnect

    // ---- .dll -> relay (control plane, loopback IPC) ----
    CtrlBindToken  = 0x80,  // map token -> playerid (mint at OnPlayerConnect)
    CtrlConfig     = 0x81,  // push authoritative config (from voice.ini)
    CtrlPlayerPos  = 0x82,  // authoritative pos+interior+vw (anti-spoof)
    CtrlPlayerMeta = 0x83,  // mute/deafen/range/alive/in-vehicle flags
    CtrlChanCreate = 0x84,  // create channel (filter, positional, priority)
    CtrlChanDestroy= 0x85,  // destroy channel
    CtrlChanJoin   = 0x86,  // add player to channel
    CtrlChanLeave  = 0x87,  // remove player from channel
    CtrlChanFilter = 0x88,  // change channel filter
    CtrlChanPrio   = 0x89,  // change channel priority
    CtrlTransmit   = 0x8A,  // player PTT/VAD transmit on/off (+ channel mask)
    CtrlPlayerGone = 0x8B,  // player disconnected: full cleanup
    CtrlMixPolicy  = 0x8C,  // per-player mix policy (mix/duck/exclusive)
    CtrlPlayerName = 0x8D,  // map playerid -> display name (overlay UI, G2)
    CtrlPairMute   = 0x8E,  // listener mutes a single target (per-pair, stage G/J11)
    CtrlSpeakerphone=0x8F,  // toggle a player's phone-on-speaker (J6)

    // ---- relay -> .dll (status plane, loopback IPC, req. B) ----
    Status         = 0x90,  // relay-side event for the gamemode/bridge
};

// Status events the relay pushes back to the bridge (drive Pawn callbacks
// + client reconnect). req. B / G1.
enum class StatusType : uint8_t {
    SpeakerStart   = 1,   // a player began transmitting  -> OnPlayerStartTalking
    SpeakerStop    = 2,   // a player stopped             -> OnPlayerStopTalking
    SessionTimeout = 3,   // heartbeat lost -> .dll re-mints token (reconnect)
    RadioKey       = 4,   // PTT on a radio channel       -> OnPlayerRadioKey
};

// ============================================================
//  Channel filters & mixing policy
// ============================================================
enum class Filter : uint8_t {
    None   = 0,   // clean (proximity, plain talk)
    Radio  = 1,   // bandpass 300Hz-3kHz + squelch + garble on collision
    Phone  = 2,   // narrowband telephone colouring
};

enum class MixPolicy : uint8_t {
    Mix       = 0,  // all buses at equal volume
    Duck      = 1,  // low-priority buses duck when a higher one is active
    Exclusive = 2,  // only the highest-priority active bus is heard
};

// Proximity range presets (units). Whisper/Normal/Shout (req. D2).
enum class RangePreset : uint8_t {
    Whisper = 0,  // ~8
    Normal  = 1,  // ~18
    Shout   = 2,  // ~40
};

// Per-frame flags on AudioUp / ProximityDown.
enum AudioFlags : uint8_t {
    AF_VAD_ACTIVE = 1 << 0,  // speaker's VAD says voice present
    AF_PTT        = 1 << 1,  // PTT held
    AF_OCCLUDED   = 1 << 2,  // server: source occluded (apply lowpass)
    AF_SPEAKERPH  = 1 << 3,  // server: rendered as speakerphone source
};

#pragma pack(push, 1)

// ── Handshake: C -> R ────────────────────────────────────────
// Sent in CLEARTEXT (the very first datagram). The token itself is
// the secret; the relay replies with a per-session key for AEAD.
struct HandshakeReq {
    uint8_t  type;                 // PktType::Handshake
    uint16_t verMajor;
    uint16_t verMinor;
    uint8_t  token[TOKEN_BYTES];   // minted by .dll, delivered via game channel
    uint8_t  clientPubKey[32];     // X25519 ephemeral public key (key exchange)
};

// ── Handshake ack: R -> C ────────────────────────────────────
// Carries the relay ephemeral pubkey (client derives the shared
// session key) + the EFFECTIVE authoritative limits the client must
// obey (req. H: server dictates, client conforms).
struct HandshakeAck {
    uint8_t  type;                 // PktType::HandshakeAck
    uint16_t playerId;             // authoritative id bound to this socket
    uint8_t  relayPubKey[32];      // X25519 ephemeral public key
    // effective limits (authoritative; client clamps to these)
    uint8_t  vadAllowed;           // 0/1 may the client use VAD vs PTT-only
    uint8_t  defaultMode;          // 0 = PTT, 1 = VAD (server default)
    float    maxRange;             // hard ceiling on proximity range
    uint16_t opusBitrate;          // negotiated bitrate (e.g. 24000)
    uint8_t  maxConcurrentStreams; // cap on simultaneous decoded streams
};

struct HandshakeNak {
    uint8_t  type;                 // PktType::HandshakeNak
    uint8_t  reason;               // 0=bad token,1=expired,2=version,3=full
};

// ── Audio up: C -> R (AEAD payload) ──────────────────────────
// Client never reports position/channel — server is authoritative.
// It only says "here is my voice frame and my transmit intent".
struct AudioUp {
    uint8_t  type;                 // PktType::AudioUp
    uint16_t seq;                  // sequence (jitter buffer / replay)
    uint8_t  flags;                // AudioFlags (VAD/PTT)
    uint16_t length;               // Opus byte count
    // uint8_t data[length] follows
};

// ── Proximity down: R -> C (AEAD payload) ────────────────────
// One per audible speaker. Client decodes + spatialises + mixes.
struct ProximityDown {
    uint8_t  type;                 // PktType::ProximityDown
    uint16_t speakerId;
    uint16_t seq;
    uint8_t  flags;                // AudioFlags (AF_OCCLUDED/AF_SPEAKERPH)
    float    volume;               // 0..1 (log falloff, server computed)
    float    pan;                  // -1 left .. +1 right (stereo placement)
    uint16_t length;               // Opus byte count
    // uint8_t data[length] follows
};

// ── Bus down: R -> C (AEAD payload) ──────────────────────────
// A filtered channel pre-mixed PER LISTENER on the server. The
// client just plays it at the matching category volume. Filter,
// squelch and garble were already applied server-side.
struct BusDown {
    uint8_t  type;                 // PktType::BusDown
    uint16_t channelId;            // source channel (for category volume/UI)
    uint8_t  filter;               // Filter (for client UI only)
    uint16_t seq;
    float    volume;               // post-policy volume (duck/exclusive)
    uint16_t length;               // Opus byte count
    // uint8_t data[length] follows
};

struct PlayerGone {
    uint8_t  type;                 // PktType::PlayerGone
    uint16_t speakerId;
};

// ── Player name: R -> C (AEAD payload) ───────────────────────
// A roster entry so the client overlay can show names instead of bare ids.
// Sent on session establish (full roster) and whenever a name is set/changes.
// Names are display-only; identity stays the cryptographic token.
struct PlayerNameDown {
    uint8_t  type;                 // PktType::PlayerName
    uint16_t playerId;
    char     name[MAX_NAME_LEN];   // null-padded
};

struct PingPong {
    uint8_t  type;                 // Ping / Pong
    uint16_t playerId;
    uint32_t stamp;                // client tick for RTT
};

// ============================================================
//  Control plane: .dll -> relay (loopback, may be AEAD or HMAC'd
//  with a shared IPC secret from voice.ini [security]).
// ============================================================
struct CtrlBindToken {
    uint8_t  type;                 // PktType::CtrlBindToken
    uint16_t playerId;
    uint8_t  token[TOKEN_BYTES];   // same token shipped to the client
    uint32_t expiresUnix;          // token TTL (anti-replay window)
    uint32_t ip;                   // expected client IP (network order); 0 = no check
};

struct CtrlPlayerPos {
    uint8_t  type;                 // PktType::CtrlPlayerPos
    uint16_t playerId;
    float    x, y, z;
    uint16_t interior;             // req. J2: must match to hear by proximity
    uint16_t virtualWorld;         // req. J2
    uint8_t  vehicleState;         // 0=onfoot,1=open veh,2=closed veh (occlusion)
};

struct CtrlPlayerMeta {
    uint8_t  type;                 // PktType::CtrlPlayerMeta
    uint16_t playerId;
    uint8_t  flags;                // bit0 muted, bit1 deafen, bit2 alive, bit3 spectator
    uint8_t  rangePreset;          // RangePreset
    float    rangeOverride;        // <=0 = use preset; else explicit (clamped)
};

struct CtrlChanCreate {
    uint8_t  type;                 // PktType::CtrlChanCreate
    uint16_t channelId;            // allocated by .dll, mirrored on relay
    uint8_t  filter;               // Filter
    uint8_t  positional;           // 0/1 (is the channel spatialised?)
    uint8_t  priority;             // 0..255 (higher ducks lower)
};

struct CtrlChanOp {                // Destroy / Join / Leave / Filter / Prio
    uint8_t  type;
    uint16_t channelId;
    uint16_t playerId;             // for Join/Leave; INVALID for chan-wide ops
    uint8_t  arg;                  // Filter or priority depending on type
};

struct CtrlTransmit {
    uint8_t  type;                 // PktType::CtrlTransmit
    uint16_t playerId;
    uint8_t  on;                   // 0/1 transmitting
    uint32_t channelMask;          // bitmask of channel slots being keyed
};

struct CtrlMixPolicy {
    uint8_t  type;                 // PktType::CtrlMixPolicy
    uint16_t playerId;
    uint8_t  policy;               // MixPolicy
    float    duckLevel;            // 0=mute secondary .. 1=no duck
    float    proximityFloor;       // min proximity volume (never fully ducked)
};

// .dll -> relay: one listener mutes one target for THEMSELVES only (per-pair,
// req. J11/stage G). Pure server-side proximity gate; does not affect anyone
// else hearing the target. Channels are unaffected (gameplay decides those).
struct CtrlPairMute {
    uint8_t  type;                 // PktType::CtrlPairMute
    uint16_t listenerId;           // who is muting
    uint16_t targetId;             // whom they stop hearing
    uint8_t  on;                   // 1 = mute, 0 = unmute
};

// .dll -> relay: toggle a player's speakerphone (J6). While on, the filtered
// call audio that player receives is ALSO injected into their proximity so
// nearby players hear the call "on speaker". Gated server-wide by
// BehaviorTuning::speakerphone (voice.ini [behavior] speakerphone).
struct CtrlSpeakerphone {
    uint8_t  type;                 // PktType::CtrlSpeakerphone
    uint16_t playerId;
    uint8_t  on;                   // 1 = speaker on, 0 = off
};

// .dll -> relay: bind a display name to a playerid (overlay UI, G2). The relay
// caches it and fans it out to clients over their sealed sessions.
struct CtrlPlayerName {
    uint8_t  type;                 // PktType::CtrlPlayerName
    uint16_t playerId;
    char     name[MAX_NAME_LEN];   // null-padded
};

// Relay -> bridge event (loopback). One compact struct for all status
// types; unused fields are 0. (req. B)
struct StatusEvent {
    uint8_t  type;        // PktType::Status
    uint8_t  event;       // StatusType
    uint16_t playerId;
    uint16_t channelId;   // RadioKey only
    uint8_t  flag;        // RadioKey: 1 down / 0 up
};

// Generic config push mirrors voice.ini [audio]/[proximity]/[radio].
// Kept compact; relay applies authoritatively. (Filled in stage H.)
struct CtrlConfig {
    uint8_t  type;                 // PktType::CtrlConfig
    uint16_t opusBitrate;
    float    proxWhisper, proxNormal, proxShout;
    float    falloffExp;           // log/exponent curve shape
    uint8_t  occlusionEnabled;
    uint8_t  vadAllowed;
    uint8_t  defaultMode;          // 0 PTT / 1 VAD
    uint8_t  maxConcurrentStreams;
};

#pragma pack(pop)

// ── Helpers ──────────────────────────────────────────────────
inline size_t proximityHeaderSize() { return offsetof(ProximityDown, length) + sizeof(uint16_t); }
inline size_t busHeaderSize()       { return offsetof(BusDown, length) + sizeof(uint16_t); }
inline size_t audioUpHeaderSize()   { return offsetof(AudioUp, length) + sizeof(uint16_t); }

} // namespace vc
