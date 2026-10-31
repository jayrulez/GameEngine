# Foundation::Net.Manager - `foundation.net.manager`: a networked endpoint owned per running game
# (NetworkManager owns a live NetSession and RpcTable, driven each fixed step) and the `Net` script
# facade (AngelScript and Luau). Over foundation.net and foundation.script.

util_add_engine_library(Net.Manager ALIAS Foundation::Net.Manager)
target_link_libraries(Net.Manager PUBLIC Foundation::Core Foundation::Net Foundation::Net.WebSocket Foundation::Net.Tls Foundation::Net.Replication Foundation::Scene Foundation::Script Foundation::Script.Facades Foundation::Profiler Foundation::Policy)

target_sources(Net.Manager
    PUBLIC
        FILE_SET net_manager_modules TYPE CXX_MODULES FILES
            NetworkManager.cppm
    PRIVATE
        NetworkManagerImpl.cpp)

