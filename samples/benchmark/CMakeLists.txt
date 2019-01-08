cmake_minimum_required(VERSION 3.5)
project(sample_benchmark)

SET(PROJECT_SOURCES
	"main.cpp"
	"measurer.hpp"
	"measurer.cpp"
)

SET(PROJECT_LIBRARIES
	datapath
)

# Includes
include_directories(
	${PROJECT_SOURCE_DIR}
)

# Building
ADD_EXECUTABLE(${PROJECT_NAME}
	${PROJECT_SOURCES}
)

# Linking
TARGET_LINK_LIBRARIES(${PROJECT_NAME}
	${PROJECT_LIBRARIES}
)
