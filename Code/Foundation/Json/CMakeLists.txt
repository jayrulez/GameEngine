# Foundation::Json - `foundation.json` (hand-rolled JSON DOM: value, parser and writer, UTF-8).
# The engine's JSON codec at its boundaries: the MCP wire protocol and data interchange a game
# chooses. The value type is RTTI-reflected so scripts can parse, build, query and stringify JSON.
# There is no ISerializer backend: JSON is not an engine data format (engine data stays XML).
# Depends only on Core.

util_add_engine_library(Json ALIAS Foundation::Json)
target_link_libraries(Json PUBLIC Foundation::Core Foundation::Policy)

# Reflection body (out-of-line impl unit): a regular source, not in the CXX_MODULES set.
target_sources(Json PRIVATE
    ReflectionImpl.cpp)   # REFLECT_VALUE(JsonValue) body (see Reflection.cppm)

target_sources(Json
    PUBLIC
        FILE_SET json_modules TYPE CXX_MODULES FILES
            Value.cppm
            Writer.cppm
            Parser.cppm
            Reflection.cppm
            JsonModule.cppm)
