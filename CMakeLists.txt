cmake_minimum_required(VERSION 3.24)

project(Akrion VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 SerialPort)
qt_standard_project_setup(REQUIRES 6.5)

set(_qt_agent_safe_default OFF)
if(DEFINED ENV{CODEX_THREAD_ID})
    set(_qt_agent_safe_default ON)
endif()
option(
    QT_AGENT_SAFE_BUILD
    "Avoid CMake autogen subprocesses in restricted agent environments"
    ${_qt_agent_safe_default}
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(QtAgentSafe)

set(AKRION_CORE_SOURCES
    src/core/types.cpp
    src/core/types.h
    src/core/frame_decoder.cpp
    src/core/frame_decoder.h
    src/core/frame_validator.cpp
    src/core/frame_validator.h
    src/core/statistics.cpp
    src/core/statistics.h
    src/core/run_storage.cpp
    src/core/run_storage.h
    src/core/data_source.cpp
    src/core/data_source.h
    src/core/capture_session.cpp
    src/core/capture_session.h
    src/package/runpackage.cpp
    src/package/runpackage.h
)

qt_add_library(akrion-core STATIC ${AKRION_CORE_SOURCES})
set_target_properties(akrion-core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(akrion-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(akrion-core PUBLIC Qt6::Core Qt6::SerialPort)

qt_add_executable(akrion src/cli/main.cpp)
set_target_properties(akrion PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_link_libraries(akrion PRIVATE akrion-core Qt6::Core Qt6::SerialPort)

set(AKRION_GUI_SOURCES
    src/main.cpp
    src/gui/appcontroller.cpp
    src/gui/appcontroller.h
    src/gui/waveformbuffer.cpp
    src/gui/waveformbuffer.h
    src/gui/waveformcontroller.cpp
    src/gui/waveformcontroller.h
    src/gui/waveformview.cpp
    src/gui/waveformview.h
)
set(AKRION_QML_FILES
    qml/Main.qml
    qml/MetricTile.qml
    qml/WaveformPanel.qml
)
foreach(_qml_file IN LISTS AKRION_QML_FILES)
    get_filename_component(_qml_name "${_qml_file}" NAME)
    set_source_files_properties("${_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${_qml_name}")
endforeach()

qt_agent_add_qml_executable(akrion-gui
    URI Akrion
    VERSION 1.0
    RESOURCE_PREFIX "/qml"
    RESOURCE_BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES ${AKRION_GUI_SOURCES}
    MOC_HEADERS
        src/gui/appcontroller.h
        src/gui/waveformcontroller.h
        src/gui/waveformview.h
    QML_FILES ${AKRION_QML_FILES}
)
target_link_libraries(akrion-gui PRIVATE
    akrion-core
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::SerialPort
)
set_target_properties(akrion-gui PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE)

include(CTest)
if(BUILD_TESTING)
    add_executable(akrion-package-tests tests/runpackage_tests.cpp)
    set_target_properties(akrion-package-tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(akrion-package-tests PRIVATE akrion-core Qt6::Core)
    add_test(NAME akrion-package-tests COMMAND akrion-package-tests)
    set_tests_properties(akrion-package-tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_tests.cpp")
        add_executable(akrion-core-tests tests/core_tests.cpp)
        set_target_properties(akrion-core-tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        target_link_libraries(akrion-core-tests PRIVATE akrion-core Qt6::Core Qt6::SerialPort)
        add_test(NAME akrion-core-tests COMMAND akrion-core-tests)
        set_tests_properties(akrion-core-tests PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS akrion akrion-gui
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
