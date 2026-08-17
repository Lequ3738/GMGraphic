#pragma once
#include "../main.h"
#include "shader.h"

// gpart_* GPU particle system (DX9 only, D3D8 returns gerror).
// Semantics aligned with GM8's native particle API (part_type_*/part_system_*/
// part_emitter_* / part_particles_*). Implementation is a stateful GPU particle
// sim: state lives in A16B16G16R16F render-target textures (ping-pong), evolved
// by a 3-MRT fullscreen pass, rendered as point sprites via VTF (vs_3_0 texldl).
//
// Usage (GML):
//   gpart_system_create(capacity) -> configure gpart_type_* -> gpart_emitter_*
//   each step: gpart_system_update()          // global, advances ALL systems
//   draw event: gpart_system_drawit(sys)
//
// Constants below match GM8 (values identical to the engine's part_*/ps_* ones).

// ---- particle shapes (pt_shape_*) ----
enum gpart_shape : int
{
    PT_SHAPE_PIXEL = 0, PT_SHAPE_DISK = 1, PT_SHAPE_SQUARE = 2, PT_SHAPE_LINE = 3,
    PT_SHAPE_STAR = 4, PT_SHAPE_CIRCLE = 5, PT_SHAPE_RING = 6, PT_SHAPE_SPHERE = 7,
    PT_SHAPE_FLARE = 8, PT_SHAPE_SPARK = 9, PT_SHAPE_EXPLOSION = 10, PT_SHAPE_CLOUD = 11,
    PT_SHAPE_SMOKE = 12, PT_SHAPE_SNOW = 13,
    PT_SHAPE_COUNT = 14,
};

// ---- emitter region shape (ps_shape_*) ----
enum gpart_region_shape : int
{
    PS_SHAPE_RECTANGLE = 0, PS_SHAPE_ELLIPSE = 1, PS_SHAPE_DIAMOND = 2, PS_SHAPE_LINE = 3,
};

// ---- emitter distribution (ps_distr_*) ----
enum gpart_distribution : int
{
    PS_DISTR_LINEAR = 0, PS_DISTR_GAUSSIAN = 1,
};

// ---- color modes (internal; GML sets them via gpart_type_colour*) ----
enum gpart_colour_mode : int
{
    GP_COLOUR_NONE = 0,      // fallback: white constant
    GP_COLOUR_ONE = 1,       // c1 constant
    GP_COLOUR_TWO = 2,       // c1 -> c2 by age
    GP_COLOUR_THREE = 3,     // c1 -> c2 -> c3 by age
    GP_COLOUR_MIX = 4,       // random c1..c2, fixed for lifetime (per-particle)
    GP_COLOUR_RGB = 5,       // random in rgb range (per-particle)
    GP_COLOUR_HSV = 6,       // random in hsv range (per-particle)
};

enum gpart_alpha_mode : int
{
    GP_ALPHA_ONE = 1, GP_ALPHA_TWO = 2, GP_ALPHA_THREE = 3,
};

// ---- internal constants ----
constexpr int GP_GRID = 256;              // state texture grid size (256x256 = 65536 slots max)
constexpr int GP_MAX_CAPACITY = 65536;    // hard cap
constexpr int GP_MAX_BATCHES = 16;        // spawn batches per evolution pass run
constexpr int GP_TYPE_TEX_W = 256;        // type table texture width (max 256 types)
constexpr int GP_TYPE_TEX_H = 10;         // float4 texels per type (0..8 params, 9 = atlas rect)
constexpr int GP_MAX_FRAMES = 32;         // max sprite frames resolved per type
constexpr int GP_ATLAS_SIZE = 1024;       // particle atlas texture size (px)
constexpr int GP_ATLAS_TILE = 64;         // built-in shape tile size (px, 16 per row)
constexpr int GP_RECT_TEX_FRAMES = 32;    // rect table rows per type (max sprite frames)

// ---- exports: system (9) ----
exp_real gpart_system_create(double capacity);
exp_real gpart_system_destroy(double sys);
exp_real gpart_system_exists(double sys);
exp_real gpart_system_clear(double sys);
exp_real gpart_system_update();            // global: advance ALL systems one step (1/room_speed)
exp_real gpart_system_drawit(double sys);
exp_real gpart_system_draw_order(double sys, double oldtonew);
exp_real gpart_system_position(double sys, double x, double y);
exp_real gpart_system_capacity(double sys);

// ---- exports: particles (4) ----
exp_real gpart_particles_create(double sys, double x, double y, double parttype, double number);
exp_real gpart_particles_create_color(double sys, double x, double y, double parttype, double color, double number);
exp_real gpart_particles_clear(double sys);
exp_real gpart_particles_count(double sys);

// ---- exports: types (24) ----
exp_real gpart_type_create();
exp_real gpart_type_destroy(double type);
exp_real gpart_type_exists(double type);
exp_real gpart_type_clear(double type);
exp_real gpart_type_sprite(double type, double sprite, double animat, double stretch, double random);
exp_real gpart_type_shape(double type, double shape);
exp_real gpart_type_scale(double type, double xscale, double yscale);
exp_real gpart_type_life(double type, double min, double max);
exp_real gpart_type_size(double type, double min, double max);
exp_real gpart_type_speed(double type, double min, double max);
exp_real gpart_type_direction(double type, double min, double max);
exp_real gpart_type_gravity(double type, double dir, double force);
exp_real gpart_type_drag(double type, double coeff);
exp_real gpart_type_colour1(double type, double c1);
exp_real gpart_type_colour2(double type, double c1, double c2);
exp_real gpart_type_colour3(double type, double c1, double c2, double c3);
exp_real gpart_type_colour_mix(double type, double c1, double c2);
exp_real gpart_type_colour_rgb(double type, double rmin, double rmax, double gmin, double gmax, double bmin, double bmax);
exp_real gpart_type_colour_hsv(double type, double hmin, double hmax, double smin, double smax, double vmin, double vmax);
exp_real gpart_type_alpha1(double type, double a1);
exp_real gpart_type_alpha2(double type, double a1, double a2);
exp_real gpart_type_alpha3(double type, double a1, double a2, double a3);
exp_real gpart_type_blend(double type, double additive);
exp_real gpart_type_orientation(double type, double ang_min, double ang_max, double ang_incr, double ang_wiggle, double ang_relative);

// ---- exports: emitters (9) ----
exp_real gpart_emitter_create(double sys);
exp_real gpart_emitter_destroy(double sys, double em);
exp_real gpart_emitter_destroy_all(double sys);
exp_real gpart_emitter_exists(double sys, double em);
exp_real gpart_emitter_clear(double sys, double em);
exp_real gpart_emitter_region(double sys, double em, double xmin, double xmax, double ymin, double ymax, double shape, double distribution);
exp_real gpart_emitter_burst(double sys, double em, double parttype, double number);
exp_real gpart_emitter_stream(double sys, double em, double parttype, double number);