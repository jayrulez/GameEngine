add_executable(UI.Toolkit.Tests
    StyleProbeTests.cpp
    StatusBarTests.cpp
    ToolbarTests.cpp
    MenuBarTests.cpp
    SplitViewTests.cpp
    FloatingPanelTests.cpp
    BottomDockTests.cpp
    BreadcrumbBarTests.cpp
    ColorPickerTests.cpp
    HDRColorPickerTests.cpp
    GradientEditorTests.cpp
    VectorFieldsTests.cpp
    BoolEditorTests.cpp
    ButtonEditorTests.cpp
    StringEditorTests.cpp
    IntEditorTests.cpp
    FloatEditorTests.cpp
    RangeEditorTests.cpp
    EnumEditorTests.cpp
    Float2EditorTests.cpp Float3EditorTests.cpp Float4EditorTests.cpp
    ColorEditorTests.cpp
    PropertyGridTests.cpp
    ToastHostTests.cpp
    CurveCanvasTests.cpp
    TimelineTests.cpp
    DockingTests.cpp
    DockPersistenceTests.cpp
    GridDrawCostTests.cpp
    NodeGraphTests.cpp
    DraggableTreeViewTests.cpp
    CodeDocumentTests.cpp
    CodeLexerTests.cpp
    CodeEditViewTests.cpp
    ToolkitThemeExtensionTests.cpp
    ToolkitThemeSheetTests.cpp
    ToolkitTests.cpp
    TestsMain.cpp)

target_link_libraries(UI.Toolkit.Tests PRIVATE
    Foundation::UI.Toolkit Foundation::UI Foundation::Core doctest::doctest)

target_compile_definitions(UI.Toolkit.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(UI.Toolkit.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME UI.Toolkit.Tests COMMAND UI.Toolkit.Tests)
