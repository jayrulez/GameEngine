# Foundation::Xml.Serialization - `foundation.xml.serialization`.
# An XML backend for Core's ISerializer contract. Depends on Core and Xml.

util_add_engine_library(Xml.Serialization ALIAS Foundation::Xml.Serialization)
target_link_libraries(Xml.Serialization PUBLIC Foundation::Core Foundation::Xml Foundation::Policy)

target_sources(Xml.Serialization
    PUBLIC
        FILE_SET xml_serialization_modules TYPE CXX_MODULES FILES
            XmlSerializer.cppm)

