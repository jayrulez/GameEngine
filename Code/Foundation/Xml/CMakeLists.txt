# Foundation::Xml - `foundation.xml` (hand-written DOM XML parser and writer, UTF-8).
# Depends only on Core.

util_add_engine_library(Xml ALIAS Foundation::Xml)
target_link_libraries(Xml PUBLIC Foundation::Core Foundation::Policy)

target_sources(Xml
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

