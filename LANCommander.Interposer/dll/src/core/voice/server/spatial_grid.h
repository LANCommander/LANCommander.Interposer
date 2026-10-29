#pragma once

#include "LANCommander.Interposer.Comms.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace comms::server {

// Uniform 3D hash grid for proximity queries. With the cell size set to the hearing range, a query only has
// to look at the 27 cells around the speaker, so routing cost scales with how many players are nearby rather
// than how many are on the server.
class SpatialGrid {
public:
    explicit SpatialGrid(float cell_size = 30.0f) : cell_size_(sanitize(cell_size)) {}

    float cell_size() const { return cell_size_; }

    // Changing the cell size re-buckets every entry.
    void set_cell_size(float cell_size);

    void update(uint32_t id, const interposer_comms_vec3& position);
    void remove(uint32_t id);
    bool contains(uint32_t id) const { return entries_.count(id) != 0; }
    size_t size() const { return entries_.size(); }

    // Calls fn(id) for every entry in cells overlapping the cube [center - radius, center + radius].
    // Results are candidates: callers still check the exact distance.
    template <typename Fn>
    void query(const interposer_comms_vec3& center, float radius, Fn&& fn) const
    {
        const int32_t reach = radius <= cell_size_ ? 1 : static_cast<int32_t>(std::ceil(radius / cell_size_));
        const int32_t cx = coordinate(center.x), cy = coordinate(center.y), cz = coordinate(center.z);
        for (int32_t x = cx - reach; x <= cx + reach; ++x) {
            for (int32_t y = cy - reach; y <= cy + reach; ++y) {
                for (int32_t z = cz - reach; z <= cz + reach; ++z) {
                    auto it = cells_.find(key(x, y, z));
                    if (it == cells_.end())
                        continue;
                    for (uint32_t id : it->second)
                        fn(id);
                }
            }
        }
    }

private:
    struct Entry {
        uint64_t   cell = 0;
        interposer_comms_vec3 position{};
    };

    static constexpr int32_t kCoordinateLimit = (1 << 20) - 1; // 21 bits per axis, signed

    static float sanitize(float cell_size) { return cell_size > 1e-3f && std::isfinite(cell_size) ? cell_size : 1.0f; }

    int32_t coordinate(float value) const
    {
        const float cell = std::floor(value / cell_size_);
        if (!(cell > -kCoordinateLimit))
            return -kCoordinateLimit;
        if (!(cell < kCoordinateLimit))
            return kCoordinateLimit;
        return static_cast<int32_t>(cell);
    }

    static uint64_t key(int32_t x, int32_t y, int32_t z)
    {
        auto pack = [](int32_t v) { return static_cast<uint64_t>(v + kCoordinateLimit + 1) & 0x1FFFFF; };
        return (pack(x) << 42) | (pack(y) << 21) | pack(z);
    }

    uint64_t key_for(const interposer_comms_vec3& p) const { return key(coordinate(p.x), coordinate(p.y), coordinate(p.z)); }

    void insert_into_cell(uint64_t cell, uint32_t id) { cells_[cell].push_back(id); }
    void remove_from_cell(uint64_t cell, uint32_t id);

    float cell_size_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
    std::unordered_map<uint32_t, Entry> entries_;
};

} // namespace comms::server
