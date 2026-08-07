add_library(Draconic.Model.GLTF STATIC)
add_library(Draconic::Model.GLTF ALIAS Draconic.Model.GLTF)
target_link_libraries(Draconic.Model.GLTF PUBLIC Draconic::Foundation Draconic::Model Draconic::Model.IO Draconic::Image Draconic::Image.IO Draconic::Policy PRIVATE Draconic::Cgltf)
target_sources(Draconic.Model.GLTF PUBLIC FILE_SET model_gltf_modules TYPE CXX_MODULES FILES GltfLoader.cppm PRIVATE cgltf_impl.cpp)
target_compile_definitions(Draconic.Model.GLTF PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(Draconic.Model.GLTF PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=missing-field-initializers;-Wno-error=unused-function;-Wno-error=unused-but-set-variable;-Wno-error=sign-compare;-Wno-error=implicit-fallthrough;-Wno-error=type-limits;-Wno-error=cast-function-type>"
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-language-extension-token>")
