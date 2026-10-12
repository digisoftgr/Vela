add_library(vela_core STATIC
    FileItemModelBase.cpp
    FileSystemModel.cpp
    FileOperationsManager.cpp
    FileFilterProxyModel.cpp
    SearchEngine.cpp
    SearchResultsModel.cpp
    ThumbnailLoader.cpp
    MimeTypeResolver.cpp
    CommandRegistry.cpp
    VelaSettings.cpp
    JobTracker.cpp
    WorkerPools.cpp
)

target_include_directories(vela_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(vela_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent   # off-thread stat, previews (WorkerPools)
    KF6::KIOCore
    KF6::KIOGui
    KF6::KIOWidgets
    KF6::KIOFileWidgets
    KF6::JobWidgets   # KInhibitionJobTracker
    KF6::I18n
    KF6::GuiAddons
    KF6::ConfigCore
    KF6::CoreAddons   # KDirWatch
)
