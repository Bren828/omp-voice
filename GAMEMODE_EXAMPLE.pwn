// ============================================================
//  Complete gamemode example: VoiceChat + GPS/Phone + Services
//  Drop-in template. Tested against the plugins in voicechat-final-v2.
//  Required includes (put in pawno/include/):
//    voicechat.inc   (from voicechat-plugin/plugin/include/)
//    gpsphone.inc    (from open.mp - plugins/gps-phone-asi/)
//    services.inc    (from open.mp - plugins/services-asi/pawn/)
// ============================================================

#include <a_samp>
#include <sscanf2>           // optional, comment out if unavailable
#include <zcmd>              // optional, comment out if unavailable

#include <voicechat>         // provides Voice_*, ASI_RegisterPlayerIP, ASI_SendCommand
#include <gpsphone>          // provides GPS_*, Phone_*
#include <services>          // provides Notify, Taxi_*, Police_*, etc.

// ---------- Faction example IDs ------------------------------
#define FACTION_NONE    0
#define FACTION_POLICE  1
#define FACTION_MEDIC   2
#define FACTION_TAXI    3

new gPlayerFaction[MAX_PLAYERS];

// Hospital location (used by GPS hotword "spital")
#define LOC_HOSPITAL_X  1172.0
#define LOC_HOSPITAL_Y -1323.0
#define LOC_HOSPITAL_Z   15.4

// =============================================================
// GAMEMODE INIT
// =============================================================
public OnGameModeInit()
{
    SetGameModeText("VoiceChat + Services RP");
    AddPlayerClass(0, 0.0, 0.0, 3.0, 0.0, 0, 0, 0, 0, 0, 0);

    // 1) Voice channels: proximity + faction radios
    VoiceChat_Init();   // creates global proximity channel + faction radios

    print("[GM] Initialized voice channels & services bridge.");
    return 1;
}

// =============================================================
// CONNECT / DISCONNECT
// =============================================================
public OnPlayerConnect(playerid)
{
    // --- Critical: register the player's IP with the plugin so
    // ASI_SendCommand can route commands to this client's ASI ---
    new ip[16];
    GetPlayerIp(playerid, ip, sizeof(ip));
    ASI_RegisterPlayerIP(playerid, ip);

    // Join proximity voice channel
    Voice_AddPlayer(playerid, g_ChanProximity);

    // Seed phone contacts (faction dispatchers)
    Phone_AddContact(playerid, 0, "Dispecerat Politie",   "Politie",    true);
    Phone_AddContact(playerid, 0, "Dispecerat Ambulanta", "Ambulanta",  true);
    Phone_AddContact(playerid, 0, "Taxi Central",         "Civili",     true);

    SendClientMessage(playerid, 0x00FF00FF,
        "[VoiceChat] Hold B to talk. F2: phone. /servicii: services menu.");
    return 1;
}

public OnPlayerDisconnect(playerid, reason)
{
    Voice_OnPlayerDisconnect(playerid);
    gPlayerFaction[playerid] = FACTION_NONE;
    return 1;
}

// =============================================================
// VOICE: keep positions fresh so proximity works
// =============================================================
public OnPlayerUpdate(playerid)
{
    new Float:x, Float:y, Float:z;
    GetPlayerPos(playerid, x, y, z);
    Voice_UpdatePosition(playerid, x, y, z);
    return 1;
}

// =============================================================
// COMMAND: /servicii  -> open the Services screen in the phone
// =============================================================
CMD:servicii(playerid, params[])
{
    Services_Open(playerid);
    SendClientMessage(playerid, -1, "Servicii deschise in telefon (F2 daca nu apare).");
    return 1;
}

// =============================================================
// COMMAND: /gps spital | politie | aeroport
// =============================================================
CMD:gps(playerid, params[])
{
    if (!strcmp(params, "spital", true)) {
        GPS_SetDestination(playerid, LOC_HOSPITAL_X, LOC_HOSPITAL_Y, LOC_HOSPITAL_Z, "Spital");
        SendClientMessage(playerid, 0x00FF00FF, "[GPS] Ruta catre Spital activata.");
    } else if (!strcmp(params, "politie", true)) {
        GPS_GoTo(playerid, GPS_LOC_POLITIE_LS, "Sectia Politiei");
        SendClientMessage(playerid, 0x00FF00FF, "[GPS] Ruta catre Politie activata.");
    } else if (!strcmp(params, "aeroport", true)) {
        GPS_GoTo(playerid, GPS_LOC_AEROPORT, "Aeroport LS");
        SendClientMessage(playerid, 0x00FF00FF, "[GPS] Ruta catre Aeroport activata.");
    } else if (!strcmp(params, "off", true)) {
        GPS_Clear(playerid);
        SendClientMessage(playerid, -1, "[GPS] Sters.");
    } else {
        SendClientMessage(playerid, -1, "Folosire: /gps spital | politie | aeroport | off");
    }
    return 1;
}

// =============================================================
// COMMAND: /sms [playerid] [text]
// =============================================================
CMD:sms(playerid, params[])
{
    new targetID, text[141];
    if (sscanf(params, "ds[141]", targetID, text))
        return SendClientMessage(playerid, -1, "Folosire: /sms [id] [mesaj]");
    if (!IsPlayerConnected(targetID))
        return SendClientMessage(playerid, -1, "Jucatorul nu e online.");

    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    Phone_SendSMS(targetID, playerid, myName, text);
    SendClientMessage(playerid, 0x00FF00FF, "[SMS] Trimis.");
    return 1;
}

// =============================================================
// COMMAND: /suna [playerid]  -> creates a private voice channel
// =============================================================
CMD:suna(playerid, params[])
{
    new targetID;
    if (sscanf(params, "d", targetID))
        return SendClientMessage(playerid, -1, "Folosire: /suna [id]");
    if (!IsPlayerConnected(targetID))
        return SendClientMessage(playerid, -1, "Jucatorul nu e online.");

    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    Phone_Call(targetID, playerid, myName);

    new chanName[32]; format(chanName, sizeof(chanName), "Call_%d_%d", playerid, targetID);
    new chanID = Voice_CreateChannel(VCHAN_TEAM, chanName);
    Voice_AddPlayer(playerid, chanID);
    Voice_AddPlayer(targetID, chanID);
    SendClientMessage(playerid, 0x00FF00FF, "[Telefon] Apel pornit.");
    return 1;
}

// =============================================================
// COMMAND: /taxi  -> request a taxi (notifies all taxi drivers)
// =============================================================
CMD:taxi(playerid, params[])
{
    if (gPlayerFaction[playerid] == FACTION_TAXI)
        return SendClientMessage(playerid, -1, "Esti deja taxist.");

    SendClientMessage(playerid, 0x00FF00FF, "[Taxi] Comanda trimisa.");

    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    new Float:px, Float:py, Float:pz; GetPlayerPos(playerid, px, py, pz);

    new msg[128];
    format(msg, sizeof(msg), "Comanda taxi: %s la %.0f, %.0f", myName, px, py);
    for (new i = 0; i < MAX_PLAYERS; i++) {
        if (!IsPlayerConnected(i)) continue;
        if (gPlayerFaction[i] != FACTION_TAXI) continue;
        Phone_SendSMS(i, 0, "Dispecerat Taxi", msg);
    }
    return 1;
}

// =============================================================
// COMMAND: /accepttaxi [customerID]  (called by a taxi driver)
// =============================================================
CMD:accepttaxi(playerid, params[])
{
    if (gPlayerFaction[playerid] != FACTION_TAXI)
        return SendClientMessage(playerid, -1, "Nu esti taxist.");
    new customerID;
    if (sscanf(params, "d", customerID))
        return SendClientMessage(playerid, -1, "Folosire: /accepttaxi [id]");
    if (!IsPlayerConnected(customerID))
        return SendClientMessage(playerid, -1, "Jucatorul nu e online.");

    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    Taxi_DriverEnRoute(customerID, playerid, myName, 120.0);

    new Float:cx, Float:cy, Float:cz; GetPlayerPos(customerID, cx, cy, cz);
    GPS_SetDestination(playerid, cx, cy, cz, "Client taxi");
    SendClientMessage(playerid, 0x00FF00FF, "[Taxi] Comanda acceptata. GPS setat.");
    return 1;
}

// =============================================================
// COMMAND: /911  -> emergency ambulance
// =============================================================
CMD:911(playerid, params[])
{
    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    new msg[96];
    format(msg, sizeof(msg), "URGENTA: %s are nevoie de ajutor.", myName);
    for (new i = 0; i < MAX_PLAYERS; i++) {
        if (!IsPlayerConnected(i)) continue;
        if (gPlayerFaction[i] != FACTION_MEDIC) continue;
        Phone_SendSMS(i, 0, "Dispecerat 112", msg);
    }
    SendClientMessage(playerid, 0xFF0000FF, "[112] Paramedicii au fost alertati.");
    return 1;
}

CMD:acceptmedic(playerid, params[])
{
    if (gPlayerFaction[playerid] != FACTION_MEDIC)
        return SendClientMessage(playerid, -1, "Nu esti paramedic.");
    new patientID;
    if (sscanf(params, "d", patientID))
        return SendClientMessage(playerid, -1, "Folosire: /acceptmedic [id]");
    if (!IsPlayerConnected(patientID))
        return SendClientMessage(playerid, -1, "Jucatorul nu e online.");

    new myName[MAX_PLAYER_NAME]; GetPlayerName(playerid, myName, sizeof(myName));
    Ambulance_EnRoute(patientID, myName, 90.0);
    new Float:px, Float:py, Float:pz; GetPlayerPos(patientID, px, py, pz);
    GPS_SetDestination(playerid, px, py, pz, "Pacient");
    return 1;
}

// =============================================================
// COMMAND: /arrest [playerid] [reason]  (police only)
// =============================================================
CMD:arrest(playerid, params[])
{
    if (gPlayerFaction[playerid] != FACTION_POLICE)
        return SendClientMessage(playerid, -1, "Nu esti politist.");
    new targetID, reason[64];
    if (sscanf(params, "ds[64]", targetID, reason))
        return SendClientMessage(playerid, -1, "Folosire: /arrest [id] [motiv]");

    Police_IssueWarrant(targetID, reason, 3);
    Police_Arrested(targetID);
    SendClientMessage(playerid, 0x00FF00FF, "[Politie] Jucator arestat.");
    return 1;
}

// =============================================================
// COMMAND: /vmute / /vunmute  (admin)
// =============================================================
CMD:vmute(playerid, params[])
{
    if (!IsPlayerAdmin(playerid)) return 0;
    new t; if (sscanf(params, "d", t)) return 1;
    Voice_SetMuted(t, true);
    SendClientMessage(playerid, -1, "Jucator muted.");
    return 1;
}
CMD:vunmute(playerid, params[])
{
    if (!IsPlayerAdmin(playerid)) return 0;
    new t; if (sscanf(params, "d", t)) return 1;
    Voice_SetMuted(t, false);
    SendClientMessage(playerid, -1, "Jucator unmuted.");
    return 1;
}

// =============================================================
// COMMAND: /radio [freq]
// =============================================================
CMD:radio(playerid, params[])
{
    new Float:freq;
    if (sscanf(params, "f", freq))
        return SendClientMessage(playerid, -1, "Folosire: /radio [frecventa]");
    if (freq < 88.0 || freq > 199.0)
        return SendClientMessage(playerid, -1, "Frecventa: 88.0 - 199.0");
    Voice_SetRadioFreq(playerid, freq);
    new msg[48]; format(msg, sizeof(msg), "[Radio] Frecventa %.1f MHz.", freq);
    SendClientMessage(playerid, 0xFFFF00FF, msg);
    return 1;
}

// =============================================================
// Helper: set a player's faction (call this from your own /setfaction)
// =============================================================
SetPlayerFaction(playerid, faction)
{
    gPlayerFaction[playerid] = faction;
    switch (faction) {
        case FACTION_POLICE: {
            Voice_AddPlayer(playerid, g_ChanPolice);
            Voice_SetRadioFreq(playerid, RADIO_FREQ_POLICE);
        }
        case FACTION_MEDIC: {
            Voice_AddPlayer(playerid, g_ChanMedic);
            Voice_SetRadioFreq(playerid, RADIO_FREQ_MEDIC);
        }
    }
}
