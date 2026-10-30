# Foundation::Http.Script - `foundation.http.script`: the `Http` script facade (get, post, send,
# cancel; answers by callback on the game's thread) over a run's IHttpClient, reached through a
# service on the script context as Net's and Save's are.

util_add_engine_library(Http.Script ALIAS Foundation::Http.Script)
target_link_libraries(Http.Script PUBLIC
    Foundation::Core Foundation::Http Foundation::Http.Client Foundation::Script Foundation::Script.Facades Foundation::Policy)

target_sources(Http.Script
    PRIVATE
        HttpScriptImpl.cpp
    PUBLIC
        FILE_SET http_script_modules TYPE CXX_MODULES FILES
            HttpScript.cppm)
