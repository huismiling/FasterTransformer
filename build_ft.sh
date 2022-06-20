

mkdir build
cd build
cmake .. -DBUILD_TRT=ON -DTRT_PATH=/opt/tensorrt/
make -j
cd -
