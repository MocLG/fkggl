cmake_minimum_required(VERSION 3.22)
project(fkggl C)

# Used for standard NDK builds (e.g. Android Studio on x86_64 hosts).
# On other host architectures use scripts/build_native.sh instead; the
# Gradle build then packages the prebuilt libs from app/src/main/jniLibs.

add_library(photo STATIC photo.c)
target_compile_definitions(photo PRIVATE PHOTO_STATIC)
target_compile_options(photo PRIVATE -O2 -Wall)
target_include_directories(photo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(photo_jni SHARED photo_jni.c)
target_link_libraries(photo_jni PRIVATE photo m)
