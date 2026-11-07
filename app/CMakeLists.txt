# MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-07.
# Older distribution SDKs lack security fixes needed when opening external
# documents and webpages. Release packaging additionally checks the exact pin.
find_package(Qt6 6.11.3 REQUIRED COMPONENTS Widgets Concurrent WebEngineWidgets Svg LinguistTools)
find_package(LibArchive REQUIRED)

qt_standard_project_setup()

# The entry point contains the inherited GUI regression harness (most of its
# size). Optimizing that harness takes substantial time and memory without
# improving comparison performance; the application views and engines keep
# the normal Release optimization settings.
if(MSVC)
  set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS /Od)
else()
  set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS -O0)
endif()

if(APPLE)
  set(LM_APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.icns)
  set_source_files_properties(${LM_APP_ICON} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_executable(MergeDesk
  ${LM_APP_ICON}
  main.cpp
  CommandLine.cpp
  Settings.cpp
  MainWindow.cpp
  MainWindowExtras.cpp
  MessageBoxes.cpp
  NewComparisonView.cpp
  FileCompareView.cpp
  TableCompareView.cpp
  TableFiles.cpp
  ImageCompareView.cpp
  ImagePane.cpp
  HexView.cpp
  HexCompareView.cpp
  HexDialogs.cpp
  FilepathEdit.cpp
  OptionsDialog.cpp
  FiltersDialog.cpp
  FileFilterCombo.cpp
  FileFilterMenu.cpp
  FilterConditionDialog.cpp
  ComparisonResultFilterDialog.cpp
  DisplayFilterBar.cpp
  LineFilterMenu.cpp
  MatchInsideDialog.cpp
  ReplaceLists.cpp
  FileFilters.cpp
  DiffTextEdit.cpp
  Icons.cpp
  ItemCheckStyle.cpp
  LocationPane.cpp
  PopupList.cpp
  SyntaxHighlighter.cpp
  Theme.cpp
  FolderCompareView.cpp
  FolderCompareDriver.cpp
  ArchiveCompare.cpp
  AboutDialog.cpp
  ProjectLinks.cpp
  StarPrompt.cpp
  WebCompareView.cpp
  SafeFileOps.cpp
  AutoMerge.cpp
  SessionIO.cpp
  ExportTools.cpp
  DocumentConverters.cpp
  MergeDeskFeatureTests.cpp
  MergeDeskSafetyTests.cpp
  MergeDeskBufferTests.cpp
  MergeDeskEncodingTests.cpp
  DesktopIntegration.cpp
  FileOps.cpp
  GoToDialog.cpp
  FindText.cpp
  PaneSearch.cpp
  FindDialogs.cpp
  TextMarkerDialog.cpp
  EngineOptions.cpp
)

qt_add_resources(MergeDesk "i18n"
  PREFIX "/i18n"
  BASE i18n
  FILES i18n/mergedesk_hu.qm i18n/mergedesk_de.qm i18n/mergedesk_fr.qm
)

# About box: the contributors list (shown by the Contributors button) and
# the application icon for the banner, both from outside app/
set(LM_ABOUT_CONTRIBUTORS ${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md)
set(LM_ABOUT_ICON ${CMAKE_SOURCE_DIR}/packaging/linux/icons/mergedesk-512.png)
set_source_files_properties(${LM_ABOUT_CONTRIBUTORS}
  PROPERTIES QT_RESOURCE_ALIAS CONTRIBUTORS.md)
set_source_files_properties(${LM_ABOUT_ICON}
  PROPERTIES QT_RESOURCE_ALIAS icon.png)
set_source_files_properties(${CMAKE_SOURCE_DIR}/assets/mergedesk.svg PROPERTIES QT_RESOURCE_ALIAS mergedesk.svg)
set_source_files_properties(${CMAKE_SOURCE_DIR}/LICENSE PROPERTIES QT_RESOURCE_ALIAS LICENSE)
set_source_files_properties(${CMAKE_SOURCE_DIR}/docs/MANUAL.md PROPERTIES QT_RESOURCE_ALIAS MANUAL.md)
qt_add_resources(MergeDesk "about"
  PREFIX "/about"
  FILES ${LM_ABOUT_CONTRIBUTORS} ${LM_ABOUT_ICON} ${CMAKE_SOURCE_DIR}/assets/mergedesk.svg ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/docs/MANUAL.md
)

# Linux desktop integration (--install-desktop-integration): the packaged
# desktop entry, retargeted at the AppImage, and the hicolor icons
set(LM_DESKTOP_FILES ${CMAKE_SOURCE_DIR}/packaging/linux/mergedesk.desktop)
set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/linux/mergedesk.desktop
  PROPERTIES QT_RESOURCE_ALIAS mergedesk.desktop)
foreach(_size 16 32 64 128 256 512)
  set(_icon ${CMAKE_SOURCE_DIR}/packaging/linux/icons/mergedesk-${_size}.png)
  set_source_files_properties(${_icon}
    PROPERTIES QT_RESOURCE_ALIAS icons/mergedesk-${_size}.png)
  list(APPEND LM_DESKTOP_FILES ${_icon})
endforeach()
qt_add_resources(MergeDesk "desktop"
  PREFIX "/desktop"
  FILES ${LM_DESKTOP_FILES}
)

if(APPLE)
  enable_language(OBJCXX)
  target_sources(MergeDesk PRIVATE MacServices.mm)
  target_link_libraries(MergeDesk PRIVATE "-framework AppKit")
  # localized titles for the Finder Services menu entry
  target_sources(MergeDesk PRIVATE
    ${CMAKE_SOURCE_DIR}/packaging/en.lproj/ServicesMenu.strings
    ${CMAKE_SOURCE_DIR}/packaging/ca.lproj/ServicesMenu.strings
    ${CMAKE_SOURCE_DIR}/packaging/de.lproj/ServicesMenu.strings
    ${CMAKE_SOURCE_DIR}/packaging/es.lproj/ServicesMenu.strings
    ${CMAKE_SOURCE_DIR}/packaging/fr.lproj/ServicesMenu.strings
    ${CMAKE_SOURCE_DIR}/packaging/pt.lproj/ServicesMenu.strings)
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/en.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/en.lproj")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/ca.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/ca.lproj")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/de.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/de.lproj")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/es.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/es.lproj")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/fr.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fr.lproj")
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/packaging/pt.lproj/ServicesMenu.strings
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/pt.lproj")
endif()

# WinMerge's preset file filters and the template of a new one, shipped as
# files (the engine reads them from a folder): the bundle's
# Resources/Filters on macOS, share/mergedesk/Filters elsewhere, and next
# to a build's binary so that it runs uninstalled
file(GLOB LM_FILTER_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/Filters/*.flt
  ${CMAKE_SOURCE_DIR}/Filters/FileFilter.tmpl)
if(APPLE)
  target_sources(MergeDesk PRIVATE ${LM_FILTER_FILES})
  set_source_files_properties(${LM_FILTER_FILES}
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/Filters")
else()
  add_custom_command(TARGET MergeDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:MergeDesk>/Filters
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LM_FILTER_FILES}
      $<TARGET_FILE_DIR:MergeDesk>/Filters)
endif()

# WinMerge's TreeSitterGrammars folder (engine/TreeSitterGrammars.cmake):
# the grammar modules and their queries, in the bundle's Resources on
# macOS and next to a build's binary elsewhere (installed under the
# libdir, see below)
if(LM_TREE_SITTER_GRAMMARS)
  add_dependencies(MergeDesk tree-sitter-grammars)
  if(APPLE)
    set(LM_TREE_SITTER_APP_DIR "$<TARGET_BUNDLE_CONTENT_DIR:MergeDesk>/Resources/TreeSitterGrammars")
  else()
    set(LM_TREE_SITTER_APP_DIR "$<TARGET_FILE_DIR:MergeDesk>/TreeSitterGrammars")
  endif()
  add_custom_command(TARGET MergeDesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LM_TREE_SITTER_GRAMMARS_DIR}" "${LM_TREE_SITTER_APP_DIR}")
endif()

# the macOS the bundle needs (LSMinimumSystemVersion): the deployment
# target of a release build, Qt 6.12's 14.4 otherwise, so that an older
# macOS says so instead of the app failing to start
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(LM_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(LM_MACOS_MINIMUM "14.4")
endif()

set_target_properties(MergeDesk PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_GUI_IDENTIFIER org.davoda1.MergeDesk
  MACOSX_BUNDLE_BUNDLE_VERSION 1.0.0
  MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0.0
  MACOSX_BUNDLE_ICON_FILE AppIcon.icns
  MACOSX_BUNDLE_BUNDLE_NAME MergeDesk
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/Info.plist.in
)

target_link_libraries(MergeDesk PRIVATE
  Qt6::Widgets
  Qt6::Concurrent
  Qt6::WebEngineWidgets
  Qt6::Svg
  LibArchive::LibArchive
  libremerge-engine
  libremerge-imgmerge
)

if(NOT WIN32)
  target_compile_options(MergeDesk PRIVATE
    -include ${CMAKE_SOURCE_DIR}/engine/ports/posix_compat.h)
endif()

if(UNIX AND NOT APPLE)
  # the desktop's light/dark setting, read from the XDG desktop portal
  find_package(Qt6 REQUIRED COMPONENTS DBus)
  target_sources(MergeDesk PRIVATE PortalAppearance.cpp)
  target_link_libraries(MergeDesk PRIVATE Qt6::DBus)
  target_compile_definitions(MergeDesk PRIVATE LM_HAVE_PORTAL)

  include(GNUInstallDirs)
  set_target_properties(MergeDesk PROPERTIES OUTPUT_NAME mergedesk)
  install(TARGETS MergeDesk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/mergedesk.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/linux/io.github.davoda1.mergedesk.appdata.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  install(FILES ${LM_FILTER_FILES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mergedesk/Filters)
  # the grammar modules are the platform's code: the libdir, with their
  # queries beside them; the binary finds them relative to itself
  set(LM_TREE_SITTER_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/mergedesk/TreeSitterGrammars)
  file(RELATIVE_PATH LM_TREE_SITTER_RELDIR "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}/mergedesk/TreeSitterGrammars")
  target_compile_definitions(MergeDesk PRIVATE
    LM_TREE_SITTER_RELDIR="${LM_TREE_SITTER_RELDIR}")
  if(LM_TREE_SITTER_GRAMMARS)
    install(DIRECTORY "${LM_TREE_SITTER_GRAMMARS_DIR}/"
      DESTINATION ${LM_TREE_SITTER_INSTALL_DIR} USE_SOURCE_PERMISSIONS)
  endif()
  foreach(_size 16 32 64 128 256 512)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/icons/mergedesk-${_size}.png
      DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
      RENAME mergedesk.png)
  endforeach()
endif()

if(WIN32)
  set_target_properties(MergeDesk PROPERTIES WIN32_EXECUTABLE TRUE)
  # Qt detects a manifest in the target sources and preserves it instead of
  # generating a second, conflicting application identity during finalization.
  target_sources(MergeDesk PRIVATE
    ${CMAKE_SOURCE_DIR}/packaging/windows/MergeDesk.rc
    ${CMAKE_SOURCE_DIR}/packaging/windows/MergeDesk.manifest)
  if(MSVC)
    # The custom manifest already declares the asInvoker execution level.
    target_link_options(MergeDesk PRIVATE /MANIFESTUAC:NO)
  endif()
endif()
