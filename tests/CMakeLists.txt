# MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-07.
# Upstream GoogleTest suite for the engine, running on the ported library.
# Windows-only suites (RegOptionsMgr, mystat) are excluded on POSIX.

set(TEST_SOURCES
  support/test_main.cpp
  BinaryCompare/BinaryCompare_test.cpp
  ByteCompare/ByteCompare_test.cpp
  DIffItemList/DiffItemList_test.cpp
  DiffCode/DiffCode_test.cpp
  DiffWrapper/DiffWrapper_test.cpp
  Encoding/charsets_test.cpp
  Encoding/codepage_detect_test.cpp
  ExistenceCompare/ExistenceCompare_test.cpp
  FileFilter/FileFilterHelper_test.cpp
  FilterEngine/FilterExpression_test.cpp
  FiltersList/FiltersList_test.cpp
  FullQuickCompare/FullQuickCompare_test.cpp
  LineFilterHelper/LineFilterHelper_test.cpp
  MoveDetection/RenameMoveDetection_test.cpp
  OptionsMgr/VariantValue_test.cpp
  Plugins/FileTransformPosix_test.cpp
  TreeSitter/TreeSitterParser_test.cpp
  StringDiffs/stringdiffs_test.cpp
  StringDiffs/stringdiffs_test_adds.cpp
  StringDiffs/stringdiffs_test_bugs.cpp
  StringDiffs/stringdiffs_test_bytelevel.cpp
  SubstitutionList/SubstitutionList_test.cpp
  TimeSizeCompare/TimeSizeCompare_test.cpp
  UnicodeString/UnicodeString_test.cpp
  diffutils/util_test.cpp
  unicoder/unicoder_test.cpp
  xdiff/xutils_test.cpp
)

if(WIN32)
  # upstream test asserts Windows path semantics (drive letters, backslash)
  list(APPEND TEST_SOURCES Paths/paths_test.cpp Paths/windows_utf8_test.cpp)
else()
  list(APPEND TEST_SOURCES Paths/paths_posix_test.cpp)
endif()

add_executable(libremerge-tests ${TEST_SOURCES})
if(WIN32)
  # Keep the engine tests under the application's UTF-8 process contract.
  # Attach the manifest to the executable, not to the engine's link interface.
  target_sources(libremerge-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/packaging/windows/MergeDesk.manifest)
  if(MSVC)
    target_link_options(libremerge-tests PRIVATE /MANIFESTUAC:NO)
  endif()
endif()

# Fixtures resolved relative to the test binary (env::GetProgPath())
add_custom_command(TARGET libremerge-tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/README.md $<TARGET_FILE_DIR:libremerge-tests>/../README.md
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/TestData $<TARGET_FILE_DIR:libremerge-tests>/../TestData
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/FileFilter/Filters $<TARGET_FILE_DIR:libremerge-tests>/../FileFilter/Filters
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/Data $<TARGET_FILE_DIR:libremerge-tests>/../../Data
)

target_link_libraries(libremerge-tests PRIVATE
  libremerge-engine
  GTest::gtest
)

# the Tree-sitter grammars of the build, for TreeSitterParser_test
add_dependencies(libremerge-tests tree-sitter-grammars)
target_compile_definitions(libremerge-tests PRIVATE
  LM_TEST_TREE_SITTER_DIR="${LM_TREE_SITTER_GRAMMARS_DIR}"
  LM_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT WIN32)
  target_compile_options(libremerge-tests PRIVATE
    -include ${CMAKE_SOURCE_DIR}/engine/ports/posix_compat.h)
endif()

# Image-compare core tests (needs Qt6 Gui; own binary with a
# QGuiApplication offscreen main)
include(GoogleTest)
if(TARGET libremerge-imgmerge)
  add_executable(libremerge-imgmerge-tests ImgMerge/ImgMergeBuffer_test.cpp)
  target_link_libraries(libremerge-imgmerge-tests PRIVATE
    libremerge-imgmerge
    GTest::gtest
  )
  gtest_discover_tests(libremerge-imgmerge-tests DISCOVERY_TIMEOUT 60)
endif()

# Tests use upstream-relative data paths (../TestData, ../../Data,
# ../../Src, ...). tests/run plus the Src/Externals symlinks at the repo
# root reproduce that layout.
include(GoogleTest)
# Several upstream engine fixtures intentionally reuse names such as A/B/C
# in their working directory. Serialize them across CTest processes while
# keeping the independent temporary-directory image tests parallelizable.
gtest_discover_tests(libremerge-tests
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/gtest/run
  DISCOVERY_TIMEOUT 60
  PROPERTIES RESOURCE_LOCK mergedesk-engine-fixtures
)
if(UNIX)
  add_test(NAME mergedesk-filter-dst COMMAND libremerge-tests
    --gtest_filter=OptimizationCases/FilterExpressionTest.Literals/*)
  set_tests_properties(mergedesk-filter-dst PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/gtest/run
    ENVIRONMENT "TZ=Europe/Budapest"
    RESOURCE_LOCK mergedesk-engine-fixtures)
endif()
