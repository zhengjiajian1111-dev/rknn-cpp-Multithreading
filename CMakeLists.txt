cmake_minimum_required(VERSION 3.10)

project(rknn_multi_stream CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-unused-function")

# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

option(ENABLE_MPP "Use Rockchip MPP for hardware decode/encode" ON)
option(ENABLE_RGA "Use Rockchip RGA for color conversion/resize" ON)

set(LIB_ARCH aarch64)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/rknn_multi_stream_${CMAKE_SYSTEM_NAME})
set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)

# ---------------- FFmpeg: 拉流解封装 / 推流封装 / 软件编解码兜底 ----------------
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)
pkg_check_modules(AVDEVICE libavdevice)

# ---------------- RKNN runtime ----------------
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so CACHE FILEPATH "librknnrt.so path")

# ---------------- RGA ----------------
set(RGA_PATH ${CMAKE_SOURCE_DIR}/include/3rdparty/rga/RK3588)
set(RGA_LIB ${RGA_PATH}/lib/Linux/${LIB_ARCH}/librga.so)

# ---------------- MPP: 板子上安装 librockchip-mpp-dev, 或用 -DMPP_ROOT=<mpp install dir> 指定 ----------------
if(ENABLE_MPP)
  find_path(MPP_INCLUDE_DIR rockchip/rk_mpi.h HINTS ${MPP_ROOT}/include /usr/include /usr/local/include)
  find_library(MPP_LIBRARY rockchip_mpp HINTS ${MPP_ROOT}/lib /usr/lib/aarch64-linux-gnu /usr/local/lib)
  if(MPP_INCLUDE_DIR AND MPP_LIBRARY)
    message(STATUS "MPP found: ${MPP_LIBRARY}")
  else()
    message(WARNING "MPP not found, falling back to FFmpeg software codec (install librockchip-mpp-dev for HW codec)")
    set(ENABLE_MPP OFF)
  endif()
endif()

set(SOURCES
  src/app/main.cc
  src/app/App.cc
  src/common/Config.cc
  src/common/Logger.cc
  src/common/PerfMonitor.cc
  src/common/RgaUtils.cc
  src/stream/StreamLoader.cc
  src/decode/VideoDecoder.cc
  src/decode/MppDecoder.cc
  src/decode/FfmpegDecoder.cc
  src/decode/DecodeWorker.cc
  src/infer/preprocess.cc
  src/infer/postprocess.cc
  src/infer/rknn_lite.cc
  src/infer/ModelManager.cc
  src/infer/InferWorker.cc
  src/fusion/DetectionFusion.cc
  src/output/Compositor.cc
  src/output/VideoEncoder.cc
  src/output/MppEncoder.cc
  src/output/FfmpegEncoder.cc
  src/output/FfmpegPushSink.cc
  src/output/Gb28181Sink.cc
  src/output/StreamingMgr.cc
  src/output/gb28181/Md5.cc
  src/output/gb28181/SipMessage.cc
  src/output/gb28181/PsMuxer.cc
)

add_executable(rknn_multi_stream ${SOURCES})

target_include_directories(rknn_multi_stream PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${OpenCV_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(rknn_multi_stream
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${FFMPEG_LDFLAGS}
  Threads::Threads
)

if(AVDEVICE_FOUND)
  target_compile_definitions(rknn_multi_stream PRIVATE HAVE_AVDEVICE)
  target_include_directories(rknn_multi_stream PRIVATE ${AVDEVICE_INCLUDE_DIRS})
  target_link_libraries(rknn_multi_stream ${AVDEVICE_LDFLAGS})
endif()

if(ENABLE_RGA)
  target_compile_definitions(rknn_multi_stream PRIVATE HAVE_RGA)
  target_include_directories(rknn_multi_stream PRIVATE ${RGA_PATH}/include)
  target_link_libraries(rknn_multi_stream ${RGA_LIB})
endif()

if(ENABLE_MPP)
  target_compile_definitions(rknn_multi_stream PRIVATE HAVE_MPP)
  target_include_directories(rknn_multi_stream PRIVATE ${MPP_INCLUDE_DIR})
  target_link_libraries(rknn_multi_stream ${MPP_LIBRARY})
endif()

install(TARGETS rknn_multi_stream DESTINATION ./)
install(PROGRAMS ${RKNN_RT_LIB} DESTINATION lib)
if(ENABLE_RGA)
  install(PROGRAMS ${RGA_LIB} DESTINATION lib)
endif()
install(DIRECTORY model DESTINATION ./)
install(DIRECTORY config DESTINATION ./)
