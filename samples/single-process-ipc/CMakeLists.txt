cmake_minimum_required(VERSION 3.5)
project(sample_single-process-ipc)

SET(PROJECT_SOURCES
	"${PROJECT_SOURCE_DIR}/main.cpp"
)

SET(PROJECT_LIBRARIES
	DataPath
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
