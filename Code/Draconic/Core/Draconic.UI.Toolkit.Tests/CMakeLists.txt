add_executable(Draconic.UI.Toolkit.Tests
    StyleProbeTests.cpp
    StatusBarTests.cpp
    ToolbarTests.cpp
    MenuBarTests.cpp
    SplitViewTests.cpp
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
    DockingTests.cpp
    DockPersistenceTests.cpp
    NodeGraphTests.cpp
    DraggableTreeViewTests.cpp
    CodeDocumentTests.cpp
    CodeLexerTests.cpp
    CodeEditViewTests.cpp
    ToolkitThemeExtensionTests.cpp
    ToolkitTests.cpp
    TestsMain.cpp)

target_link_libraries(Draconic.UI.Toolkit.Tests PRIVATE
    Draconic::UI.Toolkit Draconic::UI Draconic::Foundation doctest::doctest)

target_compile_definitions(Draconic.UI.Toolkit.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.UI.Toolkit.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.UI.Toolkit.Tests COMMAND Draconic.UI.Toolkit.Tests)
