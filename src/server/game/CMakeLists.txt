# This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# Exclude files in the LuaEngine directory for Mangos, CMangos and VMangos
CollectSourceFiles(
  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE_SOURCES
  # Exclude
  ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/Mangos
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/CMangos
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/VMangos)

if(USE_COREPCH)
  set(PRIVATE_PCH_HEADER PrecompiledHeaders/gamePCH.h)
endif()

GroupSources(${CMAKE_CURRENT_SOURCE_DIR})

add_definitions(-DTRINITY_API_EXPORT_GAME)

# Exclude files in the LuaEngine directory for Mangos, CMangos and VMangos
CollectIncludeDirectories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_INCLUDES
  # Exclude
  ${CMAKE_CURRENT_SOURCE_DIR}/PrecompiledHeaders
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/Mangos
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/CMangos
  ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/VMangos)

# Provide an interface target for the game project to allow
# dependent projects to build meanwhile.
add_library(game-interface INTERFACE)

target_include_directories(game-interface
  INTERFACE
    ${PUBLIC_INCLUDES})

target_link_libraries(game-interface
  INTERFACE
    shared
    Detour)

add_library(game
  ${PRIVATE_SOURCES})

target_include_directories(game
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(game
  PRIVATE
    trinity-core-interface
    efsw
  PUBLIC
    ipc
    game-interface)

set_target_properties(game
    PROPERTIES
      FOLDER
        "server")

if(UNITY_BUILDS)
  set_target_properties(game
      PROPERTIES UNITY_BUILD ON)
  set_target_properties(game
      PROPERTIES
        UNITY_BUILD_MODE BATCH
        UNITY_BUILD_BATCH_SIZE 8)
endif()

if(BUILD_SHARED_LIBS)
  if(UNIX)
    install(TARGETS game
      LIBRARY
        DESTINATION lib)
  elseif(WIN32)
    install(TARGETS game
      RUNTIME
        DESTINATION "${CMAKE_INSTALL_PREFIX}")
  endif()
endif()

# Generate precompiled header
if(USE_COREPCH)
  add_cxx_pch(game ${PRIVATE_PCH_HEADER})
endif ()

if(ELUNA)
  if(WIN32)
    if (MSVC)
      add_custom_command(TARGET game
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/$(ConfigurationName)/lua_scripts/extensions/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/extensions ${CMAKE_BINARY_DIR}/bin/$(ConfigurationName)/lua_scripts/extensions/
      )
    elseif(MINGW)
      add_custom_command(TARGET game
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/lua_scripts/extensions/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/LuaEngine/extensions ${CMAKE_BINARY_DIR}/bin/lua_scripts/extensions/
      )
    endif()
  endif()

  if(UNIX)
    install(DIRECTORY LuaEngine/extensions DESTINATION bin/lua_scripts/)
  elseif(WIN32)
    install(DIRECTORY LuaEngine/extensions DESTINATION "${CMAKE_BINARY_DIR}/bin/$(ConfigurationName)/lua_scripts/")
  endif()
endif()
