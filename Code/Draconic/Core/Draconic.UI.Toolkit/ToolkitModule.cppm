// Draconic UI Toolkit - primary module interface unit for `draconic.ui.toolkit`.
//
// The editor-grade widget set (a faithful port of Sedulous.UI.Toolkit) layered on draconic.ui: docking,
// property grid, node graph, color/gradient/curve editors, and the menu/tool/status bars. One named
// module composed of partitions (one per control), re-exported here so consumers write a single
// `import draconic.ui.toolkit;`. Add `export import :partition;` lines as the port progresses (bottom-up:
// simple bars -> property grid -> pickers -> curves -> docking -> node graph -> theme extension).

export module draconic.ui.toolkit;

export import :status_bar;
export import :toolbar;
export import :menu_bar;
export import :split_view;
export import :breadcrumb_bar;
export import :color_picker;
export import :hdr_color_picker;
export import :gradient_editor;
export import :vector_fields;
export import :property_editor;
export import :bool_editor;
export import :button_editor;
export import :string_editor;
export import :int_editor;
export import :float_editor;
export import :range_editor;
export import :enum_editor;
export import :float2_editor;
export import :float3_editor;
export import :float4_editor;
export import :color_editor;
export import :property_grid;
export import :curve_canvas;
export import :dock_position;
export import :dock_layout_node;
export import :idock_host;
export import :idockable_window_host;
export import :dock_zone_indicator;
export import :dock_split;
export import :docking;
export import :node_graph_types;
export import :node_graph_canvas;
export import :draggable_tree_view;
export import :toast_host;
export import :code_document;
export import :code_lexer;
export import :code_edit_view;
export import :markup_completion;
export import :toolkit_theme_extension;
