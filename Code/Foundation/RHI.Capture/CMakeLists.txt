# Foundation::RHI.Capture - `foundation.rhi.capture`: a presented backbuffer (or any copyable
# texture) to a PNG, and the --screenshot flags. RHI + image only, so the engine's applications,
# the editor and the sample framework all capture through it.

util_add_engine_library(RHI.Capture ALIAS Foundation::RHI.Capture)
target_link_libraries(RHI.Capture PUBLIC Foundation::Core Foundation::RHI Foundation::Image Foundation::Image.IO)

target_sources(RHI.Capture
    PUBLIC
        FILE_SET rhi_capture_modules TYPE CXX_MODULES FILES
            ScreenshotCapture.cppm)
