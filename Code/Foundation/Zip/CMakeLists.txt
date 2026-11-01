# Foundation::Zip - `foundation.zip`: reading zip archives (stored and deflate through zlib,
# CRC-checked) and extracting them, for the sample downloads.

util_add_engine_library(Zip ALIAS Foundation::Zip)
target_link_libraries(Zip PUBLIC Foundation::Core Foundation::Policy PRIVATE ThirdParty::Zlib)

target_sources(Zip
    PRIVATE
        ZipImpl.cpp
    PUBLIC
        FILE_SET zip_modules TYPE CXX_MODULES FILES
            Zip.cppm)
