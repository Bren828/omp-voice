// ============================================================
//  VoiceChat core — Spatial hash grid (req. F1)
//  File: core/include/spatial_grid.h
//
//  Buckets players into fixed-size cells so proximity routing only
//  inspects neighbouring cells instead of O(n^2) over all players.
//  Keyed by (interior, vworld, cellX, cellY) so cross-dimension players
//  never even share a bucket (req. J2 enforced for free).
// ============================================================
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cmath>
#include "registry.h"

namespace vc::core {

class SpatialGrid {
public:
    explicit SpatialGrid(float cellSize = 50.f) : m_cell(cellSize) {}

    // Full rebuild from scratch (used by tests + as a periodic self-heal).
    void rebuild(const std::unordered_map<uint16_t, Player>& players);

    // INCREMENTAL maintenance (stage F): O(1) amortised per moving player,
    // instead of an O(n) rebuild every tick. update() inserts the player or
    // moves it between cells when its cell key (pos/interior/vworld) changes;
    // remove() pulls it on disconnect. Keep these in sync with the registry.
    void update(const Player& p);
    void remove(uint16_t id);
    void clear() { m_cells.clear(); m_where.clear(); }

    // Append ids of players within `radius` of `who` that share interior+vw.
    // Caller still does the exact distance/falloff test; this just prunes.
    void queryNeighbours(const Player& who, float radius,
                         std::vector<uint16_t>& out) const;

private:
    struct Key {
        uint16_t interior, vworld; int32_t cx, cy;
        bool operator==(const Key& o) const {
            return interior==o.interior && vworld==o.vworld && cx==o.cx && cy==o.cy;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            size_t h = k.interior; h = h*131 + k.vworld;
            h = h*131 + (uint32_t)k.cx; h = h*131 + (uint32_t)k.cy; return h;
        }
    };

    Key keyFor(const Player& p) const {
        return Key{ p.interior, p.vworld,
                    (int32_t)std::floor(p.pos.x / m_cell),
                    (int32_t)std::floor(p.pos.y / m_cell) };
    }
    void eraseFromCell(const Key& k, uint16_t id);

    float m_cell;
    std::unordered_map<Key, std::vector<uint16_t>, KeyHash> m_cells;
    std::unordered_map<uint16_t, Key> m_where;   // id -> its current cell key
};

} // namespace vc::core
