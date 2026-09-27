
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(APPLE)
	enable_language(OBJCXX)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Concurrent Svg Qml Quick Widgets Network)
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.10.0")
	find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()

find_package(Qt6 COMPONENTS WebEngineQuick)
find_package(Qt6 COMPONENTS DBus)

if(WIN32)
	add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

set(SOURCE_FILES
	include/exception.h
	src/main.cpp
	include/discoverymanager.h
	src/discoverymanager.cpp
	include/streamsession.h
	src/streamsession.cpp
	include/sessionlog.h
	src/sessionlog.cpp
	include/settings.h
	src/settings.cpp
	include/host.h
	src/host.cpp
	res/resources.qrc
	include/controllermanager.h
	src/controllermanager.cpp
	include/psnaccountid.h
	src/psnaccountid.cpp
	include/psntoken.h
	src/psntoken.cpp
	include/jsonrequester.h
	src/jsonrequester.cpp
	include/nax5/nax5apiconfig.h
	src/nax5/nax5apiconfig.cpp
	include/nax5/nax5runtime.h
	src/nax5/nax5runtime.cpp
	include/nax5/nax5processlog.h
	src/nax5/nax5processlog.cpp
	include/nax5/nax5crashhandler.h
	src/nax5/nax5crashhandler.cpp
	include/nax5/nax5networkhint.h
	src/nax5/nax5networkhint.cpp
	include/nax5/nax5pathprobe.h
	src/nax5/nax5pathprobe.cpp
	include/nax5/nax5streamhealth.h
	src/nax5/nax5streamhealth.cpp
	include/nax5/nax5clientreport.h
	src/nax5/nax5clientreport.cpp
	include/nax5/nax5reportqueue.h
	src/nax5/nax5reportqueue.cpp
	include/nax5/nax5telemetry.h
	src/nax5/nax5installationid.cpp
	src/nax5/nax5telemetry.cpp
	include/nax5/nax5autherror.h
	src/nax5/nax5autherror.cpp
	include/nax5/nax5authparser.h
	src/nax5/nax5authparser.cpp
	include/nax5/nax5apilane.h
	src/nax5/nax5apilane.cpp
	include/nax5/nax5operatorhost.h
	src/nax5/nax5operatorhost.cpp
	include/nax5/nax5apiclient.h
	src/nax5/nax5apiclient.cpp
	include/nax5/nax5authstate.h
	src/nax5/nax5authstate.cpp
	include/nax5/nax5authcontroller.h
	src/nax5/nax5authcontroller.cpp
	include/nax5/nax5authstore.h
	src/nax5/nax5authstore.cpp
	include/nax5/session/nax5sessionerror.h
	src/nax5/session/nax5sessionerror.cpp
	include/nax5/session/nax5sessionstate.h
	src/nax5/session/nax5sessionstate.cpp
	include/nax5/session/nax5sessionlifecycle.h
	src/nax5/session/nax5sessionlifecycle.cpp
	include/nax5/session/nax5sessionparser.h
	src/nax5/session/nax5sessionparser.cpp
	include/nax5/session/nax5sessioncontroller.h
	src/nax5/session/nax5sessioncontroller.cpp
	include/nax5/connection/nax5connectionmaterial.h
	src/nax5/connection/nax5connectionmaterial.cpp
	include/nax5/connection/nax5connectionparser.h
	src/nax5/connection/nax5connectionparser.cpp
	include/nax5/connection/nax5transienthost.h
	src/nax5/connection/nax5transienthost.cpp
	src/qml/qml.qrc
	include/qmlmainwindow.h
	src/qmlmainwindow.cpp
	include/qmlbackend.h
	src/qmlbackend.cpp
	include/qmlcontroller.h
	src/qmlcontroller.cpp
	include/qmlsettings.h
	src/qmlsettings.cpp
	include/qmlsvgprovider.h
	src/qmlsvgprovider.cpp
	include/systemdinhibit.h
	src/systemdinhibit.cpp
	)
set(RESOURCE_FILES "")

set(NAX5_VERSION "unknown")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
	file(READ "${CMAKE_SOURCE_DIR}/VERSION" NAX5_VERSION)
	string(STRIP "${NAX5_VERSION}" NAX5_VERSION)
endif()
if(NOT NAX5_VERSION)
	set(NAX5_VERSION "unknown")
endif()
set(NAX5_CLIENT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE NAX5_CLIENT_SHA
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
endif()
if(NOT NAX5_CLIENT_SHA)
	set(NAX5_CLIENT_SHA "unknown")
endif()
message(STATUS "NAX5 product version ${NAX5_VERSION} (${NAX5_CLIENT_SHA})")

if(APPLE)
	list(APPEND RESOURCE_FILES "chiaking.icns")
elseif(WIN32)
	configure_file(
		${CMAKE_CURRENT_SOURCE_DIR}/chiaking.rc.in
		${CMAKE_CURRENT_SOURCE_DIR}/chiaking.rc
		@ONLY)
	list(APPEND RESOURCE_FILES "chiaking.rc")
endif()

if(CHIAKI_ENABLE_STEAMDECK_NATIVE AND LINUX)
	list(APPEND SOURCE_FILES
		include/sdinputcontext.h
		src/sdinputcontext.cpp
	)
endif()

if(APPLE)
	list(APPEND SOURCE_FILES
		include/macMicPermission.h
		src/macMicPermission.m
		include/macWakeSleep.h
		src/macWakeSleep.cpp
	)
elseif(WIN32)
	list(APPEND SOURCE_FILES
		include/windowsWakeSleep.h
		src/windowsWakeSleep.cpp
	)
endif()

add_executable(chiaki WIN32
		${RESOURCE_FILES}
		${SOURCE_FILES}
)
target_include_directories(chiaki PRIVATE include)
target_compile_definitions(chiaki PRIVATE
	NAX5_VERSION="${NAX5_VERSION}"
	NAX5_CLIENT_SHA="${NAX5_CLIENT_SHA}"
)

add_library(placebo-libav-impl src/libav_impl.c)
target_link_libraries(placebo-libav-impl PkgConfig::LIBPLACEBO FFMPEG::avcodec FFMPEG::avutil FFMPEG::avformat)
target_link_libraries(chiaki PkgConfig::LIBPLACEBO placebo-libav-impl)

target_link_libraries(chiaki chiaki-lib)

# Test-only stream replay (recorded media instead of a PS5). Never enable for
# product packages; release scripts configure with -DNAX5_STREAM_REPLAY=OFF.
option(NAX5_STREAM_REPLAY "Build chiaki with the recorded-media stream replay test mode" OFF)
if(NAX5_STREAM_REPLAY)
	target_sources(chiaki PRIVATE include/nax5/nax5streamreplay.h src/nax5/nax5streamreplay.cpp)
	target_compile_definitions(chiaki PRIVATE NAX5_STREAM_REPLAY)
endif()
if(CHIAKI_ENABLE_CLI)
	add_definitions(-DCHIAKI_ENABLE_CLI)
	target_link_libraries(chiaki chiaki-cli-lib)
endif()

target_link_libraries(chiaki Qt::Core Qt::Gui Qt::GuiPrivate Qt::Concurrent Qt::Svg Qt::Qml Qt::Quick Qt::Widgets Qt::Network)
if (${Qt6DBus_FOUND} AND NOT WIN32)
	target_link_libraries(chiaki Qt::DBus)
if (NOT APPLE)
	target_compile_definitions(chiaki PRIVATE CHIAKI_HAVE_DBUS)
endif()
endif()
if (${Qt6WebEngineQuick_FOUND})
	target_link_libraries(chiaki Qt::WebEngineQuick)
	target_compile_definitions(chiaki PRIVATE CHIAKI_HAVE_WEBENGINE)
endif()
target_link_libraries(chiaki SDL2::SDL2)
if(WIN32)
	target_link_libraries(chiaki iphlpapi ws2_32 wlanapi)
endif()
if(CHIAKI_ENABLE_SPEEX)
  target_link_libraries(chiaki PkgConfig::SpeexDSP)
  target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SPEEX)
endif()
if(CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER)
endif()
if(CHIAKI_ENABLE_SETSU)
	target_link_libraries(chiaki setsu)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SETSU)
endif()
if(CHIAKI_ENABLE_STEAMDECK_NATIVE)
	target_link_libraries(chiaki sdeck)
	target_compile_definitions(chiaki PRIVATE QT_STATICPLUGIN CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE)
endif()
if(CHIAKI_ENABLE_STEAM_SHORTCUT)
	target_link_libraries(chiaki cpp-steam-tools)
	target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_STEAM_SHORTCUT)
endif()
if(NOT CHIAKI_ENABLE_FFMPEG_DECODER)
	message(FATAL_ERROR "Chiaki GUI requires CHIAKI_ENABLE_FFMPEG_DECODER=ON")
endif()
if(APPLE)
	target_link_libraries(chiaki "-framework AVFoundation" "-framework IOKit")
endif()

# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fanalyzer")
set_target_properties(chiaki PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/MacOSXBundleInfo.plist.in"
		MACOSX_BUNDLE_BUNDLE_NAME chiaki-ng
		MACOSX_BUNDLE_BUNDLE_VERSION ${CHIAKI_VERSION}
		MACOSX_BUNDLE_COPYRIGHT "thestr4ng3r (AGPLv3)"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.streetpea.chiaking"
		MACOSX_BUNDLE_ICON_FILE chiaking.icns
		RESOURCE "${RESOURCE_FILES}")

install(TARGETS chiaki
		RUNTIME DESTINATION bin
		BUNDLE DESTINATION bin)
install(FILES chiaking.desktop DESTINATION share/applications)
install(FILES chiaking.png DESTINATION share/icons/hicolor/512x512/apps)
install(FILES io.github.streetpea.Chiaki4deck.appdata.xml DESTINATION share/metainfo)

if(CHIAKI_ENABLE_TESTS)
	add_executable(nax5-auth-unit
		src/nax5/nax5apiconfig.cpp
		src/nax5/nax5runtime.cpp
		src/nax5/nax5installationid.cpp
		src/nax5/nax5authstore.cpp
		src/nax5/nax5autherror.cpp
		src/nax5/nax5authstate.cpp
		src/nax5/nax5authparser.cpp
		src/nax5/nax5authparser_test.cpp
	)
	target_include_directories(nax5-auth-unit PRIVATE include)
	target_link_libraries(nax5-auth-unit Qt::Core)
	add_test(NAME nax5-auth COMMAND nax5-auth-unit)

	add_executable(nax5-session-unit
		src/nax5/nax5apiconfig.cpp
		src/nax5/nax5apilane.cpp
		src/nax5/nax5operatorhost.cpp
		src/nax5/session/nax5sessionerror.cpp
		src/nax5/session/nax5sessionstate.cpp
		src/nax5/session/nax5sessionlifecycle.cpp
		src/nax5/session/nax5sessionparser.cpp
		src/nax5/session/nax5sessionparser_test.cpp
	)
	target_include_directories(nax5-session-unit PRIVATE include)
	target_link_libraries(nax5-session-unit Qt::Core)
	add_test(NAME nax5-session COMMAND nax5-session-unit)

	add_executable(nax5-connection-unit
		src/nax5/nax5runtime.cpp
		src/nax5/nax5installationid.cpp
		src/nax5/session/nax5sessionerror.cpp
		src/nax5/session/nax5sessionstate.cpp
		src/nax5/session/nax5sessionparser.cpp
		src/nax5/connection/nax5connectionmaterial.cpp
		src/nax5/connection/nax5connectionparser.cpp
		src/nax5/connection/nax5connection_test.cpp
	)
	target_include_directories(nax5-connection-unit PRIVATE include)
	target_link_libraries(nax5-connection-unit Qt::Core)
	add_test(NAME nax5-connection COMMAND nax5-connection-unit)

	add_executable(nax5-telemetry-unit
		src/nax5/nax5runtime.cpp
		src/nax5/nax5installationid.cpp
		src/nax5/nax5processlog.cpp
		src/nax5/nax5networkhint.cpp
		src/nax5/nax5clientreport.cpp
		src/nax5/nax5reportqueue.cpp
		src/nax5/nax5telemetry.cpp
		src/nax5/nax5pathprobe.cpp
		src/nax5/nax5streamhealth.cpp
		src/nax5/nax5telemetry_test.cpp
	)
	target_include_directories(nax5-telemetry-unit PRIVATE include ${CMAKE_SOURCE_DIR}/lib/include)
	target_compile_definitions(nax5-telemetry-unit PRIVATE
		CHIAKI_VERSION="1.10.0"
		NAX5_VERSION="testver"
		NAX5_CLIENT_SHA="testsha"
	)
	target_link_libraries(nax5-telemetry-unit Qt::Core Qt::Network)
	if(WIN32)
		target_link_libraries(nax5-telemetry-unit iphlpapi ws2_32 wlanapi)
	endif()
	add_test(NAME nax5-telemetry COMMAND nax5-telemetry-unit)

	# Needs a live local backend: scripts/tests/integration/run-backend-integration.ps1.
	# Built with the unit tests but not registered with ctest.
	add_executable(nax5-integration
		src/nax5/nax5runtime.cpp
		src/nax5/nax5installationid.cpp
		src/nax5/nax5processlog.cpp
		src/nax5/nax5networkhint.cpp
		src/nax5/nax5clientreport.cpp
		src/nax5/nax5reportqueue.cpp
		src/nax5/nax5telemetry.cpp
		src/nax5/nax5apiconfig.cpp
		src/nax5/nax5apilane.cpp
		include/nax5/nax5apiclient.h
		src/nax5/nax5apiclient.cpp
		src/nax5/nax5autherror.cpp
		src/nax5/nax5authparser.cpp
		src/nax5/session/nax5sessionerror.cpp
		src/nax5/session/nax5sessionparser.cpp
		src/nax5/connection/nax5connectionmaterial.cpp
		src/nax5/connection/nax5connectionparser.cpp
		src/nax5/nax5integration_test.cpp
	)
	target_include_directories(nax5-integration PRIVATE include ${CMAKE_SOURCE_DIR}/lib/include)
	target_compile_definitions(nax5-integration PRIVATE
		CHIAKI_VERSION="1.10.0"
		NAX5_VERSION="alpha-0.5-build-13"
		NAX5_CLIENT_SHA="integration"
	)
	target_link_libraries(nax5-integration Qt::Core Qt::Network)
	if(WIN32)
		target_link_libraries(nax5-integration iphlpapi ws2_32 wlanapi)
	endif()
endif()
