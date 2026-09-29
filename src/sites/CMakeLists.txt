project(sites)

# Ninja invokes the Windows npm shim; CTest runs Jest through Node directly.
if(WIN32)
	set(NPM_COMMAND cmd.exe /d /c npm.cmd)
else()
	set(NPM_COMMAND npm)
endif()

# Get all TS files to transpile
file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/**.ts")

# Exclude type definitions and node_modules from the list
include(ListFilterRegex)
listFilterRegex(SOURCES ".*.d.ts$")
listFilterRegex(SOURCES "node_modules")

# Install NPM dependencies
add_custom_command(
	OUTPUT NPM_modules
	COMMAND ${NPM_COMMAND} ci --no-audit --no-fund
	COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/NPM_modules
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "Installing npm packages..."
	DEPENDS package.json package-lock.json
)
add_custom_target(sites_modules DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/NPM_modules SOURCES package.json package-lock.json)

# Transpiling TypeScript files into JavaScript
add_custom_command(
	OUTPUT JavaScript_sites
	COMMAND ${NPM_COMMAND} run build
	COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/JavaScript_sites
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "Transpiling TypeScript sources into JavaScript..."
	DEPENDS "${SOURCES}"
)

add_custom_target(sites DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/JavaScript_sites SOURCES "${SOURCES}")
add_dependencies(sites sites_modules)

find_program(NODE_EXECUTABLE NAMES node)
if(NOT NODE_EXECUTABLE)
	message(FATAL_ERROR "Node.js is required to run the site tests")
endif()

add_test(
	NAME sites
	COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/jest/bin/jest.js" --runInBand
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
