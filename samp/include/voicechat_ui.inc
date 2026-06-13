// ============================================================
//  VoiceChat — "who's talking" indicator (open.mp + SA-MP 0.3.7 R5)
//  File: voicechat_ui.inc   (optional drop-in on top of <voicechat>)
//
//  Shows a small floating label above a player's head while they talk
//  (proximity) or key the radio — visible to everyone in range. Pure Pawn:
//  it rides the plugin's existing OnPlayerStartTalking / OnPlayerStopTalking /
//  OnPlayerRadioKey callbacks, so there is no extra wire traffic.
//
//  This include does NOT hook the callbacks itself (no ALS/y_hooks needed) —
//  call the VoiceUI_* helpers from your own callback bodies:
//
//      #include <voicechat>
//      #include <voicechat_ui>
//      public OnPlayerStartTalking(playerid) { VoiceUI_OnStartTalking(playerid); return 1; }
//      public OnPlayerStopTalking(playerid)  { VoiceUI_OnStopTalking(playerid);  return 1; }
//      public OnPlayerRadioKey(pid, ch, bool:down) { VoiceUI_OnRadioKey(pid, ch, down); return 1; }
//      public OnPlayerDisconnect(playerid, reason)  { VoiceUI_OnDisconnect(playerid); return 1; }
//
//  Tunables: #define any of VOICE_UI_* BEFORE including this file to override.
// ============================================================
#if defined _voicechat_ui_included
    #endinput
#endif
#define _voicechat_ui_included

#if !defined _voicechat_included
    #error Include <voicechat> BEFORE <voicechat_ui>
#endif

// ── Tunables ────────────────────────────────────────────────
#if !defined VOICE_UI_TALK_TEXT
    #define VOICE_UI_TALK_TEXT    "(( talking ))"
#endif
#if !defined VOICE_UI_RADIO_TEXT
    #define VOICE_UI_RADIO_TEXT   "(( radio ))"
#endif
#if !defined VOICE_UI_TALK_COLOR
    #define VOICE_UI_TALK_COLOR   (0x33FF33FF)   // green
#endif
#if !defined VOICE_UI_RADIO_COLOR
    #define VOICE_UI_RADIO_COLOR  (0xFFCC00FF)   // amber
#endif
#if !defined VOICE_UI_DRAW_DIST
    #define VOICE_UI_DRAW_DIST    (20.0)         // units the label is visible from
#endif
#if !defined VOICE_UI_OFFSET_Z
    #define VOICE_UI_OFFSET_Z     (0.40)         // height above the player origin
#endif

// One label per player per kind. A bool flag tracks existence so we never
// touch an uninitialised/zero id (label id 0 is a VALID, foreign label).
static Text3D:gVoiceUI_Talk [MAX_PLAYERS];
static bool:  gVoiceUI_HasTalk [MAX_PLAYERS];
static Text3D:gVoiceUI_Radio[MAX_PLAYERS];
static bool:  gVoiceUI_HasRadio[MAX_PLAYERS];

static stock Text3D:VoiceUI_MakeLabel(playerid, const text[], color, Float:offz)
{
    new Text3D:l = Create3DTextLabel(text, color, 0.0, 0.0, 0.0,
        VOICE_UI_DRAW_DIST, GetPlayerVirtualWorld(playerid), false);
    if (l != INVALID_3DTEXT_ID)
        Attach3DTextLabelToPlayer(l, playerid, 0.0, 0.0, offz);
    return l;
}

// ── Talking (proximity) ─────────────────────────────────────
stock VoiceUI_HideTalk(playerid)
{
    if (gVoiceUI_HasTalk[playerid]) {
        Delete3DTextLabel(gVoiceUI_Talk[playerid]);
        gVoiceUI_HasTalk[playerid] = false;
    }
}
stock VoiceUI_ShowTalk(playerid)
{
    VoiceUI_HideTalk(playerid);                 // refresh if already shown
    new Text3D:l = VoiceUI_MakeLabel(playerid, VOICE_UI_TALK_TEXT,
                                     VOICE_UI_TALK_COLOR, VOICE_UI_OFFSET_Z);
    if (l != INVALID_3DTEXT_ID) {
        gVoiceUI_Talk[playerid]    = l;
        gVoiceUI_HasTalk[playerid] = true;
    }
}

// ── Radio (offset a little higher so it never overlaps the talk label) ──
stock VoiceUI_HideRadio(playerid)
{
    if (gVoiceUI_HasRadio[playerid]) {
        Delete3DTextLabel(gVoiceUI_Radio[playerid]);
        gVoiceUI_HasRadio[playerid] = false;
    }
}
stock VoiceUI_ShowRadio(playerid)
{
    VoiceUI_HideRadio(playerid);
    new Text3D:l = VoiceUI_MakeLabel(playerid, VOICE_UI_RADIO_TEXT,
                                     VOICE_UI_RADIO_COLOR, VOICE_UI_OFFSET_Z + 0.18);
    if (l != INVALID_3DTEXT_ID) {
        gVoiceUI_Radio[playerid]    = l;
        gVoiceUI_HasRadio[playerid] = true;
    }
}

// ── Drop-in hooks: call these from your callback bodies ──────
stock VoiceUI_OnStartTalking(playerid) { VoiceUI_ShowTalk(playerid); }
stock VoiceUI_OnStopTalking(playerid)  { VoiceUI_HideTalk(playerid); }
stock VoiceUI_OnRadioKey(playerid, channelid, bool:down)
{
    #pragma unused channelid
    if (down) VoiceUI_ShowRadio(playerid);
    else      VoiceUI_HideRadio(playerid);
}
stock VoiceUI_OnDisconnect(playerid)   // clear both labels on leave / id reuse
{
    VoiceUI_HideTalk(playerid);
    VoiceUI_HideRadio(playerid);
}
