// ============================================================
//  VoiceChat core — Spatial grid implementation
//  File: core/src/spatial_grid.cpp
// ============================================================
#include "../include/spatial_grid.h"
#include <algorithm>
#include <cmath>

namespace vc::core {

void SpatialGrid::eraseFromCell(const Key& k, uint16_t id)
{
    auto it = m_cells.find(k);
    if (it == m_cells.end()) return;
    auto& vec = it->second;
    auto e = std::find(vec.begin(), vec.end(), id);
    if (e != vec.end()) { *e = vec.back(); vec.pop_back(); }   // swap-erase, O(1)
    if (vec.empty()) m_cells.erase(it);                        // keep the map lean
}

void SpatialGrid::rebuild(const std::unordered_map<uint16_t, Player>& players)
{
    m_cells.clear();
    m_where.clear();
    for (auto& [id, p] : players) {
        Key k = keyFor(p);
        m_cells[k].push_back(id);
        m_where[id] = k;
    }
}

void SpatialGrid::update(const Player& p)
{
    Key k = keyFor(p);
    auto w = m_where.find(p.id);
    if (w != m_where.end()) {
        if (w->second == k) return;          // same cell — nothing to do
        eraseFromCell(w->second, p.id);       // moved cells: pull from the old
        w->second = k;
    } else {
        m_where.emplace(p.id, k);             // first time we've seen this id
    }
    m_cells[k].push_back(p.id);
}

void SpatialGrid::remove(uint16_t id)
{
    auto w = m_where.find(id);
    if (w == m_where.end()) return;
    eraseFromCell(w->second, id);
    m_where.erase(w);
}

void SpatialGrid::queryNeighbours(const Player& who, float radius,
                                  std::vector<uint16_t>& out) const
{
    // How many cells does `radius` span in each direction?
    int span = (int)std::ceil(radius / m_cell);
    int32_t cx = (int32_t)std::floor(who.pos.x / m_cell);
    int32_t cy = (int32_t)std::floor(who.pos.y / m_cell);

    for (int dx = -span; dx <= span; ++dx)
        for (int dy = -span; dy <= span; ++dy) {
            Key k{ who.interior, who.vworld, cx + dx, cy + dy };
            auto it = m_cells.find(k);
            if (it == m_cells.end()) continue;
            for (uint16_t id : it->second)
                if (id != who.id) out.push_back(id);
        }
}

} // namespace vc::core
