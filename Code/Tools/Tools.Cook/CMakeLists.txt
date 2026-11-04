# Tools.Cook - the command-line cooker (headless: no shell/GPU).
add_executable(Tools.Cook Main.cpp)
util_link_plugins(Tools.Cook ROLE cook PLUGINS ${UTIL_HOST_PLUGINS}) # what a project may enable
target_link_libraries(Tools.Cook PRIVATE
    Foundation::Core Foundation::Content Foundation::VFS
    Pipeline::Core Editor::Project Pipeline::Cook
    Pipeline::Registration # the composition root brings every builder/type registration
    Engine::Composition) # RegisterAllScriptFacades: the complete engine facade surface the
                           # script cooks compile against (run/ui/physics/audio/... facades)


# Its runtime dependencies beside it, listed in Tools.Cook.runtime-libs (the editor distribution ships
# the tool with them): the engine's own libraries in a shared build, and DXC, which its shader cooks
# load at run time.
util_copy_runtime_deps(Tools.Cook EXTRA ${BUILDSYSTEM_DXC_LIB} ${BUILDSYSTEM_NOTICES_FILE})
