# Draconic::GUI - `draconic.gui`, a fresh eepp-derived retained-mode UI framework,
# progressively sliced onto the Draconic VG/Fonts stack (docs/design/gui-port.md).
# Platform- and renderer-agnostic core; VG/Fonts/Image/Xml are linked as those phases
# land, and the gui.shell input bridge is a separate library. Built bottom-up.
add_library(Draconic.GUI STATIC)
add_library(Draconic::GUI ALIAS Draconic.GUI)

target_link_libraries(Draconic.GUI
    PUBLIC Draconic::Foundation Draconic::VG Draconic::Image Draconic::Fonts Draconic::Xml Draconic::Policy)

target_sources(Draconic.GUI
    PUBLIC
        FILE_SET gui_modules TYPE CXX_MODULES FILES
            Core/Rect.cppm
            Core/Transform2D.cppm
            Core/Thickness.cppm
            Core/Transformable.cppm
            Core/Event.cppm
            Core/Clipboard.cppm
            Core/MutationQueue.cppm
            Core/Node.cppm
            Scene/Action.cppm
            Scene/ActionManager.cppm
            Scene/Actions.cppm
            Scene/EventDispatcher.cppm
            Scene/SceneNode.cppm
            Drawing/ControlState.cppm
            Drawing/DrawContext.cppm
            Drawing/Drawable.cppm
            Drawing/RectangleDrawable.cppm
            Drawing/BorderDrawable.cppm
            Drawing/StateListDrawable.cppm
            Drawing/LinearGradientDrawable.cppm
            Drawing/RadialGradientDrawable.cppm
            Drawing/ImageDrawable.cppm
            Drawing/NineSliceDrawable.cppm
            Drawing/LayerDrawable.cppm
            Text/Text.cppm
            Model/Variant.cppm
            Model/ModelIndex.cppm
            Model/Model.cppm
            Model/StringListModel.cppm
            Model/TableModel.cppm
            Model/TreeModel.cppm
            Model/SortingProxyModel.cppm
            Widgets/UINode.cppm
            Widgets/UIWidget.cppm
            Widgets/Label.cppm
            Widgets/Button.cppm
            Widgets/CheckBox.cppm
            Widgets/Radio.cppm
            Widgets/Slider.cppm
            Widgets/ProgressBar.cppm
            Widgets/LinearLayout.cppm
            Widgets/GridLayout.cppm
            Widgets/RelativeLayout.cppm
            Widgets/FlexLayout.cppm
            Widgets/TextField.cppm
            Widgets/ScrollBar.cppm
            Widgets/ScrollView.cppm
            Widgets/Image.cppm
            Widgets/ListBox.cppm
            Widgets/AbstractItemView.cppm
            Widgets/ListView.cppm
            Widgets/TableView.cppm
            Widgets/TreeView.cppm
            Widgets/TabWidget.cppm
            Widgets/Window.cppm
            Widgets/MessageBox.cppm
            Widgets/ComboBox.cppm
            Widgets/Menu.cppm
            Widgets/MenuBar.cppm
            Widgets/Tooltip.cppm
            Styling/ResourceProvider.cppm
            Styling/ParseUtil.cppm
            Styling/StyleSelector.cppm
            Styling/MediaQuery.cppm
            Styling/StyleRule.cppm
            Styling/StyleSheet.cppm
            Styling/CSSParser.cppm
            Styling/CSSValues.cppm
            Styling/StyleApplier.cppm
            Styling/Transition.cppm
            Styling/StyleManager.cppm
            Styling/Theme.cppm
            Markup/MarkupLoader.cppm
            GUIModule.cppm
    PRIVATE
        # Module implementation unit: Node method bodies that call the EventDispatcher
        # (kept out of :node to avoid a :node <-> :event_dispatcher partition cycle).
        Scene/GUIClusterImpl.cpp)

