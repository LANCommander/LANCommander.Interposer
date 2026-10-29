#include "server/spatial_grid.h"

#include <algorithm>

namespace comms::server {

void SpatialGrid::set_cell_size(float cell_size)
{
    cell_size = sanitize(cell_size);
    if (cell_size == cell_size_)
        return;
    cell_size_ = cell_size;
    cells_.clear();
    for (auto& [id, entry] : entries_) {
        entry.cell = key_for(entry.position);
        insert_into_cell(entry.cell, id);
    }
}

void SpatialGrid::update(uint32_t id, const interposer_comms_vec3& position)
{
    const uint64_t cell = key_for(position);
    auto [it, inserted] = entries_.try_emplace(id);
    if (!inserted && it->second.cell != cell)
        remove_from_cell(it->second.cell, id);
    if (inserted || it->second.cell != cell)
        insert_into_cell(cell, id);
    it->second.cell = cell;
    it->second.position = position;
}

void SpatialGrid::remove(uint32_t id)
{
    auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    remove_from_cell(it->second.cell, id);
    entries_.erase(it);
}

void SpatialGrid::remove_from_cell(uint64_t cell, uint32_t id)
{
    auto it = cells_.find(cell);
    if (it == cells_.end())
        return;
    auto& ids = it->second;
    auto pos = std::find(ids.begin(), ids.end(), id);
    if (pos != ids.end()) {
        *pos = ids.back();
        ids.pop_back();
    }
    if (ids.empty())
        cells_.erase(it);
}

} // namespace comms::server
