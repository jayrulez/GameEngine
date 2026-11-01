# Foundation::Zip - `foundation.zip`: reading zip archives (stored and deflate, CRC-checked) and
# extracting them, for the sample downloads. Deflate is stb's zlib decoder, defined in Image.IO's
# stb unit (PNG needs it), so no library is added for it.

util_add_engine_library(Zip ALIAS Foundation::Zip)
target_link_libraries(Zip PUBLIC Foundation::Core Foundation::Policy PRIVATE Foundation::Image.IO ThirdParty::Stb)

target_sources(Zip
    PRIVATE
        ZipImpl.cpp
    PUBLIC
        FILE_SET zip_modules TYPE CXX_MODULES FILES
            Zip.cppm)
