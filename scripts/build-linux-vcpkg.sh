#!/bin/bash

cmake -S . \
      -B ./build-x64-linux \
      -G Ninja \
      -D CMAKE_BUILD_TYPE=Release \
      -D CMAKE_INSTALL_PREFIX:PATH=./build-x64-linux/vcpkg_installed/x64-linux \
      -D WITH_VCPKG=ON \
      -D BUILD_WITH_QT6=ON \
      -D VCPKG_TARGET_TRIPLET=x64-linux \
      -D WITH_QTWEBKIT=OFF \
      -D WITH_3D=OFF \
      -D WITH_BINDINGS=OFF \
      -D WITH_GRASS7=OFF \
      -D WITH_GRASS8=OFF \
      -D WITH_GRASS_PLUGIN=OFF \
      -D ENABLE_TESTS=OFF \
      -D VCPKG_MANIFEST_FEATURES="process;gui;desktop"

cmake --build ./build-x64-linux --config Release
