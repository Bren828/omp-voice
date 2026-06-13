// ============================================================
//  VoiceChat core — Registry implementation
//  File: core/src/registry.cpp
// ============================================================
#include "../include/registry.h"

namespace vc::core {

Player& Registry::player(uint16_t id)
{
    auto& p = m_players[id];
    p.id = id;
    return p;
}

Player* Registry::find(uint16_t id)
{
    auto it = m_players.find(id);
    return it == m_players.end() ? nullptr : &it->second;
}

void Registry::removePlayer(uint16_t id)
{
    removeFromAllChannels(id);
    m_players.erase(id);
}

Channel& Registry::createChannel(uint16_t id, Filter f, bool positional, uint8_t prio)
{
    auto& c = m_channels[id];
    c.id = id; c.filter = f; c.positional = positional; c.priority = prio;
    return c;
}

Channel* Registry::channel(uint16_t id)
{
    auto it = m_channels.find(id);
    return it == m_channels.end() ? nullptr : &it->second;
}

void Registry::destroyChannel(uint16_t id)
{
    auto it = m_channels.find(id);
    if (it == m_channels.end()) return;
    for (uint16_t pid : it->second.members)
        if (Player* p = find(pid)) p->channels.erase(id);
    m_channels.erase(it);
}

void Registry::joinChannel(uint16_t pid, uint16_t cid)
{
    Channel* c = channel(cid);
    if (!c) return;
    c->members.insert(pid);
    player(pid).channels.insert(cid);
}

void Registry::leaveChannel(uint16_t pid, uint16_t cid)
{
    if (Channel* c = channel(cid)) c->members.erase(pid);
    if (Player*  p = find(pid))    p->channels.erase(cid);
}

void Registry::removeFromAllChannels(uint16_t pid)
{
    Player* p = find(pid);
    if (!p) return;
    for (uint16_t cid : p->channels)
        if (Channel* c = channel(cid)) c->members.erase(pid);
    p->channels.clear();
}

} // namespace vc::core
