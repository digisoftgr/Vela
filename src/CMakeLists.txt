add_subdirectory(core)

set(VELA_SRCS
    main.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
    core/FileManagerDBus.cpp
    ui/MainWindow.cpp
    ui/Panel.cpp
    ui/NavigationHistory.cpp
    ui/ViewModeSlider.cpp
    ui/OperationsWindow.cpp
    ui/FilterBar.cpp
    ui/MatchHighlightDelegate.cpp
    ui/FileTreeView.cpp
    ui/MessageBoxUtil.cpp
    ui/BreadcrumbBar.cpp
    ui/OpenInListEditor.cpp
    ui/Sidebar.cpp
    ui/MountWatcher.cpp
    ui/CommandPalette.cpp
    ui/SettingsDialog.cpp
    ui/DropZoneOverlay.cpp
    ui/PanelContainer.cpp
    ui/TerminalPanel.cpp
    ui/SortLockHeaderView.cpp
    ui/preview/PreviewPane.cpp
    ui/preview/PreviewContainer.cpp
    ui/preview/FolderPreview.cpp
    ui/preview/ImagePreview.cpp
    ui/preview/PdfPreview.cpp
    ui/preview/TextPreview.cpp
)

add_executable(vela ${VELA_SRCS})

target_include_directories(vela PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vela
    vela_core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::DBus
    KF6::CoreAddons
    KF6::WidgetsAddons
    KF6::Bookmarks
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::XmlGui
    KF6::KIOFileWidgets
    KF6::Solid
    KF6::GlobalAccel
    KF6::ColorScheme
    KF6::SyntaxHighlighting
    KF6::Service    # KApplicationTrader / KSycoca (default file manager setting)
    KF6::Parts      # embedded terminal (konsolepart)
    Poppler::Qt6
)


add_custom_command(TARGET vela POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_BINARY_DIR}/share/kxmlgui5/vela
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/resources/velaui.rc
        ${CMAKE_BINARY_DIR}/share/kxmlgui5/vela/velaui.rc
    COMMENT "Staging velaui.rc into build tree"
)

add_custom_command(TARGET vela POST_BUILD
    COMMAND chmod +x $<TARGET_FILE:vela>
    COMMENT "Fixing execute bit (NTFS filesystem)"
)

install(TARGETS vela DESTINATION ${KDE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/resources/velaui.rc
    DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/vela)
