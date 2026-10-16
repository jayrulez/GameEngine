# Experimental::GUI unit tests (doctest), added per subsystem as the port lands.
add_executable(GUI.Tests
    TestsMain.cpp
    RectTests.cpp
    Transform2DTests.cpp
    DrawingTests.cpp
    GradientImageTests.cpp
    CompositorTests.cpp
    TransformableTests.cpp
    NodeTests.cpp
    ElementTests.cpp
    NodeDrawTests.cpp
    ActionTests.cpp
    SceneRootTests.cpp
    EventDispatcherTests.cpp
    TextTests.cpp
    ModelTests.cpp
    WidgetTests.cpp
    StyleSelectorTests.cpp
    StyleSheetTests.cpp
    StyleCoreTests.cpp
    BoxStyleTests.cpp
    CSSParserTests.cpp
    StyleApplierTests.cpp
    CssCascadeTests.cpp
    MediaQueryTests.cpp
    TransitionTests.cpp
    IncrementalFrameTests.cpp
    EffectsTests.cpp
    EasingTests.cpp
    StyleManagerTests.cpp
    LabelButtonTests.cpp
    LinearLayoutTests.cpp
    FlexLayoutTests.cpp
    CheckBoxTests.cpp
    SliderTests.cpp
    ProgressBarTests.cpp
    TextFieldTests.cpp
    LayoutTests.cpp
    ScrollTests.cpp
    RadioTests.cpp
    ImageWidgetTests.cpp
    ListBoxTests.cpp
    ListViewTests.cpp
    TableViewTests.cpp
    SortingProxyModelTests.cpp
    TreeViewTests.cpp
    TabWidgetTests.cpp
    WindowTests.cpp
    MessageBoxTests.cpp
    ComboBoxTests.cpp
    MenuTests.cpp
    MenuBarTests.cpp
    TooltipTests.cpp
    DragDropTests.cpp
    ThemeTests.cpp
    KeyframesTests.cpp
    MarkupTests.cpp)

target_link_libraries(GUI.Tests PRIVATE
    Experimental::GUI doctest::doctest)

target_compile_definitions(GUI.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Tests COMMAND GUI.Tests)
