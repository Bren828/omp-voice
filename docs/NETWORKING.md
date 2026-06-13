# Networking & UDP ports

VoiceChat uses **UDP** only. There are three ports; defaults are in
[`shared/protocol.h`](../shared/protocol.h) and every one is overridable in config.

| Port (default) | Direction | Bound by | Expose? |
|----------------|-----------|----------|---------|
| **7779** audio   | client ⇄ relay (voice + handshake) | relay (`bindAny`) | **YES — open inbound on the server** |
| **7778** control | plugin/component → relay (loopback IPC) + status back | relay (`bindAny`) | **No — keep local** |
| **7780** cmd     | server → client (delivers the auth token) | client (`bindAny`) | **Client side** (see below) |

### 7779 — audio (the important one)
The relay listens here; every player's client connects to `server-ip:7779` to do the
encrypted handshake and stream voice. **This is the port you must open** on the machine
running the server/relay (and port-forward on your router if players come from the
internet).

### 7778 — control (loopback IPC)
The SA-MP plugin / open.mp component talks to the relay on `127.0.0.1:7778` (pushes
positions/flags/channels; receives talking/radio status back). It is **local to the
server host** — do **not** expose it to the internet. It's HMAC-authenticated with the
`[security]` IPC secret from `voice.ini`, but there's no reason for it to be reachable
remotely; firewall it to localhost.

### 7780 — cmd / token delivery
After `Voice_OnPlayerConnect`, the server sends the freshly minted token to the player at
`clientIP:7780` (legacy cmd bridge — the planned RakNet delivery is the NAT-friendly
upgrade). The **client** listens on 7780.
- **LAN:** works out of the box.
- **Internet / behind NAT:** the player may need inbound UDP 7780 reachable
  (port-forward) for the token to arrive. The client falls back to re-requesting if it
  doesn't get one.

## Opening the port (server: UDP 7779)

**Windows** (run as admin):
```powershell
netsh advfirewall firewall add rule name="VoiceChat audio 7779" dir=in action=allow protocol=UDP localport=7779
```

**Linux — ufw:**
```bash
sudo ufw allow 7779/udp
```

**Linux — iptables:**
```bash
sudo iptables -A INPUT -p udp --dport 7779 -j ACCEPT
```

**Router (players over the internet):** forward **UDP 7779** to the server's LAN IP. If
you also use the legacy token delivery for remote players, forward **UDP 7780** to each
player's machine (or rely on the client re-request).

## Changing the ports

**Server** — `voice.ini`:
```ini
[network]
control_port = 7778
audio_port   = 7779
```

**Client** — `voicechat.ini`:
```ini
[network]
relay_ip   = 192.168.0.178   ; the server's IP (127.0.0.1 if hosting locally)
audio_port = 7779            ; must match the server's audio_port
cmd_port   = 7780            ; where this client receives its token
```

`audio_port` must match on both sides. `relay_ip` is the only thing most players need to
change.
