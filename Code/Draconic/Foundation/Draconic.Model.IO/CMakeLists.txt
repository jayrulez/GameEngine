add_library(Draconic.Model.IO STATIC)
add_library(Draconic::Model.IO ALIAS Draconic.Model.IO)
target_link_libraries(Draconic.Model.IO PUBLIC Draconic::Foundation Draconic::Model Draconic::Policy)
target_sources(Draconic.Model.IO PUBLIC FILE_SET model_io_modules TYPE CXX_MODULES FILES ModelIOModule.cppm)
