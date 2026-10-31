# Foundation::I18N - `foundation.i18n`: a game's and the editor's text in several languages.
# String tables that describe their languages whole (texts, plural
# forms, each locale's plural rule and direction), a resolver merging them with a fallback chain,
# positional formatting, plural forms and pseudolocalization. A leaf over Core: no resource, RHI,
# UI or messaging; whoever needs text owns a Localization.
util_add_engine_library(I18N ALIAS Foundation::I18N)
target_link_libraries(I18N PUBLIC Foundation::Core Foundation::Policy)

target_sources(I18N
    PUBLIC
        FILE_SET i18n_modules TYPE CXX_MODULES FILES
            I18NModule.cppm
            Locale.cppm
            PluralRule.cppm
            StringTable.cppm
            Localization.cppm
    PRIVATE
        StringTableImpl.cpp)
