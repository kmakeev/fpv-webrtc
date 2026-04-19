cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fpv_streamer)

# SPIFFS-образ из директории public/ — монтируется как /www в прошивке
spiffs_create_partition_image(storage ../public FLASH_IN_PROJECT)
