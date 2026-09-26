set -e

# TARGET_SOC="rk3588"
GCC_COMPILER=aarch64-linux-gnu

export LD_LIBRARY_PATH=${TOOL_CHAIN}/lib64:$LD_LIBRARY_PATH
export CC=${GCC_COMPILER}-gcc
export CXX=${GCC_COMPILER}-g++

ROOT_PWD=$( cd "$( dirname $0 )" && cd -P "$( dirname "$SOURCE" )" && pwd )

# build
BUILD_DIR=${ROOT_PWD}/build/build_linux_aarch64

if [ ! -d "${BUILD_DIR}" ]; then
  mkdir -p ${BUILD_DIR}
fi

cd ${BUILD_DIR}
# 可选: -DENABLE_MPP=OFF / -DENABLE_RGA=OFF / -DMPP_ROOT=<mpp 安装目录>
cmake ../.. -DCMAKE_SYSTEM_NAME=Linux
make -j8
make install
cd -

# 快速运行(兼容旧用法): 单模型 + 单路视频 + 本地窗口
cd install/rknn_multi_stream_Linux/ && ./rknn_multi_stream ./model/RK3588/yolov5s-640-640.rknn ../../720p60hz.mp4
# 多路 + 多模型融合 + 推流: 按配置文件运行
# cd install/rknn_multi_stream_Linux/ && ./rknn_multi_stream -c config/app.ini
# 使用摄像头
# cd install/rknn_multi_stream_Linux/ && ./rknn_multi_stream ./model/RK3588/yolov5s-640-640.rknn 0
