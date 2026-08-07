# Draconic::UI unit tests (doctest) - faithful ports of Sedulous.UI.Tests, added as each subsystem lands.
add_executable(Draconic.UI.Tests
    TestsMain.cpp
    ThicknessTests.cpp
    ViewTransformTests.cpp
    ViewIdTests.cpp
    PropertyTests.cpp
    DrawableTests.cpp
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
    TestHelpers.cpp
    ViewTests.cpp
    ViewGroupTests.cpp
    UIContextTests.cpp
    LayoutTests.cpp
    LocalStyleSheetTests.cpp
    InputTests.cpp
    EditingTests.cpp
    EditTextTests.cpp
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
    ContextMenuTests.cpp
    DialogTests.cpp
    AnimationTests.cpp
    MarkupLoaderTests.cpp
    MutationQueueTests.cpp
    PseudoElementTests.cpp
    WantsTextInputTests.cpp
    ControlTests.cpp)

target_link_libraries(Draconic.UI.Tests PRIVATE
    Draconic::UI doctest::doctest)

target_compile_definitions(Draconic.UI.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.UI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.UI.Tests COMMAND Draconic.UI.Tests)
