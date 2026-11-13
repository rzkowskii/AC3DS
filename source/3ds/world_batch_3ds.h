#pragma once

#include <array>

struct Ac3dsWorldBatchKey
{
    std::array<float, 7> view;
    std::array<float, 24> clip;
    std::array<int, 11> options;
    unsigned world, rays, lighting, textures;

    bool operator==(const Ac3dsWorldBatchKey &other) const
    {
        return world == other.world && rays == other.rays && lighting == other.lighting &&
               textures == other.textures && view == other.view &&
               options == other.options && clip == other.clip;
    }
};

bool ac3ds_restore_world_batch(int pass, const Ac3dsWorldBatchKey &key);
void ac3ds_report_world_batches();
void ac3ds_save_world_batch(const Ac3dsWorldBatchKey &key);
bool ac3ds_can_record_lit_world_batch();
void ac3ds_begin_world_dependencies(bool record);
void ac3ds_world_dependency_mip(int mip);
void ac3ds_record_world_defer(const sqr *cube);
bool ac3ds_hold_refraction_world();
bool ac3ds_reuse_refraction_world();
void ac3ds_release_refraction_world();
bool ac3ds_same_refraction_world_view();
unsigned ac3ds_shared_reflection_replays();
void ac3ds_begin_world_strips(const float *clip);
unsigned ac3ds_static_world_lighting_generation();
bool ac3ds_dynamic_world_lighting();
const vector<block> &ac3ds_dynamic_world_regions();
unsigned ac3ds_world_texture_generation();
