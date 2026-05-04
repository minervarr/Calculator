cmake_minimum_required(VERSION 3.22.1)
project(calculator)

set(                              NATIVE_APP_GLUE
                                  ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(                      calculator SHARED
		                              cpp/calculator.cc
                                  cpp/app.cc
                                  ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

target_include_directories(       calculator PRIVATE
                                  ${NATIVE_APP_GLUE}

)

target_link_libraries(		        calculator
		                              android
				                          log
                                  vulkan
)
