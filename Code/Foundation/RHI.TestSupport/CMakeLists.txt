# Foundation::RHI.TestSupport - `foundation.rhi.testsupport`: backend-agnostic offscreen-render
# test support (make a device, read a target back to a CPU image to probe). Test-only; depends
# only on RHI and Core so any backend test can link it.

util_add_engine_library(RHI.TestSupport ALIAS Foundation::RHI.TestSupport)
target_link_libraries(RHI.TestSupport PUBLIC Foundation::RHI Foundation::Core Foundation::VFS)

target_sources(RHI.TestSupport
    PUBLIC
        FILE_SET rhi_testsupport_modules TYPE CXX_MODULES FILES
            RhiTestSupport.cppm)
