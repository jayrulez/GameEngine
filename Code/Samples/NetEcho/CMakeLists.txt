# NetEcho - a console proof of draconic.net (reliable-UDP client/server over localhost). No graphics;
# just Foundation + Net, so it builds anywhere and runs headless (returns 0 when all lines round-trip).

add_executable(NetEcho main.cpp)
target_link_libraries(NetEcho PRIVATE Draconic::Foundation Draconic::Net Draconic::Policy)
