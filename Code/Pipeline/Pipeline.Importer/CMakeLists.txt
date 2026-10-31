# Pipeline::Importer - `pipeline.importer` (the file-import framework: IFileImporter,
# ImportOptions, ImporterRegistry and the deferred-write/copy helpers). Tooling only and
# headless-drivable: importers write sources and typed Assets through an injected
# ImportContext, so the editor's project model never appears here.

util_add_engine_library(Pipeline.Importer ALIAS Pipeline::Importer)
target_link_libraries(Pipeline.Importer PUBLIC Foundation::Core Foundation::Content Foundation::VFS Foundation::Policy Pipeline::Core)

target_sources(Pipeline.Importer
    PRIVATE
        ImporterImpl.cpp
    PUBLIC
        FILE_SET pipeline_modules TYPE CXX_MODULES FILES
            Importer.cppm)
