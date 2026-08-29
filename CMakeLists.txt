# SPDX-License-Identifier: Apache-2.0
#
# JarvisNano v5 — top-level plain ESP-IDF project (ESP-IDF v5.5.4).
#
# The release image is rooted here: no ESP-Claw/bootstrap patch layer.
# Components under components/ are auto-discovered and main/ is the composition
# root. `scripts/build-v5.sh` is the canonical entry point; it selects the 32 MB
# Waveshare 1.75C, generates board-manager code with
# `esp32s3_touch_amoled_1_75c`, synchronizes sdkconfig, applies pinned managed
# fixes, and runs the IDF build in Docker.
#
# Build: ./scripts/build-v5.sh
# Flash: ./scripts/flash-v5.sh
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jarvisrobot_v5)
