# Draconic::Net.Manager - `draconic.net.manager`: a networked endpoint owned per running game
# (NetworkManager owns a live NetSession + RpcTable, driven each fixed step) + the `Net` script
# facade (Wren + AngelScript). Over draconic.net + draconic.script. See docs/design/networking.md §6.

add_library(Draconic.Net.Manager STATIC)
add_library(Draconic::Net.Manager ALIAS Draconic.Net.Manager)

target_link_libraries(Draconic.Net.Manager PUBLIC Draconic::Foundation Draconic::Net Draconic::Net.Replication Draconic::Scene Draconic::Script Draconic::Script.Facades Draconic::Policy)

target_sources(Draconic.Net.Manager
    PUBLIC
        FILE_SET net_manager_modules TYPE CXX_MODULES FILES
            NetworkManager.cppm
    PRIVATE
        NetworkManagerImpl.cpp)

