# Foundation::UI unit tests (doctest) - faithful ports of Sedulous.UI.Tests, added as each subsystem lands.
add_executable(UI.Tests
    TestsMain.cpp
    ThicknessTests.cpp
    ViewTransformTests.cpp
    ViewIdTests.cpp
    PropertyTests.cpp
    DrawableTests.cpp
    SliderFocusTests.cpp
    UnitTests.cpp
    SizeSpecTests.cpp
    BoxConstraintsTests.cpp
    GravityTests.cpp
    GravityHelperTests.cpp
    StyleSheetTests.cpp
    ThemeAtlasTests.cpp
    ThemeTests.cpp
    TokenizerTests.cpp
    SSSParserTests.cpp
    CascadeTests.cpp
    TestHelpers.cpp
    ViewTests.cpp
    ViewGroupTests.cpp
    UIContextTests.cpp
    LayoutTests.cpp
    LocalStyleSheetTests.cpp
    InputTests.cpp
    EditingTests.cpp
    EditTextTests.cpp
    TextRunsTests.cpp
    TextLayoutTests.cpp
    FontFamilyTests.cpp
    InlineStyleTests.cpp
    NumericFieldTests.cpp
    EditableLabelTests.cpp
    ScrollViewTests.cpp
    DataTests.cpp
    ListViewTests.cpp
    GridViewTests.cpp
    TabViewTests.cpp
    TreeViewTests.cpp
    ComboBoxTests.cpp
    PopupPositionerTests.cpp
    PopupLayerTests.cpp
    BoxMetricsTests.cpp
    ThemeSheetTests.cpp
    ContextMenuTests.cpp
    DialogTests.cpp
    AnimationTests.cpp
    MarkupLoaderTests.cpp
    BoxModelTests.cpp
    TransitionTests.cpp
    MeasureStoreTests.cpp
    FlexWrapTests.cpp
    DragDropTests.cpp
    TooltipTests.cpp
    MutationQueueTests.cpp
    PseudoElementTests.cpp
    WantsTextInputTests.cpp
    ControlTests.cpp)

target_link_libraries(UI.Tests PRIVATE
    Foundation::UI Foundation::Fonts.TrueType Foundation::VFS doctest::doctest)

target_compile_definitions(UI.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(UI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME UI.Tests COMMAND UI.Tests)
