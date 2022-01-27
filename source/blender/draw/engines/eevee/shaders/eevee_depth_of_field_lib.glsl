/**
 * Depth of Field utils.
 **/

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_shader_shared.hh)

/* -------------------------------------------------------------------- */
/** \name Constants.
 * \{ */

#ifndef DOF_SLIGHT_FOCUS_DENSITY
#  define DOF_SLIGHT_FOCUS_DENSITY 2
#endif

#ifdef DOF_RESOLVE_PASS
const bool is_resolve = true;
#else
const bool is_resolve = false;
#endif
#ifdef DOF_FOREGROUND_PASS
const bool is_foreground = DOF_FOREGROUND_PASS;
#else
const bool is_foreground = false;
#endif
/* Debug options */
const bool debug_gather_perf = false;
const bool debug_scatter_perf = false;
const bool debug_resolve_perf = false;

const bool no_smooth_intersection = false;
const bool no_gather_occlusion = false;
const bool no_gather_mipmaps = false;
const bool no_gather_random = false;
const bool no_gather_filtering = false;
const bool no_scatter_occlusion = false;
const bool no_scatter_pass = false;
const bool no_foreground_pass = false;
const bool no_background_pass = false;
const bool no_slight_focus_pass = false;
const bool no_focus_pass = false;
const bool no_holefill_pass = false;

/* Distribute weights between near/slightfocus/far fields (slide 117). */
const float dof_layer_threshold = 4.0;
/* Make sure it overlaps. */
const float dof_layer_offset_fg = 0.5 + 1.0;
/* Extra offset for convolution layers to avoid light leaking from background. */
const float dof_layer_offset = 0.5 + 0.5;

const int dof_max_slight_focus_radius = 5;

const vec2 quad_offsets[4] = vec2[4](
    vec2(-0.5, 0.5), vec2(0.5, 0.5), vec2(0.5, -0.5), vec2(-0.5, -0.5));

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weighting and downsampling utils.
 * \{ */

float dof_hdr_color_weight(vec4 color)
{
  /* Very fast "luma" weighting. */
  float luma = (color.g * 2.0) + (color.r + color.b);
  /* TODO(fclem) Pass correct exposure. */
  const float exposure = 1.0;
  return 1.0 / (luma * exposure + 4.0);
}

float dof_coc_select(vec4 cocs)
{
  /* Select biggest coc. */
  float selected_coc = cocs.x;
  if (abs(cocs.y) > abs(selected_coc)) {
    selected_coc = cocs.y;
  }
  if (abs(cocs.z) > abs(selected_coc)) {
    selected_coc = cocs.z;
  }
  if (abs(cocs.w) > abs(selected_coc)) {
    selected_coc = cocs.w;
  }
  return selected_coc;
}

/* NOTE: Do not forget to normalize weights afterwards. */
vec4 dof_bilateral_coc_weights(vec4 cocs)
{
  float chosen_coc = dof_coc_select(cocs);

  const float scale = 4.0; /* TODO(fclem) revisit. */
  /* NOTE: The difference between the cocs should be inside a abs() function,
   * but we follow UE4 implementation to improve how dithered transparency looks (see slide 19). */
  return saturate(1.0 - (chosen_coc - cocs) * scale);
}

/* NOTE: Do not forget to normalize weights afterwards. */
vec4 dof_bilateral_color_weights(vec4 colors[4])
{
  vec4 weights;
  for (int i = 0; i < 4; i++) {
    weights[i] = dof_hdr_color_weight(colors[i]);
  }
  return weights;
}

/* Returns signed Circle of confusion radius (in pixel) based on depth buffer value [0..1]. */
float dof_coc_from_depth(DepthOfFieldData dof, vec2 uv, float depth)
{
  if (is_panoramic(dof.camera_type)) {
    /* Use radial depth. */
    depth = -length(get_view_space_from_depth(uv, depth));
  }
  else {
    depth = get_view_z_from_depth(depth);
  }
  return coc_radius_from_camera_depth(dof, depth);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gather & Scatter Weighting
 * \{ */

float dof_layer_weight(float coc, const bool is_foreground)
{
  /* NOTE: These are fullres pixel CoC value. */
  if (is_resolve) {
    return saturate(-abs(coc) + dof_layer_threshold + dof_layer_offset) *
           float(is_foreground ? (coc <= 0.5) : (coc > -0.5));
  }
  else {
    coc *= 2.0; /* Account for half pixel gather. */
    float threshold = dof_layer_threshold -
                      ((is_foreground) ? dof_layer_offset_fg : dof_layer_offset);
    return saturate(((is_foreground) ? -coc : coc) - threshold);
  }
}
vec4 dof_layer_weight(vec4 coc)
{
  /* NOTE: Used for scatter pass which already flipped the sign correctly. */
  coc *= 2.0; /* Account for half pixel gather. */
  return saturate(coc - dof_layer_threshold + dof_layer_offset);
}

/* NOTE: This is halfres CoC radius. */
float dof_sample_weight(float coc)
{
  /* Full intensity if CoC radius is below the pixel footprint. */
  const float min_coc = 1.0;
  coc = max(min_coc, abs(coc));
  return (M_PI * min_coc * min_coc) / (M_PI * coc * coc);
}
vec4 dof_sample_weight(vec4 coc)
{
  /* Full intensity if CoC radius is below the pixel footprint. */
  const float min_coc = 1.0;
  coc = max(vec4(min_coc), abs(coc));
  return (M_PI * min_coc * min_coc) / (M_PI * coc * coc);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Circle of Confusion tiles
 * \{ */

struct CocTile {
  float fg_min_coc;
  float fg_max_coc;
  float fg_max_intersectable_coc;
  float fg_slight_focus_max_coc;
  float bg_min_coc;
  float bg_max_coc;
  float bg_min_intersectable_coc;
};

struct CocTilePrediction {
  bool do_foreground;
  bool do_slight_focus;
  bool do_focus;
  bool do_background;
  bool do_holefill;
};

/* WATCH: Might have to change depending on the texture format. */
const float dof_tile_defocus = 0.25;
const float dof_tile_focus = 0.0;
const float dof_tile_mixed = 0.75;
const float dof_tile_large_coc = 1024.0;

/* Init a CoC tile for reduction algorithms. */
CocTile dof_coc_tile_init(void)
{
  CocTile tile;
  tile.fg_min_coc = 0.0;
  tile.fg_max_coc = -dof_tile_large_coc;
  tile.fg_max_intersectable_coc = dof_tile_large_coc;
  tile.fg_slight_focus_max_coc = -1.0;
  tile.bg_min_coc = dof_tile_large_coc;
  tile.bg_max_coc = 0.0;
  tile.bg_min_intersectable_coc = dof_tile_large_coc;
  return tile;
}

CocTile dof_coc_tile_load(sampler2D fg_buffer, sampler2D bg_buffer, ivec2 tile_co)
{
  ivec2 tex_size = textureSize(fg_buffer, 0).xy;
  tile_co = clamp(tile_co, ivec2(0), tex_size - 1);

  vec4 fg = texelFetch(fg_buffer, tile_co, 0);
  vec3 bg = texelFetch(bg_buffer, tile_co, 0).xyz;

  CocTile tile;
  tile.fg_min_coc = -fg.x;
  tile.fg_max_coc = -fg.y;
  tile.fg_max_intersectable_coc = -fg.z;
  tile.fg_slight_focus_max_coc = fg.w;
  tile.bg_min_coc = bg.x;
  tile.bg_max_coc = bg.y;
  tile.bg_min_intersectable_coc = bg.z;
  return tile;
}

void dof_coc_tile_store(CocTile tile, out vec4 out_fg, out vec3 out_bg)
{
  out_fg.x = -tile.fg_min_coc;
  out_fg.y = -tile.fg_max_coc;
  out_fg.z = -tile.fg_max_intersectable_coc;
  out_fg.w = tile.fg_slight_focus_max_coc;
  out_bg.x = tile.bg_min_coc;
  out_bg.y = tile.bg_max_coc;
  out_bg.z = tile.bg_min_intersectable_coc;
}

bool dof_do_fast_gather(float max_absolute_coc, float min_absolute_coc, const bool is_foreground)
{
  float min_weight = dof_layer_weight((is_foreground) ? -min_absolute_coc : min_absolute_coc,
                                      is_foreground);
  if (min_weight < 1.0) {
    return false;
  }
  /* FIXME(fclem): This is a workaround to fast gather triggering too early. Since we use custom
   * opacity mask, the opacity is not given to be 100% even for after normal threshold. */
  if (is_foreground && min_absolute_coc < dof_layer_threshold) {
    return false;
  }
  return (max_absolute_coc - min_absolute_coc) < (DOF_FAST_GATHER_COC_ERROR * max_absolute_coc);
}

CocTilePrediction dof_coc_tile_prediction_get(CocTile tile)
{
  /* Based on tile value, predict what pass we need to load. */
  CocTilePrediction predict;

  predict.do_foreground = (-tile.fg_min_coc > dof_layer_threshold - dof_layer_offset_fg);
  bool fg_fully_opaque = predict.do_foreground &&
                         dof_do_fast_gather(-tile.fg_min_coc, -tile.fg_max_coc, true);

  predict.do_slight_focus = !fg_fully_opaque && (tile.fg_slight_focus_max_coc >= 0.5);
  predict.do_focus = !fg_fully_opaque && (tile.fg_slight_focus_max_coc == dof_tile_focus);

  predict.do_background = !predict.do_focus && !fg_fully_opaque &&
                          (tile.bg_max_coc > dof_layer_threshold - dof_layer_offset);
  bool bg_fully_opaque = predict.do_background &&
                         dof_do_fast_gather(-tile.bg_max_coc, tile.bg_min_coc, false);
  predict.do_holefill = !predict.do_focus && !fg_fully_opaque && -tile.fg_max_coc > 0.0;

#if 0 /* Debug */
  predict.do_foreground = predict.do_background = predict.do_holefill = true;
#endif
  return predict;
}

/* Special function to return the correct max value of 2 slight focus coc. */
float dof_coc_max_slight_focus(float coc1, float coc2)
{
  /* Do not consider values below 0.5 for expansion as they are "encoded".
   * See setup pass shader for more infos. */
  if ((coc1 == dof_tile_defocus && coc2 == dof_tile_focus) ||
      (coc1 == dof_tile_focus && coc2 == dof_tile_defocus)) {
    /* Tile where completely out of focus and in focus are both present.
     * Consider as very slightly out of focus. */
    return dof_tile_mixed;
  }
  return max(coc1, coc2);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gathering
 * \{ */

/**
 * Generate samples in a square pattern with the ring radius. X is the center tile.
 *
 *    Dist1          Dist2
 *                 6 5 4 3 2
 *    3 2 1        7       1
 *    . X 0        .   X   0
 *    . . .        .       .
 *                 . . . . .
 *
 * Samples are expected to be mirrored to complete the pattern.
 **/
ivec2 dof_square_ring_sample_offset(int ring_distance, int sample_id)
{
  ivec2 offset;
  if (sample_id < ring_distance) {
    offset.x = ring_distance;
    offset.y = sample_id;
  }
  else if (sample_id < ring_distance * 3) {
    offset.x = ring_distance - sample_id + ring_distance;
    offset.y = ring_distance;
  }
  else {
    offset.x = -ring_distance;
    offset.y = ring_distance - sample_id + 3 * ring_distance;
  }
  return offset;
}

/** \} */