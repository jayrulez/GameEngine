# Foundation::UI.Toolkit - `foundation.ui.toolkit`: the editor-grade widget set (docking, property grid,
# node graph, color pickers, curve editor, menu/tool/status bars) layered on foundation.ui. Faithful port
# of Sedulous.UI.Toolkit. A separate library/module so the core foundation.ui stays lean; consumers write
# `import foundation.ui.toolkit;`.
util_add_engine_library(UI.Toolkit ALIAS Foundation::UI.Toolkit)
target_link_libraries(UI.Toolkit
    PUBLIC Foundation::Core Foundation::VG Foundation::VG.SVG Foundation::Fonts Foundation::Image Foundation::UI Foundation::Policy)

# Embed the toolkit .sss styling fragments as a generated module partition (same pattern as
# the core themes in Foundation/UI: edit the .sss, configure re-embeds).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Themes/toolkit-dark.sss UITOOLKIT_DARK_SSS)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Themes/toolkit-light.sss UITOOLKIT_LIGHT_SSS)
configure_file(Themes/EmbeddedToolkitThemes.cppm.in
               ${CMAKE_CURRENT_BINARY_DIR}/Generated/EmbeddedToolkitThemes.cppm @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/Themes/toolkit-dark.sss
             ${CMAKE_CURRENT_SOURCE_DIR}/Themes/toolkit-light.sss)

target_sources(UI.Toolkit
    PUBLIC
        FILE_SET uitoolkit_generated_modules TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/Generated
            FILES ${CMAKE_CURRENT_BINARY_DIR}/Generated/EmbeddedToolkitThemes.cppm
    PUBLIC
        FILE_SET uitoolkit_modules TYPE CXX_MODULES FILES
            StatusBar.cppm
            ToastHost.cppm
            Toolbar.cppm
            MenuBar.cppm
            SplitView.cppm
            BottomDock.cppm
            BreadcrumbBar.cppm
            ColorPicker.cppm
            HDRColorPicker.cppm
            GradientEditor.cppm
            VectorFields.cppm
            Float2Editor.cppm
            Float4Editor.cppm
            PropertyEditor.cppm
            BoolEditor.cppm
            ButtonEditor.cppm
            StringEditor.cppm
            IntEditor.cppm
            FloatEditor.cppm
            RangeEditor.cppm
            EnumEditor.cppm
            Float3Editor.cppm
            ColorEditor.cppm
            PropertyGrid.cppm
            CurveCanvas.cppm
            Timeline.cppm
            DockPosition.cppm
            DockLayoutNode.cppm
            IDockHost.cppm
            IDockableWindowHost.cppm
            DockZoneIndicator.cppm
            DockSplit.cppm
            Docking.cppm
            NodeGraphTypes.cppm
            NodeGraphCanvas.cppm
            DraggableTreeView.cppm
            Code/CodeDocument.cppm
            Code/CodeLexer.cppm
            Code/CodeLexerScan.cppm
            Code/CodeEditView.cppm
            Code/MarkupCompletion.cppm
            FloatingPanel.cppm
            ToolkitThemeExtension.cppm
            ToolkitModule.cppm)

# Implementation units for the code-editor stack (declared in the Code/ partitions).
target_sources(UI.Toolkit PRIVATE
    Code/CodeHighlighter.cpp
    Code/CLikeLexer.cpp
    Code/LuaLikeLexer.cpp
    Code/XmlLexer.cpp
    Code/CodeLexerRegistry.cpp)

