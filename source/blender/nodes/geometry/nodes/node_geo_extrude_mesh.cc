/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "BLI_disjoint_set.hh"
#include "BLI_task.hh"
#include "BLI_vector_set.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_extrude_mesh_cc {

NODE_STORAGE_FUNCS(NodeGeometryExtrudeMesh)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Mesh").supported_type(GEO_COMPONENT_TYPE_MESH);
  b.add_input<decl::Bool>(N_("Selection")).default_value(true).supports_field().hide_value();
  b.add_input<decl::Vector>(N_("Offset")).subtype(PROP_TRANSLATION).implicit_field().hide_value();
  b.add_input<decl::Float>(N_("Strength")).default_value(1.0f).supports_field();
  b.add_input<decl::Bool>(N_("Individual")).default_value(true);
  b.add_output<decl::Geometry>("Mesh");
  b.add_output<decl::Bool>(N_("Top")).field_source();
  b.add_output<decl::Bool>(N_("Side")).field_source();
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "mode", 0, "", ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeGeometryExtrudeMesh *data = MEM_cnew<NodeGeometryExtrudeMesh>(__func__);
  data->mode = GEO_NODE_EXTRUDE_MESH_FACES;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  const NodeGeometryExtrudeMesh &storage = node_storage(*node);
  const GeometryNodeExtrudeMeshMode mode = static_cast<GeometryNodeExtrudeMeshMode>(storage.mode);

  bNodeSocket *individual_socket = (bNodeSocket *)node->inputs.last;

  nodeSetSocketAvailability(ntree, individual_socket, mode == GEO_NODE_EXTRUDE_MESH_FACES);
}

static MutableSpan<MVert> mesh_verts(Mesh &mesh)
{
  return {mesh.mvert, mesh.totvert};
}
static Span<MEdge> mesh_edges(const Mesh &mesh)
{
  return {mesh.medge, mesh.totedge};
}
static MutableSpan<MEdge> mesh_edges(Mesh &mesh)
{
  return {mesh.medge, mesh.totedge};
}
static Span<MPoly> mesh_polys(const Mesh &mesh)
{
  return {mesh.mpoly, mesh.totpoly};
}
static MutableSpan<MPoly> mesh_polys(Mesh &mesh)
{
  return {mesh.mpoly, mesh.totpoly};
}
static Span<MLoop> mesh_loops(const Mesh &mesh)
{
  return {mesh.mloop, mesh.totloop};
}
static MutableSpan<MLoop> mesh_loops(Mesh &mesh)
{
  return {mesh.mloop, mesh.totloop};
}

struct AttributeOutputs {
  StrongAnonymousAttributeID top_id;
  StrongAnonymousAttributeID side_id;
};

static void save_selection_as_attribute(MeshComponent &component,
                                        const AnonymousAttributeID *id,
                                        const AttributeDomain domain,
                                        const IndexMask selection)
{
  BLI_assert(!component.attribute_exists(id));

  OutputAttribute_Typed<bool> attribute = component.attribute_try_get_for_output_only<bool>(
      id, domain);
  if (selection.is_range()) {
    attribute.as_span().slice(selection.as_range()).fill(true);
  }
  else {
    attribute.as_span().fill_indices(selection, true);
  }

  attribute.save();
}

static void expand_mesh_size(Mesh &mesh,
                             const int vert_expand,
                             const int edge_expand,
                             const int poly_expand,
                             const int loop_expand)
{
  if (vert_expand != 0) {
    CustomData_duplicate_referenced_layers(&mesh.vdata, mesh.totvert);
    mesh.totvert += vert_expand;
    CustomData_realloc(&mesh.vdata, mesh.totvert);
  }
  else {
    /* Even when the number of vertices is not changed, the mesh can still be deformed. */
    CustomData_duplicate_referenced_layer(&mesh.vdata, CD_MVERT, mesh.totvert);
  }
  if (edge_expand != 0) {
    CustomData_duplicate_referenced_layers(&mesh.edata, mesh.totedge);
    mesh.totedge += edge_expand;
    CustomData_realloc(&mesh.edata, mesh.totedge);
  }
  if (poly_expand != 0) {
    CustomData_duplicate_referenced_layers(&mesh.pdata, mesh.totpoly);
    mesh.totpoly += poly_expand;
    CustomData_realloc(&mesh.pdata, mesh.totpoly);
  }
  if (loop_expand != 0) {
    CustomData_duplicate_referenced_layers(&mesh.ldata, mesh.totloop);
    mesh.totloop += loop_expand;
    CustomData_realloc(&mesh.ldata, mesh.totloop);
  }
  BKE_mesh_update_customdata_pointers(&mesh, false);

  for (MVert &vert : mesh_verts(mesh).take_back(vert_expand)) {
    vert.flag = 0;
  }
}

template<typename T> void copy_with_indices(MutableSpan<T> dst, Span<T> src, Span<int> indices)
{
  BLI_assert(dst.size() == indices.size());
  for (const int i : dst.index_range()) {
    dst[i] = src[indices[i]];
  }
}

template<typename T> void copy_with_mask(MutableSpan<T> dst, Span<T> src, IndexMask mask)
{
  BLI_assert(dst.size() == mask.size());
  threading::parallel_for(mask.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      dst[i] = src[mask[i]];
    }
  });
}

/**
 * \param get_mix_indices_fn: Returns a Span of indices of the source points to mix for every
 * result point.
 */
template<typename T, typename GetMixIndicesFn>
void copy_with_mixing(MutableSpan<T> dst, Span<T> src, GetMixIndicesFn get_mix_indices_fn)
{
  threading::parallel_for(dst.index_range(), 512, [&](const IndexRange range) {
    attribute_math::DefaultMixer<T> mixer{dst.slice(range)};
    for (const int i_dst : IndexRange(range.size())) {
      for (const int i_src : get_mix_indices_fn(range[i_dst])) {
        mixer.mix_in(i_dst, src[i_src]);
      }
    }
    mixer.finalize();
  });
}

static Array<Vector<int>> create_vert_to_edge_map(const int vert_size,
                                                  Span<MEdge> edges,
                                                  const int vert_offset = 0)
{
  Array<Vector<int>> vert_to_edge_map(vert_size);
  for (const int i : edges.index_range()) {
    vert_to_edge_map[edges[i].v1 - vert_offset].append(i);
    vert_to_edge_map[edges[i].v2 - vert_offset].append(i);
  }
  return vert_to_edge_map;
}

static void extrude_mesh_vertices(MeshComponent &component,
                                  const Field<bool> &selection_field,
                                  const Field<float3> &offset_field,
                                  const AttributeOutputs &attribute_outputs)
{
  Mesh &mesh = *component.get_for_write();
  const int orig_vert_size = mesh.totvert;
  const int orig_edge_size = mesh.totedge;

  GeometryComponentFieldContext context{component, ATTR_DOMAIN_POINT};
  FieldEvaluator evaluator{context, mesh.totvert};
  evaluator.add(offset_field);
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const IndexMask selection = evaluator.get_evaluated_selection_as_mask();
  const VArray<float3> offsets = evaluator.get_evaluated<float3>(0);

  /* This allows parallelizing attribute mixing for new edges. */
  Array<Vector<int>> vert_to_edge_map = create_vert_to_edge_map(orig_vert_size, mesh_edges(mesh));

  expand_mesh_size(mesh, selection.size(), selection.size(), 0, 0);

  const IndexRange new_vert_range{orig_vert_size, selection.size()};
  const IndexRange new_edge_range{orig_edge_size, selection.size()};

  MutableSpan<MVert> new_verts = mesh_verts(mesh).slice(new_vert_range);
  MutableSpan<MEdge> new_edges = mesh_edges(mesh).slice(new_edge_range);

  for (const int i_selection : selection.index_range()) {
    MEdge &edge = new_edges[i_selection];
    edge.v1 = selection[i_selection];
    edge.v2 = orig_vert_size + i_selection;
    edge.flag = ME_LOOSEEDGE;
  }

  component.attribute_foreach([&](const AttributeIDRef &id, const AttributeMetaData meta_data) {
    if (!ELEM(meta_data.domain, ATTR_DOMAIN_POINT, ATTR_DOMAIN_EDGE)) {
      return true;
    }
    OutputAttribute attribute = component.attribute_try_get_for_output(
        id, meta_data.domain, meta_data.data_type);
    attribute_math::convert_to_static_type(meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);
      MutableSpan<T> data = attribute.as_span().typed<T>();
      switch (attribute.domain()) {
        case ATTR_DOMAIN_POINT: {
          /* New vertices copy the attribute values from their source vertex. */
          copy_with_mask(data.slice(new_vert_range), data.as_span(), selection);
          break;
        }
        case ATTR_DOMAIN_EDGE: {
          /* New edge values are mixed from of all the edges connected to the source vertex. */
          copy_with_mixing(
              data.slice(new_edge_range), data.as_span(), [&](const int i) -> Span<int> {
                return vert_to_edge_map[selection[i]];
              });
          break;
        }
        default:
          BLI_assert_unreachable();
      }
    });

    attribute.save();
    return true;
  });

  devirtualize_varray(offsets, [&](const auto offsets) {
    threading::parallel_for(selection.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        const float3 offset = offsets[selection[i]];
        add_v3_v3(new_verts[i].co, offset);
      }
    });
  });

  if (attribute_outputs.top_id) {
    save_selection_as_attribute(
        component, attribute_outputs.top_id.get(), ATTR_DOMAIN_POINT, new_vert_range);
  }
  if (attribute_outputs.side_id) {
    save_selection_as_attribute(
        component, attribute_outputs.side_id.get(), ATTR_DOMAIN_EDGE, new_edge_range);
  }

  BKE_mesh_runtime_clear_cache(&mesh);
  BKE_mesh_normals_tag_dirty(&mesh);

  BKE_mesh_calc_normals(component.get_for_write());
  BLI_assert(BKE_mesh_is_valid(component.get_for_write()));
}

static Array<Vector<int, 2>> mesh_calculate_polys_of_edge(const Mesh &mesh,
                                                          const IndexMask poly_mask)
{
  Span<MPoly> polys = mesh_polys(mesh);
  Span<MLoop> loops = mesh_loops(mesh);
  Array<Vector<int, 2>> polys_of_edge(mesh.totedge);

  poly_mask.foreach_index([&](const int64_t i_poly) {
    const MPoly &poly = polys[i_poly];
    for (const MLoop &loop : loops.slice(poly.loopstart, poly.totloop)) {
      polys_of_edge[loop.e].append(i_poly);
    }
  });

  return polys_of_edge;
}

static Array<Vector<int, 2>> mesh_calculate_polys_of_edge(const Mesh &mesh)
{
  return mesh_calculate_polys_of_edge(mesh, IndexMask(mesh.totpoly));
}

static void fill_quad_consistent_direction(Span<MLoop> other_poly_loops,
                                           MutableSpan<MLoop> new_loops,
                                           const int vert_connected_to_poly_1,
                                           const int vert_connected_to_poly_2,
                                           const int vert_across_from_poly_1,
                                           const int vert_across_from_poly_2,
                                           const int edge_connected_to_poly,
                                           const int connecting_edge_1,
                                           const int edge_across_from_poly,
                                           const int connecting_edge_2)
{
  /* Find the loop on the polygon connected to the new quad that uses the duplicate edge. */
  bool start_with_connecting_edge = true;
  for (const MLoop &loop : other_poly_loops) {
    if (loop.e == edge_connected_to_poly) {
      start_with_connecting_edge = loop.v == vert_connected_to_poly_1;
      break;
    }
  }
  if (start_with_connecting_edge) {
    new_loops[0].v = vert_connected_to_poly_1;
    new_loops[0].e = connecting_edge_1;
    new_loops[1].v = vert_across_from_poly_1;
    new_loops[1].e = edge_across_from_poly;
    new_loops[2].v = vert_across_from_poly_2;
    new_loops[2].e = connecting_edge_2;
    new_loops[3].v = vert_connected_to_poly_2;
    new_loops[3].e = edge_connected_to_poly;
  }
  else {
    new_loops[0].v = vert_connected_to_poly_1;
    new_loops[0].e = edge_connected_to_poly;
    new_loops[1].v = vert_connected_to_poly_2;
    new_loops[1].e = connecting_edge_2;
    new_loops[2].v = vert_across_from_poly_2;
    new_loops[2].e = edge_across_from_poly;
    new_loops[3].v = vert_across_from_poly_1;
    new_loops[3].e = connecting_edge_1;
  }
}

/**
 * The resulting vector maps from the index in the added vertices to the original vertex they were
 * extruded from.
 */
static Vector<int> extrude_vert_orig_indices_from_edges(const IndexMask edge_selection,
                                                        const Mesh &mesh,
                                                        MutableSpan<int> new_vert_indices)
{
  Vector<int> new_vert_orig_indices;
  new_vert_orig_indices.reserve(edge_selection.size());
  for (const int i_edge : edge_selection) {
    const MEdge &edge = mesh_edges(mesh)[i_edge];

    if (new_vert_indices[edge.v1] == -1) {
      new_vert_indices[edge.v1] = mesh.totvert + new_vert_orig_indices.size();
      new_vert_orig_indices.append(edge.v1);
    }

    if (new_vert_indices[edge.v2] == -1) {
      new_vert_indices[edge.v2] = mesh.totvert + new_vert_orig_indices.size();
      new_vert_orig_indices.append(edge.v2);
    }
  }
  return new_vert_orig_indices;
}

static void extrude_mesh_edges(MeshComponent &component,
                               const Field<bool> &selection_field,
                               const Field<float3> &offset_field,
                               const AttributeOutputs &attribute_outputs)
{
  Mesh &mesh = *component.get_for_write();
  const int orig_vert_size = mesh.totvert;
  Span<MEdge> orig_edges = mesh_edges(mesh);
  Span<MPoly> orig_polys = mesh_polys(mesh);
  const int orig_loop_size = mesh.totloop;

  GeometryComponentFieldContext edge_context{component, ATTR_DOMAIN_EDGE};
  FieldEvaluator edge_evaluator{edge_context, mesh.totedge};
  edge_evaluator.set_selection(selection_field);
  edge_evaluator.add(offset_field);
  edge_evaluator.evaluate();
  const IndexMask edge_selection = edge_evaluator.get_evaluated_selection_as_mask();
  const VArray<float3> &edge_offsets = edge_evaluator.get_evaluated<float3>(0);
  if (edge_selection.is_empty()) {
    return;
  }

  Array<int> new_vert_indices(orig_vert_size, -1);
  Vector<int> new_vert_orig_indices = extrude_vert_orig_indices_from_edges(
      edge_selection, mesh, new_vert_indices);

  const IndexRange new_vert_range{orig_vert_size, new_vert_orig_indices.size()};
  /* The extruded edges connect the original and duplicate edges. */
  const IndexRange connect_edge_range{orig_edges.size(), new_vert_range.size()};
  /* The duplicate edges are extruded copies of the selected edges. */
  const IndexRange duplicate_edge_range{connect_edge_range.one_after_last(),
                                        edge_selection.size()};
  /* There is a new polygon for every selected edge. */
  const IndexRange new_poly_range{orig_polys.size(), edge_selection.size()};
  /* Every new polygon is a quad with four corners. */
  const IndexRange new_loop_range{orig_loop_size, new_poly_range.size() * 4};

  Array<Vector<int, 2>> edge_to_poly_map = mesh_calculate_polys_of_edge(mesh);

  expand_mesh_size(mesh,
                   new_vert_range.size(),
                   connect_edge_range.size() + duplicate_edge_range.size(),
                   new_poly_range.size(),
                   new_loop_range.size());

  MutableSpan<MVert> new_verts = mesh_verts(mesh).slice(new_vert_range);
  MutableSpan<MEdge> connect_edges = mesh_edges(mesh).slice(connect_edge_range);
  MutableSpan<MEdge> duplicate_edges = mesh_edges(mesh).slice(duplicate_edge_range);
  MutableSpan<MPoly> polys = mesh_polys(mesh);
  MutableSpan<MPoly> new_polys = polys.slice(new_poly_range);
  MutableSpan<MLoop> loops = mesh_loops(mesh);
  MutableSpan<MLoop> new_loops = loops.slice(new_loop_range);

  for (const int i : connect_edges.index_range()) {
    MEdge &edge = connect_edges[i];
    edge.v1 = new_vert_orig_indices[i];
    edge.v2 = orig_vert_size + i;
    edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);
  }

  for (const int i : duplicate_edges.index_range()) {
    const MEdge &orig_edge = mesh.medge[edge_selection[i]];
    MEdge &edge = duplicate_edges[i];
    edge.v1 = new_vert_indices[orig_edge.v1];
    edge.v2 = new_vert_indices[orig_edge.v2];
    edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);
  }

  for (const int i : new_polys.index_range()) {
    MPoly &poly = new_polys[i];
    poly.loopstart = orig_loop_size + i * 4;
    poly.totloop = 4;
    poly.flag = 0;
  }

  for (const int i : edge_selection.index_range()) {
    const int orig_edge_index = edge_selection[i];

    const MEdge &duplicate_edge = duplicate_edges[i];
    const int new_vert_1 = duplicate_edge.v1;
    const int new_vert_2 = duplicate_edge.v2;
    const int extrude_index_1 = new_vert_1 - orig_vert_size;
    const int extrude_index_2 = new_vert_2 - orig_vert_size;

    Span<int> connected_polys = edge_to_poly_map[orig_edge_index];

    /* When there was a single polygon connected to the new polygon, we can use the old one to keep
     * the face direction consistent. When there is more than one connected edge, the new face
     * direction is totally arbitrary and the only goal for the behavior is to be deterministic. */
    Span<MLoop> connected_poly_loops = {};
    if (connected_polys.size() == 1) {
      const MPoly &connected_poly = polys[connected_polys.first()];
      connected_poly_loops = loops.slice(connected_poly.loopstart, connected_poly.totloop);
    }
    fill_quad_consistent_direction(connected_poly_loops,
                                   new_loops.slice(4 * i, 4),
                                   new_vert_orig_indices[extrude_index_1],
                                   new_vert_orig_indices[extrude_index_2],
                                   new_vert_1,
                                   new_vert_2,
                                   orig_edge_index,
                                   connect_edge_range[extrude_index_1],
                                   duplicate_edge_range[i],
                                   connect_edge_range[extrude_index_2]);
  }

  /* Create a map of indices in the extruded vertices array to all of the indices of edges
   * in the duplicate edges array that connect to that vertex. This can be used to simplify the
   * mixing of attribute data for the connecting edges. */
  Array<Vector<int>> new_vert_to_duplicate_edge_map = create_vert_to_edge_map(
      new_vert_range.size(), duplicate_edges, orig_vert_size);

  component.attribute_foreach([&](const AttributeIDRef &id, const AttributeMetaData meta_data) {
    OutputAttribute attribute = component.attribute_try_get_for_output(
        id, meta_data.domain, meta_data.data_type);
    if (!attribute) {
      return true; /* Impossible to write the "normal" attribute. */
    }

    attribute_math::convert_to_static_type(meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);
      MutableSpan<T> data = attribute.as_span().typed<T>();
      switch (attribute.domain()) {
        case ATTR_DOMAIN_POINT: {
          /* New vertices copy the attribute values from their source vertex. */
          copy_with_indices(data.slice(new_vert_range), data.as_span(), new_vert_orig_indices);
          break;
        }
        case ATTR_DOMAIN_EDGE: {
          /* Edges parallel to original edges copy the edge attributes from the original edges. */
          MutableSpan<T> duplicate_data = data.slice(duplicate_edge_range);
          copy_with_mask(duplicate_data, data.as_span(), edge_selection);

          /* Edges connected to original vertices mix values of selected connected edges. */
          MutableSpan<T> connect_data = data.slice(connect_edge_range);
          copy_with_mixing(connect_data, duplicate_data.as_span(), [&](const int i_new_vert) {
            return new_vert_to_duplicate_edge_map[i_new_vert].as_span();
          });
          break;
        }
        case ATTR_DOMAIN_FACE: {
          /* Attribute values for new faces are a mix of the values of faces connected to the its
           * original edge.  */
          copy_with_mixing(data.slice(new_poly_range), data.as_span(), [&](const int i) {
            return edge_to_poly_map[edge_selection[i]].as_span();
          });

          break;
        }
        case ATTR_DOMAIN_CORNER: {
          /* New corners get the average value of all adjacent corners on original faces connected
           * to the original edge. */
          MutableSpan<T> new_data = data.slice(new_loop_range);
          threading::parallel_for(edge_selection.index_range(), 256, [&](const IndexRange range) {
            for (const int i_edge_selection : range) {
              const int orig_edge_index = edge_selection[i_edge_selection];

              Span<int> connected_polys = edge_to_poly_map[orig_edge_index];
              if (connected_polys.is_empty()) {
                /* If there are no connected polygons, there is no corner data to
                 * interpolate. */
                new_data.slice(4 * i_edge_selection, 4).fill(T());
                continue;
              }

              /* Both corners on each vertical edge of the side polygon get the same value,
               * so there are only two unique values to mix. */
              Array<T> side_poly_corner_data(2);
              attribute_math::DefaultMixer<T> mixer{side_poly_corner_data};

              const MEdge &duplicate_edge = duplicate_edges[i_edge_selection];
              const int new_vert_1 = duplicate_edge.v1;
              const int new_vert_2 = duplicate_edge.v2;
              const int orig_vert_1 = new_vert_orig_indices[new_vert_1 - orig_vert_size];
              const int orig_vert_2 = new_vert_orig_indices[new_vert_2 - orig_vert_size];

              /* Average the corner data from the corners that share a vertex from the
               * polygons that share an edge with the extruded edge. */
              for (const int i_connected_poly : connected_polys.index_range()) {
                const MPoly &connected_poly = polys[connected_polys[i_connected_poly]];
                for (const int i_loop :
                     IndexRange(connected_poly.loopstart, connected_poly.totloop)) {
                  const MLoop &loop = loops[i_loop];
                  if (loop.v == orig_vert_1) {
                    mixer.mix_in(0, data[i_loop]);
                  }
                  if (loop.v == orig_vert_2) {
                    mixer.mix_in(1, data[i_loop]);
                  }
                }
              }

              mixer.finalize();

              /* Instead of replicating the order in #fill_quad_consistent_direction here, it's
               * simpler (though probably slower) to just match the corner data based on the vertex
               * indices. */
              for (const int i : IndexRange(4 * i_edge_selection, 4)) {
                if (ELEM(new_loops[i].v, new_vert_1, orig_vert_1)) {
                  new_data[i] = side_poly_corner_data.first();
                }
                else if (ELEM(new_loops[i].v, new_vert_2, orig_vert_2)) {
                  new_data[i] = side_poly_corner_data.last();
                }
              }
            }
          });
          break;
        }
        default:
          BLI_assert_unreachable();
      }
    });

    attribute.save();
    return true;
  });

  if (edge_offsets.is_single()) {
    const float3 offset = edge_offsets.get_internal_single();
    threading::parallel_for(new_verts.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        add_v3_v3(new_verts[i].co, offset);
      }
    });
  }
  else {
    Array<float3> vert_offsets(new_verts.size());
    attribute_math::DefaultMixer<float3> mixer(vert_offsets);
    for (const int i : edge_selection.index_range()) {
      const MEdge &edge = duplicate_edges[i];
      const float3 offset = edge_offsets[edge_selection[i]];
      mixer.mix_in(edge.v1 - orig_vert_size, offset);
      mixer.mix_in(edge.v2 - orig_vert_size, offset);
    }
    mixer.finalize();

    threading::parallel_for(new_verts.index_range(), 1024, [&](const IndexRange range) {
      for (const int i : range) {
        add_v3_v3(new_verts[i].co, vert_offsets[i]);
      }
    });
  }

  if (attribute_outputs.top_id) {
    save_selection_as_attribute(
        component, attribute_outputs.top_id.get(), ATTR_DOMAIN_EDGE, duplicate_edge_range);
  }
  if (attribute_outputs.side_id) {
    save_selection_as_attribute(
        component, attribute_outputs.side_id.get(), ATTR_DOMAIN_FACE, new_poly_range);
  }

  BKE_mesh_runtime_clear_cache(&mesh);
  BKE_mesh_normals_tag_dirty(&mesh);

  BLI_assert(BKE_mesh_is_valid(component.get_for_write()));
}

/**
 * Edges connected to one selected face are on the boundary of a region and will be duplicated into
 * a "side face".
 */
static void extrude_mesh_face_regions(MeshComponent &component,
                                      const Field<bool> &selection_field,
                                      const Field<float3> &offset_field,
                                      const AttributeOutputs &attribute_outputs)
{
  Mesh &mesh = *component.get_for_write();
  const int orig_vert_size = mesh.totvert;
  Span<MEdge> orig_edges = mesh_edges(mesh);
  Span<MPoly> orig_polys = mesh_polys(mesh);
  Span<MLoop> orig_loops = mesh_loops(mesh);

  GeometryComponentFieldContext poly_context{component, ATTR_DOMAIN_FACE};
  FieldEvaluator poly_evaluator{poly_context, mesh.totpoly};
  poly_evaluator.set_selection(selection_field);
  poly_evaluator.add(offset_field);
  poly_evaluator.evaluate();
  const IndexMask poly_selection = poly_evaluator.get_evaluated_selection_as_mask();
  const VArray<float3> &poly_offsets = poly_evaluator.get_evaluated<float3>(0);
  if (poly_selection.is_empty()) {
    return;
  }

  /* Mix the offsets from the face domain to the vertex domain. Evaluate on the face domain above
   * in order to be consistent with the selection, and to use the face normals rather than vertex
   * normals as an offset, for example. */
  Array<float3> vert_offsets;
  if (!poly_offsets.is_single()) {
    vert_offsets.reinitialize(orig_vert_size);
    attribute_math::DefaultMixer<float3> mixer(vert_offsets);
    for (const int i_poly : poly_selection) {
      const MPoly &poly = orig_polys[i_poly];
      const float3 offset = poly_offsets[i_poly];
      for (const MLoop &loop : orig_loops.slice(poly.loopstart, poly.totloop)) {
        mixer.mix_in(loop.v, offset);
      }
    }
    mixer.finalize();
  }

  /* Build a map that contains all of the selected faces connected to each edge. */
  Array<Vector<int, 2>> edge_to_selected_poly_map = mesh_calculate_polys_of_edge(mesh,
                                                                                 poly_selection);

  /* All of the faces (selected and unselected) connected to each edge. */
  // Array<Vector<int, 2>> edge_to_poly_map = mesh_calculate_polys_of_edge(mesh);

  /* All vertices that are connected to the selected polygons.
   * Start the size at one vert per poly to reduce reallocations. */
  VectorSet<int> all_selected_verts;
  all_selected_verts.reserve(orig_polys.size());
  for (const int i_poly : poly_selection) {
    const MPoly &poly = orig_polys[i_poly];
    for (const MLoop &loop : orig_loops.slice(poly.loopstart, poly.totloop)) {
      all_selected_verts.add(loop.v);
    }
  }

  /* These edges are on top of an extruded region. Their vertices should be moved, but the edges
   * themselves should not be duplicated. */
  Vector<int> in_between_edges;
  /* The extruded face corresponding to each extruded edge (and each extruded face). */
  Vector<int> edge_orig_face_indices;
  Vector<int64_t> selected_edge_orig_indices;
  for (const int i_edge : orig_edges.index_range()) {
    Span<int> selected_polys = edge_to_selected_poly_map[i_edge];
    if (selected_polys.size() == 1) {
      selected_edge_orig_indices.append(i_edge);
      edge_orig_face_indices.append(selected_polys.first());
    }
    else if (selected_polys.size() > 1) {
      in_between_edges.append(i_edge);
    }
  }
  const IndexMask edge_selection{selected_edge_orig_indices};

  /* Indices into the `duplicate_edges` span for each original selected edge. */
  Array<int> duplicate_edge_indices(orig_edges.size(), -1);
  for (const int i : edge_selection.index_range()) {
    duplicate_edge_indices[edge_selection[i]] = i;
  }

  Array<int> new_vert_indices(orig_vert_size, -1);
  Vector<int> new_vert_orig_indices = extrude_vert_orig_indices_from_edges(
      edge_selection, mesh, new_vert_indices);

  const IndexRange new_vert_range{orig_vert_size, new_vert_orig_indices.size()};
  /* One edge connects each selected vertex to a new vertex on the extruded polygons. */
  const IndexRange connect_edge_range{orig_edges.size(), new_vert_range.size()};
  /* Each selected edge is duplicated to form a single edge on the extrusion. */
  const IndexRange duplicate_edge_range{connect_edge_range.one_after_last(),
                                        edge_selection.size()};
  /* Each edge selected for extrusion is extruded into a single face. */
  const IndexRange side_poly_range{orig_polys.size(), edge_selection.size()};
  const IndexRange side_loop_range{orig_loops.size(), side_poly_range.size() * 4};

  expand_mesh_size(mesh,
                   new_vert_range.size(),
                   connect_edge_range.size() + duplicate_edge_range.size(),
                   side_poly_range.size(),
                   side_loop_range.size());

  MutableSpan<MEdge> edges = mesh_edges(mesh);
  MutableSpan<MEdge> connect_edges = edges.slice(connect_edge_range);
  MutableSpan<MEdge> duplicate_edges = edges.slice(duplicate_edge_range);
  MutableSpan<MPoly> polys = mesh_polys(mesh);
  MutableSpan<MPoly> new_polys = polys.slice(side_poly_range);
  MutableSpan<MLoop> loops = mesh_loops(mesh);
  MutableSpan<MLoop> new_loops = loops.slice(side_loop_range);

  /* Initialize the edges that form the sides of the extrusion. */
  for (const int i : connect_edges.index_range()) {
    MEdge &edge = connect_edges[i];
    edge.v1 = new_vert_orig_indices[i];
    edge.v2 = new_vert_range[i];
    edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);
  }

  /* Initialize the edges that form the top of the extrusion. */
  for (const int i : duplicate_edges.index_range()) {
    const MEdge &orig_edge = edges[edge_selection[i]];
    MEdge &edge = duplicate_edges[i];
    edge.v1 = new_vert_indices[orig_edge.v1];
    edge.v2 = new_vert_indices[orig_edge.v2];
    edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);
  }

  /* Initialize the new side polygons. */
  for (const int i : new_polys.index_range()) {
    MPoly &poly = new_polys[i];
    poly.loopstart = side_loop_range[i * 4];
    poly.totloop = 4;
    poly.flag = 0;
  }

  /* Connect original edges that are in between two selected faces to the new vertices. */
  for (const int i : in_between_edges) {
    MEdge &edge = edges[i];
    if (new_vert_indices[edge.v1] != -1) {
      edge.v1 = new_vert_indices[edge.v1];
    }
    if (new_vert_indices[edge.v2] != -1) {
      edge.v2 = new_vert_indices[edge.v2];
    }
  }

  /* Connect the selected faces to the extruded/duplicated edges and the new vertices. */
  for (const int i_poly : poly_selection) {
    const MPoly &poly = polys[i_poly];
    for (MLoop &loop : loops.slice(poly.loopstart, poly.totloop)) {
      if (new_vert_indices[loop.v] != -1) {
        loop.v = new_vert_indices[loop.v];
      }
      if (duplicate_edge_indices[loop.e] != -1) {
        loop.e = duplicate_edge_range[duplicate_edge_indices[loop.e]];
      }
    }
  }

  for (const int i : edge_selection.index_range()) {
    const MEdge &duplicate_edge = duplicate_edges[i];
    const int new_vert_1 = duplicate_edge.v1;
    const int new_vert_2 = duplicate_edge.v2;
    const int extrude_index_1 = new_vert_1 - orig_vert_size;
    const int extrude_index_2 = new_vert_2 - orig_vert_size;

    const MPoly &extrude_poly = polys[edge_orig_face_indices[i]];

    fill_quad_consistent_direction(loops.slice(extrude_poly.loopstart, extrude_poly.totloop),
                                   new_loops.slice(4 * i, 4),
                                   new_vert_1,
                                   new_vert_2,
                                   new_vert_orig_indices[extrude_index_1],
                                   new_vert_orig_indices[extrude_index_2],
                                   duplicate_edge_range[i],
                                   connect_edge_range[extrude_index_1],
                                   edge_selection[i],
                                   connect_edge_range[extrude_index_2]);
  }

  /* Create a map of indices in the extruded vertices array to all of the indices of edges
   * in the duplicate edges array that connect to that vertex. This can be used to simplify the
   * mixing of attribute data for the connecting edges. */
  Array<Vector<int>> new_vert_to_duplicate_edge_map = create_vert_to_edge_map(
      new_vert_range.size(), duplicate_edges, orig_vert_size);

  component.attribute_foreach([&](const AttributeIDRef &id, const AttributeMetaData meta_data) {
    OutputAttribute attribute = component.attribute_try_get_for_output(
        id, meta_data.domain, meta_data.data_type);
    if (!attribute) {
      return true; /* Impossible to write the "normal" attribute. */
    }

    attribute_math::convert_to_static_type(meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);
      MutableSpan<T> data = attribute.as_span().typed<T>();
      switch (attribute.domain()) {
        case ATTR_DOMAIN_POINT: {
          /* New vertices copy the attributes from their original vertices. */
          copy_with_indices(data.slice(new_vert_range), data.as_span(), new_vert_orig_indices);
          break;
        }
        case ATTR_DOMAIN_EDGE: {
          /* Edges parallel to original edges copy the edge attributes from the original edges. */
          MutableSpan<T> duplicate_data = data.slice(duplicate_edge_range);
          copy_with_mask(duplicate_data, data.as_span(), edge_selection);

          /* Edges connected to original vertices mix values of selected connected edges. */
          MutableSpan<T> connect_data = data.slice(connect_edge_range);
          copy_with_mixing(connect_data, duplicate_data.as_span(), [&](const int i) {
            return new_vert_to_duplicate_edge_map[i].as_span();
          });
          break;
        }
        case ATTR_DOMAIN_FACE: {
          /* New faces on the side of extrusions get the values from the corresponding selected
           * face. */
          copy_with_indices(data.slice(side_poly_range), data.as_span(), edge_orig_face_indices);
          break;
        }
        case ATTR_DOMAIN_CORNER: {
          /* New corners get the values for the corresponding corner on the extruded face. */
          MutableSpan<T> new_data = data.slice(side_loop_range);
          threading::parallel_for(edge_selection.index_range(), 256, [&](const IndexRange range) {
            for (const int i_edge_selection : range) {
              const MPoly &poly = polys[edge_orig_face_indices[i_edge_selection]];

              const MEdge &duplicate_edge = duplicate_edges[i_edge_selection];
              const int new_vert_1 = duplicate_edge.v1;
              const int new_vert_2 = duplicate_edge.v2;
              const int orig_vert_1 = new_vert_orig_indices[new_vert_1 - orig_vert_size];
              const int orig_vert_2 = new_vert_orig_indices[new_vert_2 - orig_vert_size];

              /* Retrieve the data for the first two sides of the quad from the extruded polygon,
               * which we generally expect to have just a small amount of sides. This loop could be
               * eliminated by adding a cache of connected loops. */
              T data_1;
              T data_2;
              for (const int i_loop : IndexRange(poly.loopstart, poly.totloop)) {
                if (loops[i_loop].v == new_vert_1) {
                  data_1 = data[i_loop];
                }
                if (loops[i_loop].v == new_vert_2) {
                  data_2 = data[i_loop];
                }
              }

              /* Instead of replicating the order in #fill_quad_consistent_direction here, it's
               * simpler (though probably slower) to just match the corner data based on the vertex
               * indices. */
              for (const int i : IndexRange(4 * i_edge_selection, 4)) {
                if (ELEM(new_loops[i].v, new_vert_1, orig_vert_1)) {
                  new_data[i] = data_1;
                }
                else if (ELEM(new_loops[i].v, new_vert_2, orig_vert_2)) {
                  new_data[i] = data_2;
                }
              }
            }
          });
          break;
        }
        default:
          BLI_assert_unreachable();
      }
    });

    attribute.save();
    return true;
  });

  if (poly_offsets.is_single()) {
    const float3 offset = poly_offsets.get_internal_single();
    threading::parallel_for(
        IndexRange(all_selected_verts.size()), 1024, [&](const IndexRange range) {
          for (const int i_orig : all_selected_verts.as_span().slice(range)) {
            const int i_new = new_vert_indices[i_orig];
            /* If the vertex is used by a selected edge, it will have been duplicated and only the
             * new vertex should use the offset. Otherwise the vertex might still need an offset,
             * but it was reused on the inside of a group of extruded faces. */
            MVert &vert = mesh_verts(mesh)[(i_new == -1) ? i_orig : i_new];
            add_v3_v3(vert.co, offset);
          }
        });
  }
  else {
    threading::parallel_for(
        IndexRange(all_selected_verts.size()), 1024, [&](const IndexRange range) {
          for (const int i_orig : all_selected_verts.as_span().slice(range)) {
            const int i_new = new_vert_indices[i_orig];
            const float3 offset = vert_offsets[i_orig];
            /* If the vertex is used by a selected edge, it will have been duplicated and only the
             * new vertex should use the offset. Otherwise the vertex might still need an offset,
             * but it was reused on the inside of a group of extruded faces. */
            MVert &vert = mesh_verts(mesh)[(i_new == -1) ? i_orig : i_new];
            add_v3_v3(vert.co, offset);
          }
        });
  }

  BKE_mesh_runtime_clear_cache(&mesh);
  BKE_mesh_normals_tag_dirty(&mesh);

  if (attribute_outputs.top_id) {
    save_selection_as_attribute(
        component, attribute_outputs.top_id.get(), ATTR_DOMAIN_FACE, poly_selection);
  }
  if (attribute_outputs.side_id) {
    save_selection_as_attribute(
        component, attribute_outputs.side_id.get(), ATTR_DOMAIN_FACE, side_poly_range);
  }

  BLI_assert(BKE_mesh_is_valid(component.get_for_write()));
}

static IndexRange selected_corner_range(Span<int> offsets, const int index)
{
  const int offset = offsets[index];
  const int next_offset = offsets[index + 1];
  return IndexRange(offset, next_offset - offset);
}

static void extrude_individual_mesh_faces(MeshComponent &component,
                                          const Field<bool> &selection_field,
                                          const Field<float3> &offset_field,
                                          const AttributeOutputs &attribute_outputs)
{
  Mesh &mesh = *component.get_for_write();
  const int orig_vert_size = mesh.totvert;
  const int orig_edge_size = mesh.totedge;
  Span<MPoly> orig_polys = mesh_polys(mesh);
  Span<MLoop> orig_loops = mesh_loops(mesh);

  Array<float3> poly_offset(orig_polys.size());
  GeometryComponentFieldContext poly_context{component, ATTR_DOMAIN_FACE};
  FieldEvaluator poly_evaluator{poly_context, mesh.totpoly};
  poly_evaluator.set_selection(selection_field);
  poly_evaluator.add_with_destination(offset_field, poly_offset.as_mutable_span());
  poly_evaluator.evaluate();
  const IndexMask poly_selection = poly_evaluator.get_evaluated_selection_as_mask();

  int extrude_corner_size = 0;
  Array<int> index_offsets(poly_selection.size() + 1);
  for (const int i_selection : poly_selection.index_range()) {
    const MPoly &poly = orig_polys[poly_selection[i_selection]];
    index_offsets[i_selection] = extrude_corner_size;
    extrude_corner_size += poly.totloop;
  }
  index_offsets.last() = extrude_corner_size;

  const IndexRange new_vert_range{orig_vert_size, extrude_corner_size};
  /* One edge connects each selected vertex to a new vertex on the extruded polygons. */
  const IndexRange connect_edge_range{orig_edge_size, extrude_corner_size};
  /* Each selected edge is duplicated to form a single edge on the extrusion. */
  const IndexRange duplicate_edge_range{connect_edge_range.one_after_last(), extrude_corner_size};
  /* Each edge selected for extrusion is extruded into a single face. */
  const IndexRange side_poly_range{orig_polys.size(), duplicate_edge_range.size()};
  const IndexRange side_loop_range{orig_loops.size(), side_poly_range.size() * 4};

  expand_mesh_size(mesh,
                   new_vert_range.size(),
                   connect_edge_range.size() + duplicate_edge_range.size(),
                   side_poly_range.size(),
                   side_loop_range.size());

  MutableSpan<MVert> new_verts = mesh_verts(mesh).slice(new_vert_range);
  MutableSpan<MEdge> edges{mesh.medge, mesh.totedge};
  MutableSpan<MEdge> connect_edges = edges.slice(connect_edge_range);
  MutableSpan<MEdge> duplicate_edges = edges.slice(duplicate_edge_range);
  MutableSpan<MPoly> polys{mesh.mpoly, mesh.totpoly};
  MutableSpan<MPoly> new_polys = polys.slice(side_poly_range);
  MutableSpan<MLoop> loops{mesh.mloop, mesh.totloop};

  /* For every selected polygon, build the faces that form the sides of the extrusion. Filling some
   * of this data like the new edges or polygons could be easily split into separate loops, which
   * may or may not be faster, and would involve more duplication. */
  threading::parallel_for(poly_selection.index_range(), 256, [&](const IndexRange range) {
    for (const int i_selection : range) {
      const IndexRange poly_corner_range = selected_corner_range(index_offsets, i_selection);

      const MPoly &poly = polys[poly_selection[i_selection]];
      Span<MLoop> poly_loops = loops.slice(poly.loopstart, poly.totloop);

      for (const int i : IndexRange(poly.totloop)) {
        const int i_next = (i == poly.totloop - 1) ? 0 : i + 1;
        const MLoop &orig_loop = poly_loops[i];
        const MLoop &orig_loop_next = poly_loops[i_next];

        const int i_extrude = poly_corner_range[i];
        const int i_extrude_next = poly_corner_range[i_next];

        const int i_duplicate_edge = duplicate_edge_range[i_extrude];
        const int new_vert = new_vert_range[i_extrude];
        const int new_vert_next = new_vert_range[i_extrude_next];

        const int orig_edge = orig_loop.e;

        const int orig_vert = orig_loop.v;
        const int orig_vert_next = orig_loop_next.v;

        MEdge &duplicate_edge = duplicate_edges[i_extrude];
        duplicate_edge.v1 = new_vert_range[i_extrude];
        duplicate_edge.v2 = new_vert_range[i_extrude_next];
        duplicate_edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);

        MPoly &side_poly = new_polys[i_extrude];
        side_poly.loopstart = side_loop_range[i_extrude * 4];
        side_poly.totloop = 4;
        side_poly.flag = 0;

        MutableSpan<MLoop> side_loops = loops.slice(side_loop_range[i_extrude * 4], 4);
        side_loops[0].v = new_vert_next;
        side_loops[0].e = i_duplicate_edge;
        side_loops[1].v = new_vert;
        side_loops[1].e = connect_edge_range[i_extrude];
        side_loops[2].v = orig_vert;
        side_loops[2].e = orig_edge;
        side_loops[3].v = orig_vert_next;
        side_loops[3].e = connect_edge_range[i_extrude_next];

        MEdge &connect_edge = connect_edges[i_extrude];
        connect_edge.v1 = orig_vert;
        connect_edge.v2 = new_vert;
        connect_edge.flag = (ME_EDGEDRAW | ME_EDGERENDER);
      }
    }
  });

  component.attribute_foreach([&](const AttributeIDRef &id, const AttributeMetaData meta_data) {
    OutputAttribute attribute = component.attribute_try_get_for_output(
        id, meta_data.domain, meta_data.data_type);
    if (!attribute) {
      return true; /* Impossible to write the "normal" attribute. */
    }

    attribute_math::convert_to_static_type(meta_data.data_type, [&](auto dummy) {
      using T = decltype(dummy);
      MutableSpan<T> data = attribute.as_span().typed<T>();
      switch (attribute.domain()) {
        case ATTR_DOMAIN_POINT: {
          /* New vertices copy the attributes from their original vertices. */
          MutableSpan<T> new_data = data.slice(new_vert_range);

          threading::parallel_for(poly_selection.index_range(), 1024, [&](const IndexRange range) {
            for (const int i_selection : range) {
              const MPoly &poly = polys[poly_selection[i_selection]];
              Span<MLoop> poly_loops = loops.slice(poly.loopstart, poly.totloop);

              const int corner_offset = index_offsets[i_selection];
              for (const int i : poly_loops.index_range()) {
                const int orig_index = poly_loops[i].v;
                new_data[corner_offset + i] = data[orig_index];
              }
            }
          });
          break;
        }
        case ATTR_DOMAIN_EDGE: {
          MutableSpan<T> duplicate_data = data.slice(duplicate_edge_range);
          MutableSpan<T> connect_data = data.slice(connect_edge_range);

          threading::parallel_for(poly_selection.index_range(), 512, [&](const IndexRange range) {
            for (const int i_selection : range) {
              const MPoly &poly = polys[poly_selection[i_selection]];
              Span<MLoop> poly_loops = loops.slice(poly.loopstart, poly.totloop);

              const IndexRange poly_corner_range = selected_corner_range(index_offsets,
                                                                         i_selection);

              /* The data for the duplicate edge is simply a copy of the original edge's data. */
              for (const int i : poly_loops.index_range()) {
                const int orig_index = poly_loops[i].e;
                duplicate_data[poly_corner_range[i]] = data[orig_index];
              }

              /* For the extruded edges, mix the data from the two neighboring original edges of
               * the polygon. */
              for (const int i : poly_loops.index_range()) {
                const int i_loop_next = (i == poly.totloop - 1) ? 0 : i + 1;
                const int orig_index = poly_loops[i].e;
                const int orig_index_next = poly_loops[i_loop_next].e;
                connect_data[poly_corner_range[i]] = attribute_math::mix2(
                    0.5f, data[orig_index], data[orig_index_next]);
              }
            }
          });
          break;
        }
        case ATTR_DOMAIN_FACE: {
          /* Each side face gets the values from the correspoinding new face. */
          MutableSpan<T> new_data = data.slice(side_poly_range);
          threading::parallel_for(poly_selection.index_range(), 1024, [&](const IndexRange range) {
            for (const int i_selection : range) {
              const int poly_index = poly_selection[i_selection];
              const IndexRange poly_corner_range = selected_corner_range(index_offsets,
                                                                         i_selection);
              new_data.slice(poly_corner_range).fill(data[poly_index]);
            }
          });
          break;
        }
        case ATTR_DOMAIN_CORNER: {
          /* Each corner on a side face gets its value from the matching corner on an extruded
           * face. */
          MutableSpan<T> new_data = data.slice(side_loop_range);
          threading::parallel_for(poly_selection.index_range(), 256, [&](const IndexRange range) {
            for (const int i_selection : range) {
              const MPoly &poly = polys[poly_selection[i_selection]];
              Span<T> poly_loop_data = data.slice(poly.loopstart, poly.totloop);
              const IndexRange poly_corner_range = selected_corner_range(index_offsets,
                                                                         i_selection);

              for (const int i : IndexRange(poly.totloop)) {
                const int i_next = (i == poly.totloop - 1) ? 0 : i + 1;
                const int i_extrude = poly_corner_range[i];

                MutableSpan<T> side_loop_data = new_data.slice(i_extrude * 4, 4);

                /* The two corners on each side of the side polygon get the data from the matching
                 * corners of the extruded polygon. This order depends on the loop filling the loop
                 * indices. */
                side_loop_data[0] = poly_loop_data[i_next];
                side_loop_data[1] = poly_loop_data[i];
                side_loop_data[2] = poly_loop_data[i];
                side_loop_data[3] = poly_loop_data[i_next];
              }
            }
          });
          break;
        }
        default:
          BLI_assert_unreachable();
      }
    });

    attribute.save();
    return true;
  });

  /* Offset the new vertices. */
  threading::parallel_for(poly_selection.index_range(), 1024, [&](const IndexRange range) {
    for (const int i_selection : range) {
      const IndexRange poly_corner_range = selected_corner_range(index_offsets, i_selection);
      for (MVert &vert : new_verts.slice(poly_corner_range)) {
        add_v3_v3(vert.co, poly_offset[poly_selection[i_selection]]);
      }
    }
  });

  /* Finally update each extruded polygon's loops to point to the new edges and vertices.
   * This must be done last, because they were used to find original indices for attribute
   * interpolation before. Alternatively an original index array could be built for each domain. */
  threading::parallel_for(poly_selection.index_range(), 256, [&](const IndexRange range) {
    for (const int i_selection : range) {
      const IndexRange poly_corner_range = selected_corner_range(index_offsets, i_selection);

      const MPoly &poly = polys[poly_selection[i_selection]];
      MutableSpan<MLoop> poly_loops = loops.slice(poly.loopstart, poly.totloop);

      for (const int i : IndexRange(poly.totloop)) {
        MLoop &loop = poly_loops[i];
        loop.v = new_vert_range[poly_corner_range[i]];
        loop.e = duplicate_edge_range[poly_corner_range[i]];
      }
    }
  });

  BKE_mesh_runtime_clear_cache(&mesh);
  BKE_mesh_normals_tag_dirty(&mesh);

  if (attribute_outputs.top_id) {
    save_selection_as_attribute(
        component, attribute_outputs.top_id.get(), ATTR_DOMAIN_FACE, poly_selection);
  }
  if (attribute_outputs.side_id) {
    save_selection_as_attribute(
        component, attribute_outputs.side_id.get(), ATTR_DOMAIN_FACE, side_poly_range);
  }

  BLI_assert(BKE_mesh_is_valid(component.get_for_write()));
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  Field<float3> offset = params.extract_input<Field<float3>>("Offset");
  Field<float> strength = params.extract_input<Field<float>>("Strength");
  const NodeGeometryExtrudeMesh &storage = node_storage(params.node());
  GeometryNodeExtrudeMeshMode mode = static_cast<GeometryNodeExtrudeMeshMode>(storage.mode);

  /* Create a combined field from the offset and the strength so the field evaluator
   * can take care of the multiplication and to simplify each extrude function. */
  static fn::CustomMF_SI_SI_SO<float3, float, float3> multiply_fn{
      "Scale", [](const float3 &offset, const float strength) { return offset * strength; }};
  std::shared_ptr<FieldOperation> multiply_op = std::make_shared<FieldOperation>(
      FieldOperation(multiply_fn, {std::move(offset), std::move(strength)}));
  const Field<float3> final_offset{std::move(multiply_op)};

  AttributeOutputs attribute_outputs;
  if (params.output_is_required("Top")) {
    attribute_outputs.top_id = StrongAnonymousAttributeID("Top");
  }
  if (params.output_is_required("Side")) {
    attribute_outputs.side_id = StrongAnonymousAttributeID("Side");
  }

  const bool extrude_individual = mode == GEO_NODE_EXTRUDE_MESH_FACES &&
                                  params.extract_input<bool>("Individual");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      MeshComponent &component = geometry_set.get_component_for_write<MeshComponent>();
      switch (mode) {
        case GEO_NODE_EXTRUDE_MESH_VERTICES:
          extrude_mesh_vertices(component, selection, final_offset, attribute_outputs);
          break;
        case GEO_NODE_EXTRUDE_MESH_EDGES:
          extrude_mesh_edges(component, selection, final_offset, attribute_outputs);
          break;
        case GEO_NODE_EXTRUDE_MESH_FACES: {
          if (extrude_individual) {
            extrude_individual_mesh_faces(component, selection, final_offset, attribute_outputs);
          }
          else {
            extrude_mesh_face_regions(component, selection, final_offset, attribute_outputs);
          }
          break;
        }
      }
    }
  });

  params.set_output("Mesh", std::move(geometry_set));
  if (attribute_outputs.top_id) {
    params.set_output("Top",
                      AnonymousAttributeFieldInput::Create<bool>(
                          std::move(attribute_outputs.top_id), params.attribute_producer_name()));
  }
  if (attribute_outputs.side_id) {
    params.set_output("Side",
                      AnonymousAttributeFieldInput::Create<bool>(
                          std::move(attribute_outputs.side_id), params.attribute_producer_name()));
  }
}

}  // namespace blender::nodes::node_geo_extrude_mesh_cc

void register_node_type_geo_extrude_mesh()
{
  namespace file_ns = blender::nodes::node_geo_extrude_mesh_cc;

  static bNodeType ntype;
  geo_node_type_base(&ntype, GEO_NODE_EXTRUDE_MESH, "Extrude Mesh", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  node_type_init(&ntype, file_ns::node_init);
  node_type_update(&ntype, file_ns::node_update);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  node_type_storage(
      &ntype, "NodeGeometryExtrudeMesh", node_free_standard_storage, node_copy_standard_storage);
  ntype.draw_buttons = file_ns::node_layout;
  nodeRegisterType(&ntype);
}