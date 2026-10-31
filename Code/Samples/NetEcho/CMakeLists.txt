# NetEcho - a console proof of foundation.net (reliable-UDP client/server over localhost). No graphics;
# just Core and Net, so it builds anywhere and runs headless (returns 0 when all lines round-trip).

add_executable(NetEcho main.cpp)
target_link_libraries(NetEcho PRIVATE Foundation::Core Foundation::Net Foundation::Policy)
