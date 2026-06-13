// ============================================================
//  VoiceChat v2 — full test filterscript (open.mp / SA-MP 0.3.7 R5)
//  File: examples/voicechat_test.pwn
//
//  A FILTERSCRIPT (does NOT replace your gamemode) that exposes a chat command
//  for EVERY native + callback in <voicechat>, so each feature can be tested
//  individually. Type /vhelp in game for the full list.
//
//  Channels created on load: proximity (always-on), a global RADIO bus, and a
//  PHONE bus — so the radio/phone DSP filters can be exercised separately.
//
//  Compile with qawno/pawno; put voicechat.inc + voicechat_ui.inc + sscanf2 in
//  the include dir. Add "filterscripts/voicechat_test" to config.json side_scripts.
// ============================================================
#include <open.mp>     // SA-MP 0.3.7 R5 server: replace with  #include <a_samp>
#include <voicechat>
#include <voicechat_ui>   // floating "who's talking" / radio labels above heads
#include <sscanf2>

new gProximity = -1;            // clean positional channel everyone joins
new gRadio     = -1;            // global RADIO-filter bus (bandpass + squelch)
new gPhone     = -1;            // global PHONE-filter bus (narrower bandpass)

new bool:gInRadio[MAX_PLAYERS];
new bool:gInPhone[MAX_PLAYERS];
new bool:gSpeaker[MAX_PLAYERS];   // J6 speakerphone toggle

// Player flags mirror (Voice_SetPlayerFlags sets all four at once, so we keep
// the current state and re-send the whole set when one toggles).
new bool:gMuted    [MAX_PLAYERS];
new bool:gDeafen   [MAX_PLAYERS];
new bool:gAlive    [MAX_PLAYERS];
new bool:gSpectator[MAX_PLAYERS];

ApplyFlags(playerid)
{
    Voice_SetPlayerFlags(playerid, gMuted[playerid], gDeafen[playerid],
                         gAlive[playerid], gSpectator[playerid]);
}

SetupVoice(playerid)
{
    new ip[16];
    GetPlayerIp(playerid, ip, sizeof ip);
    Voice_OnPlayerConnect(playerid, ip);              // mint + deliver token (req. A)
    Voice_AddToChannel(playerid, gProximity);          // hear/your voice on proximity
    Voice_SetPlayerRange(playerid, VOICE_RANGE_NORMAL);

    gInRadio[playerid]   = false;
    gInPhone[playerid]   = false;
    gSpeaker[playerid]   = false;
    gMuted[playerid]     = false;
    gDeafen[playerid]    = false;
    gAlive[playerid]     = true;
    gSpectator[playerid] = false;
    ApplyFlags(playerid);
}

public OnFilterScriptInit()
{
    print("[voicechat_test] starting");
    gProximity = Voice_CreateChannel(VOICE_FILTER_NONE,  true,  1);    // positional floor
    gRadio     = Voice_CreateChannel(VOICE_FILTER_RADIO, false, 150);  // bandpass + garble
    gPhone     = Voice_CreateChannel(VOICE_FILTER_PHONE, false, 200);  // narrow bandpass
    printf("[voicechat_test] proximity=%d radio=%d phone=%d", gProximity, gRadio, gPhone);
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) SetupVoice(i);
    return 1;
}

public OnFilterScriptExit()
{
    if (gPhone != -1) Voice_DestroyChannel(gPhone);
    if (gRadio != -1) Voice_DestroyChannel(gRadio);
    if (gProximity != -1) Voice_DestroyChannel(gProximity);
    return 1;
}

public OnPlayerConnect(playerid)
{
    SetupVoice(playerid);
    SendClientMessage(playerid, 0x33CCFFFF, "VoiceChat v2 test loaded. Type /vhelp for all test commands.");
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    Voice_OnPlayerDisconnect(playerid);                // full cleanup (req. J8)
    VoiceUI_OnDisconnect(playerid);                    // drop any floating labels
    gInRadio[playerid] = false;
    gInPhone[playerid] = false;
    gSpeaker[playerid] = false;
    return 1;
}

// Server is authoritative for position/interior/VW (req. J10): push every update.
public OnPlayerUpdate(playerid)
{
    new Float:x, Float:y, Float:z;
    GetPlayerPos(playerid, x, y, z);
    new vehState = (GetPlayerVehicleID(playerid) != 0) ? VOICE_VEH_CLOSED : VOICE_ONFOOT;
    Voice_UpdatePosition(playerid, x, y, z,
        GetPlayerInterior(playerid), GetPlayerVirtualWorld(playerid), vehState);
    return 1;
}

// Hold N = PTT on EVERY bus channel you've joined (radio and/or phone). This
// exercises the channelMask (a bit per channel id).
public OnPlayerKeyStateChange(playerid, KEY:newkeys, KEY:oldkeys)
{
    if ((newkeys & KEY_NO) && !(oldkeys & KEY_NO)) {
        new mask = 0;
        if (gInRadio[playerid]) mask |= (1 << (gRadio & 31));
        if (gInPhone[playerid]) mask |= (1 << (gPhone & 31));
        if (mask) Voice_SetTransmitting(playerid, true, mask);
    } else if (!(newkeys & KEY_NO) && (oldkeys & KEY_NO)) {
        Voice_SetTransmitting(playerid, false, 0);
    }
    return 1;
}

// ============================================================
//  Test commands — one per native, grouped. /vhelp lists them.
// ============================================================
public OnPlayerCommandText(playerid, cmdtext[])
{
    new cmd[24], params[128];
    if (sscanf(cmdtext, "s[24]S()[128]", cmd, params)) return 0;

    // ---- help / status ----
    if (!strcmp(cmd, "/vhelp", true)) {
        SendClientMessage(playerid, 0x33CCFFFF, "== VoiceChat test ==  PTT: hold B (proximity), hold N (radio/phone)");
        SendClientMessage(playerid, 0xFFFFFFAA, "Range:  /vrange whisper|normal|shout   /vrangeu <units>");
        SendClientMessage(playerid, 0xFFFFFFAA, "Buses:  /vradio   /vphone   /vinchan   /vspeaker");
        SendClientMessage(playerid, 0xFFFFFFAA, "Channel:/vfilter none|radio|phone   /vprio <n>   /vchans");
        SendClientMessage(playerid, 0xFFFFFFAA, "Flags:  /vmute  /vdeafen  /valive  /vspec");
        SendClientMessage(playerid, 0xFFFFFFAA, "Mix:    /vmix mix|duck|exclusive [duckLevel] [floor]");
        SendClientMessage(playerid, 0xFFFFFFAA, "Pair:   /vpmute <id>   /vpunmute <id>");
        SendClientMessage(playerid, 0xFFFFFFAA, "Config: /vreload        Info: /vstatus");
        return 1;
    }
    if (!strcmp(cmd, "/vstatus", true)) {
        new s[160];
        format(s, sizeof s, "radio=%s phone=%s speaker=%s | muted=%s deafen=%s alive=%s spec=%s",
            gInRadio[playerid]?"Y":"n", gInPhone[playerid]?"Y":"n", gSpeaker[playerid]?"Y":"n",
            gMuted[playerid]?"Y":"n", gDeafen[playerid]?"Y":"n",
            gAlive[playerid]?"Y":"n", gSpectator[playerid]?"Y":"n");
        SendClientMessage(playerid, 0x33CCFFFF, s);
        format(s, sizeof s, "inProximity=%d inRadio=%d inPhone=%d",
            Voice_IsInChannel(playerid, gProximity),
            Voice_IsInChannel(playerid, gRadio),
            Voice_IsInChannel(playerid, gPhone));
        SendClientMessage(playerid, 0x33CCFFFF, s);
        return 1;
    }

    // ---- proximity range (req. D2) ----
    if (!strcmp(cmd, "/vrange", true)) {
        if (!strcmp(params, "whisper", true)) { Voice_SetPlayerRange(playerid, VOICE_RANGE_WHISPER); return SendClientMessage(playerid, 0x33CCFFFF, "Range: whisper (~8u)"); }
        if (!strcmp(params, "normal",  true)) { Voice_SetPlayerRange(playerid, VOICE_RANGE_NORMAL);  return SendClientMessage(playerid, 0x33CCFFFF, "Range: normal (~18u)"); }
        if (!strcmp(params, "shout",   true)) { Voice_SetPlayerRange(playerid, VOICE_RANGE_SHOUT);   return SendClientMessage(playerid, 0x33CCFFFF, "Range: shout (~40u)"); }
        return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vrange whisper|normal|shout");
    }
    if (!strcmp(cmd, "/vrangeu", true)) {
        new Float:u;
        if (sscanf(params, "f", u) || u <= 0.0) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vrangeu <units>  (e.g. 25.0)");
        Voice_SetPlayerRange(playerid, VOICE_RANGE_NORMAL, u);   // preset + custom override
        new s[64]; format(s, sizeof s, "Range override: %.1f units", u);
        return SendClientMessage(playerid, 0x33CCFFFF, s);
    }

    // ---- bus membership (req. I1) ----
    if (!strcmp(cmd, "/vradio", true)) {
        if (gInRadio[playerid]) { Voice_RemoveFromChannel(playerid, gRadio); gInRadio[playerid] = false; return SendClientMessage(playerid, 0xFFCC00FF, "[Radio] left frequency."); }
        Voice_AddToChannel(playerid, gRadio); gInRadio[playerid] = true;
        return SendClientMessage(playerid, 0xFFCC00FF, "[Radio] joined. Hold N to talk.");
    }
    if (!strcmp(cmd, "/vphone", true)) {
        if (gInPhone[playerid]) { Voice_RemoveFromChannel(playerid, gPhone); gInPhone[playerid] = false; return SendClientMessage(playerid, 0xCC99FFFF, "[Phone] hung up."); }
        Voice_AddToChannel(playerid, gPhone); gInPhone[playerid] = true;
        return SendClientMessage(playerid, 0xCC99FFFF, "[Phone] connected. Hold N to talk.");
    }
    if (!strcmp(cmd, "/vinchan", true)) {
        new s[96]; format(s, sizeof s, "IsInChannel -> proximity:%d radio:%d phone:%d",
            Voice_IsInChannel(playerid, gProximity), Voice_IsInChannel(playerid, gRadio), Voice_IsInChannel(playerid, gPhone));
        return SendClientMessage(playerid, 0x33CCFFFF, s);
    }
    // J6 speakerphone: bystanders near you hear your phone/radio call out loud.
    if (!strcmp(cmd, "/vspeaker", true)) {
        gSpeaker[playerid] = !gSpeaker[playerid];
        Voice_SetSpeakerphone(playerid, gSpeaker[playerid]);
        new s[64]; format(s, sizeof s, "Speakerphone %s", gSpeaker[playerid] ? "ON (nearby players hear your call)" : "off");
        return SendClientMessage(playerid, 0xCC99FFFF, s);
    }

    // ---- live channel tweaks on the RADIO channel (req. I2) ----
    if (!strcmp(cmd, "/vfilter", true)) {
        new f = -1;
        if (!strcmp(params, "none",  true)) f = VOICE_FILTER_NONE;
        else if (!strcmp(params, "radio", true)) f = VOICE_FILTER_RADIO;
        else if (!strcmp(params, "phone", true)) f = VOICE_FILTER_PHONE;
        if (f == -1) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vfilter none|radio|phone  (applies to the radio channel)");
        Voice_SetChannelFilter(gRadio, f);
        new s[64]; format(s, sizeof s, "Radio channel filter -> %d", f);
        return SendClientMessage(playerid, 0xFFCC00FF, s);
    }
    if (!strcmp(cmd, "/vprio", true)) {
        new p;
        if (sscanf(params, "i", p)) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vprio <priority>");
        Voice_SetChannelPriority(gRadio, p);
        new s[64]; format(s, sizeof s, "Radio channel priority -> %d", p);
        return SendClientMessage(playerid, 0xFFCC00FF, s);
    }
    // Channel queries (req. I): count, validity, member counts + member ids.
    if (!strcmp(cmd, "/vchans", true)) {
        new s[144];
        format(s, sizeof s, "Channels: %d total | proximity(%d) valid=%d members=%d",
            Voice_GetChannelCount(), gProximity, Voice_IsValidChannel(gProximity),
            Voice_GetChannelMemberCount(gProximity));
        SendClientMessage(playerid, 0x33CCFFFF, s);
        new ids[8], n = Voice_GetChannelPlayers(gRadio, ids, sizeof ids), list[128] = "Radio members:";
        for (new i = 0; i < n; i++) { new e[8]; format(e, sizeof e, " %d", ids[i]); strcat(list, e); }
        if (n == 0) strcat(list, " (none)");
        SendClientMessage(playerid, 0xFFCC00FF, list);
        return 1;
    }

    // ---- player flags (req. J): muted / deafen / alive / spectator ----
    if (!strcmp(cmd, "/vmute", true))   { gMuted[playerid]     = !gMuted[playerid];     ApplyFlags(playerid); new s[40]; format(s,sizeof s,"flag muted=%s",    gMuted[playerid]?"ON":"off");     return SendClientMessage(playerid, 0x33CCFFFF, s); }
    if (!strcmp(cmd, "/vdeafen", true)) { gDeafen[playerid]    = !gDeafen[playerid];    ApplyFlags(playerid); new s[40]; format(s,sizeof s,"flag deafen=%s",   gDeafen[playerid]?"ON":"off");    return SendClientMessage(playerid, 0x33CCFFFF, s); }
    if (!strcmp(cmd, "/valive", true))  { gAlive[playerid]     = !gAlive[playerid];     ApplyFlags(playerid); new s[40]; format(s,sizeof s,"flag alive=%s",    gAlive[playerid]?"ON":"off");     return SendClientMessage(playerid, 0x33CCFFFF, s); }
    if (!strcmp(cmd, "/vspec", true))   { gSpectator[playerid] = !gSpectator[playerid]; ApplyFlags(playerid); new s[40]; format(s,sizeof s,"flag spectator=%s",gSpectator[playerid]?"ON":"off"); return SendClientMessage(playerid, 0x33CCFFFF, s); }

    // ---- mix policy (req. I4) ----
    if (!strcmp(cmd, "/vmix", true)) {
        new pol[16]; new Float:duck, Float:floor;
        if (sscanf(params, "s[16]F(0.5)F(0.25)", pol, duck, floor)) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vmix mix|duck|exclusive [duckLevel] [floor]");
        new policy = -1;
        if (!strcmp(pol, "mix",       true)) policy = VOICE_MIX;
        else if (!strcmp(pol, "duck",      true)) policy = VOICE_DUCK;
        else if (!strcmp(pol, "exclusive", true)) policy = VOICE_EXCLUSIVE;
        if (policy == -1) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vmix mix|duck|exclusive [duckLevel] [floor]");
        Voice_SetMixPolicy(playerid, policy, duck, floor);
        new s[80]; format(s, sizeof s, "Mix policy=%s duck=%.2f floor=%.2f", pol, duck, floor);
        return SendClientMessage(playerid, 0x33CCFFFF, s);
    }

    // ---- per-pair mute (req. J11) ----
    if (!strcmp(cmd, "/vpmute", true) || !strcmp(cmd, "/vpunmute", true)) {
        new tid; new bool:on = !strcmp(cmd, "/vpmute", true);
        if (sscanf(params, "i", tid) || !IsPlayerConnected(tid)) return SendClientMessage(playerid, 0xFF8888FF, "Usage: /vpmute <id>  |  /vpunmute <id>");
        Voice_MutePlayer(playerid, tid, on);
        new s[64]; format(s, sizeof s, "%s player %d", on ? "Muted" : "Unmuted", tid);
        return SendClientMessage(playerid, 0x33CCFFFF, s);
    }

    // ---- runtime config reload (req. H) ----
    if (!strcmp(cmd, "/vreload", true)) {
        Voice_ReloadConfig();
        return SendClientMessage(playerid, 0x33CCFFFF, "voice.ini re-read + pushed to relay.");
    }

    return 0;
}

// ===== Callbacks fired BY the plugin into the script (Stage B) =====
public OnPlayerStartTalking(playerid)
{
    printf("[voice] player %d START talking", playerid);
    VoiceUI_OnStartTalking(playerid);    // green "(( talking ))" above the head
    return 1;
}

public OnPlayerStopTalking(playerid)
{
    printf("[voice] player %d STOP talking", playerid);
    VoiceUI_OnStopTalking(playerid);     // remove the label
    return 1;
}

public OnPlayerRadioKey(playerid, channelid, bool:down)
{
    VoiceUI_OnRadioKey(playerid, channelid, down);   // amber "(( radio ))" while keyed
    new name[MAX_PLAYER_NAME]; GetPlayerName(playerid, name, sizeof name);
    new msg[96];
    format(msg, sizeof msg, "[Radio ch %d] %s %s", channelid, name,
        down ? "keys up" : "stops");
    SendClientMessageToAll(0xFFCC00FF, msg);
    return 1;
}
