# SPDX-License-Identifier: Apache-2.0
#
# JarvisRobot v5 — top-level plain-ESP-IDF project (ESP-IDF v5.5.4).
#
# This is the whole build: no esp-claw, no bootstrap.sh heredocs, no vendored-
# tree patching. Components under components/ are auto-discovered; main/ is the
# L6 composition root. The Waveshare board definition lives at boards/ and is
# turned into a generated component by:
#
#     idf.py gen-bmgr-config -c ./boards -b esp32s3_touch_amoled_1_75
#
# (see docs/reference/build-toolchain.md-equivalent notes in tools/idf-pins.txt).
#
# Build one-liner:
#     idf.py set-target esp32s3 && idf.py build
# Flash (DIO mandatory for CO5300 QSPI): scripts/flash-v5.sh
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jarvisrobot_v5)
