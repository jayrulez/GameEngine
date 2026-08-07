# Draconic::Xml.Serialization - `draconic.xml.serialization`.
# An XML backend for Core's ISerializer contract. Depends on Core + Xml.

add_library(Draconic.Xml.Serialization STATIC)
add_library(Draconic::Xml.Serialization ALIAS Draconic.Xml.Serialization)

target_link_libraries(Draconic.Xml.Serialization PUBLIC Draconic::Foundation Draconic::Xml Draconic::Policy)

target_sources(Draconic.Xml.Serialization
    PUBLIC
        FILE_SET xml_serialization_modules TYPE CXX_MODULES FILES
            XmlSerializer.cppm)

