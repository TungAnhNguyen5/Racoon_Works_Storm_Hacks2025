#!/bin/bash

# Build script for Racoon Works Storm Hacks 2025

echo "Setting up CMake build in build/ directory..."

# Create build directory if it doesn't exist
mkdir -p build

# Configure CMake
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the project
echo "Building project..."
cmake --build . --config Release

echo "Build completed!"
echo "Executables are in the build/ directory:"
ls -la scheduler baseline 2>/dev/null || echo "Check build output for errors"

cd ..
echo "To run:"
echo "  ./build/scheduler input/example1.txt"
echo "  ./build/baseline input/example1.txt"