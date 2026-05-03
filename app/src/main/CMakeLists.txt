cmake_minimum_required(VERSION 3.22.1)
project(calculator)

add_library(	calculator SHARED
		calculator.cc
)

target_link_libraries(		calculator
				android
				log
	
)
