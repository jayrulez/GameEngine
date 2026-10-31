# Pipeline::Texture.Compression - `texture.compression`: the only code that includes the block-
# compression encoders (ThirdParty::Bc7enc, PRIVATE so the headers do not leak). Runs at cook and
# import time and is UI-free, like every Pipeline library.
util_add_engine_library(Texture.Compression ALIAS Pipeline::Texture.Compression)
target_link_libraries(Texture.Compression
    PUBLIC Foundation::Core Foundation::RHI
    PRIVATE ThirdParty::Bc7enc ThirdParty::Astcenc)

target_sources(Texture.Compression
    PRIVATE
        TextureCompressionImpl.cpp
        Bc6hEncoderImpl.cpp
        TextureCompressionReflectionImpl.cpp
    PUBLIC
        FILE_SET texcomp_modules TYPE CXX_MODULES FILES TextureCompression.cppm)
