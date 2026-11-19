add_executable(datapath-benchmark
	benchmark/main.cpp
	benchmark/clock.hpp
	benchmark/stats.hpp
)
target_link_libraries(datapath-benchmark PRIVATE datapath)

add_executable(datapath-echo echo/main.cpp)
target_link_libraries(datapath-echo PRIVATE datapath)
