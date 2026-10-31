# Foundation::UI - `foundation.ui`, the retained-mode UI framework. Platform-agnostic
# core; the ui.shell bridge is a separate library. Built bottom-up (Core first).
util_add_engine_library(UI ALIAS Foundation::UI)
target_link_libraries(UI
    PUBLIC Foundation::Core Foundation::VG Foundation::VG.SVG Foundation::Fonts Foundation::Image Foundation::Policy Foundation::Xml)

# Embed the built-in .sss themes as generated module partitions (edit the .sss,
# configure re-embeds; CONFIGURE_DEPENDS makes touching the .sss reconfigure).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/dark.sss UI_DARK_THEME_SSS)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/light.sss UI_LIGHT_THEME_SSS)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/rounded-dark.sss UI_ROUNDED_DARK_THEME_SSS)
configure_file(Styling/Themes/EmbeddedThemes.cppm.in
               ${CMAKE_CURRENT_BINARY_DIR}/Generated/EmbeddedThemes.cppm @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/dark.sss
             ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/light.sss
             ${CMAKE_CURRENT_SOURCE_DIR}/Styling/Themes/rounded-dark.sss)

target_sources(UI
    PUBLIC
        FILE_SET ui_generated_modules TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/Generated
            FILES ${CMAKE_CURRENT_BINARY_DIR}/Generated/EmbeddedThemes.cppm
    PUBLIC
        FILE_SET ui_modules TYPE CXX_MODULES FILES
            Core/Enums.cppm
            Core/Thickness.cppm
            Core/ViewTransform.cppm
            Core/Transform2D.cppm
            Core/ViewId.cppm
            Core/Event.cppm
            Core/IPropertyOwner.cppm
            Core/Property.cppm
            Core/ICommand.cppm
            Core/IClipboard.cppm
            Input/InputEnums.cppm
            Input/EventArgs.cppm
            Input/IAcceleratorHandler.cppm
            Input/Shortcut.cppm
            Input/ShortcutManager.cppm
            Input/FocusManager.cppm
            Input/InputManager.cppm
            Overlay/PopupPositioner.cppm
            Overlay/TooltipPlacement.cppm
            Overlay/ITooltipProvider.cppm
            Overlay/TooltipManager.cppm
            DragDrop/DragDropEffects.cppm
            DragDrop/DragData.cppm
            DragDrop/IDragSource.cppm
            DragDrop/IDropTarget.cppm
            DragDrop/DragDropManager.cppm
            Animation/Easing.cppm
            Animation/Animation.cppm
            Animation/FloatAnimation.cppm
            Animation/ColorAnimation.cppm
            Animation/Float2Animation.cppm
            Animation/Storyboard.cppm
            Animation/AnimationManager.cppm
            Editing/InputFilter.cppm
            Controls/MomentumHelper.cppm
            Editing/UndoStack.cppm
            Editing/ITextEditHost.cppm
            Editing/TextEditingBehavior.cppm
            Core/View.cppm
            Data/IListAdapter.cppm
            Data/SelectionModel.cppm
            Data/ViewRecycler.cppm
            Data/ITreeAdapter.cppm
            Data/FlattenedTreeAdapter.cppm
            Overlay/IPopupOwner.cppm
            Overlay/PopupEntry.cppm
            Overlay/ModalBackdrop.cppm
            Overlay/PopupLayer.cppm
            Overlay/ContextMenu.cppm
            Overlay/Dialog.cppm
            Overlay/TooltipView.cppm
            DragDrop/DragAdorner.cppm
            Animation/ViewAnimator.cppm
            Styling/StyleProperty.cppm
            Styling/StyleValue.cppm
            Styling/StyleSelector.cppm
            Styling/StyleRule.cppm
            Styling/StyleSheet.cppm
            Styling/Palette.cppm
            Styling/ThemePalette.cppm
            Styling/IThemeExtension.cppm
            Styling/ThemeRegistry.cppm
            Styling/ThemeIcons.cppm
            Styling/ThemeIconSet.cppm
            Styling/DarkTheme.cppm
            Styling/GameTheme.cppm
            Styling/LightTheme.cppm
            Styling/RoundedDarkTheme.cppm
            Styling/TexturedTheme.cppm
            Styling/ThemeImageSet.cppm
            Styling/ThemeAtlas.cppm
            Styling/Parser/Token.cppm
            Styling/Parser/Tokenizer.cppm
            Styling/Parser/IResourceProvider.cppm
            Styling/Parser/Values.cppm
            Styling/Parser/ColorFunctions.cppm
            Styling/Parser/UITypeRegistry.cppm
            Styling/Parser/SSSParser.cppm
            Styling/Parser/StyleSheetLoader.cppm
            Debug/UIDebugDrawSettings.cppm
            Debug/UIDebugOverlay.cppm
            Drawing/ControlState.cppm
            Drawing/UIDrawContext.cppm
            Drawing/Drawable.cppm
            Drawing/ColorDrawable.cppm
            Drawing/RoundedRectDrawable.cppm
            Drawing/ShapeDrawable.cppm
            Drawing/GradientDrawable.cppm
            Drawing/InsetDrawable.cppm
            Drawing/LayerDrawable.cppm
            Drawing/StateListDrawable.cppm
            Drawing/ImageDrawable.cppm
            Drawing/NineSliceDrawable.cppm
            Drawing/AtlasImageDrawable.cppm
            Drawing/AtlasNineSliceDrawable.cppm
            Drawing/SVGDrawable.cppm
            Layout/Unit.cppm
            Layout/SizeSpec.cppm
            Layout/BoxConstraints.cppm
            Layout/Gravity.cppm
            Layout/GravityHelper.cppm
            Layout/LayoutStyle.cppm
            Controls/ButtonBase.cppm
            Controls/Button.cppm
            Controls/RepeatButton.cppm
            Controls/CheckBox.cppm
            Controls/Label.cppm
            Text/TextRuns.cppm
            Text/TextLayout.cppm
            Controls/RichText.cppm
            Controls/Panel.cppm
            Controls/Spacer.cppm
            Controls/Separator.cppm
            Controls/ColorView.cppm
            Controls/ProgressBar.cppm
            Controls/ImageView.cppm
            Controls/ToggleButton.cppm
            Controls/RadioButton.cppm
            Controls/RadioGroup.cppm
            Controls/ToggleSwitch.cppm
            Controls/Slider.cppm
            Controls/Expander.cppm
            Controls/EditText.cppm
            Controls/PasswordBox.cppm
            Controls/NumericField.cppm
            Controls/EditableLabel.cppm
            Controls/ScrollBar.cppm
            Controls/ScrollView.cppm
            Controls/ListView.cppm
            Controls/GridView.cppm
            Controls/TabView.cppm
            Controls/TreeView.cppm
            Controls/ComboBox.cppm
            Data/HierarchicalState.cppm
            Controls/ContentButton.cppm
            Controls/IconButton.cppm
            Controls/DrawableView.cppm
            Layout/FrameLayout.cppm
            Layout/AbsoluteLayout.cppm
            Layout/FlowLayout.cppm
            Layout/DockLayout.cppm
            Layout/GridLayout.cppm
            Layout/FlexLayout.cppm
            Markup/MarkupRegistry.cppm
            Markup/MarkupLoader.cppm
            UIModule.cppm
    PRIVATE
        # Module implementation units (not interface partitions): cross-cluster method bodies.
        Core/UIClusterImpl.cpp
        Input/InputImpl.cpp
        Overlay/TooltipImpl.cpp
        DragDrop/DragImpl.cpp
        Debug/UIDebugOverlayImpl.cpp
        Styling/Parser/SSSParserImpl.cpp
        Styling/Parser/UITypeRegistryImpl.cpp
        UiRegistryStateImpl.cpp)
