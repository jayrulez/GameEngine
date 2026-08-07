# Draconic::Xml - `draconic.xml` (DOM XML parser + writer, UTF-8).
# Faithful port of the hand-written Sedulous.Xml library. Depends only on Core.

add_library(Draconic.Xml STATIC)
add_library(Draconic::Xml ALIAS Draconic.Xml)

target_link_libraries(Draconic.Xml PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Xml
    PUBLIC
        FILE_SET xml_modules TYPE CXX_MODULES FILES
            Result.cppm
            Lexer.cppm
            Escape.cppm
            Namespace.cppm
            Nodes.cppm
            Writer.cppm
            Document.cppm
            XmlModule.cppm)

