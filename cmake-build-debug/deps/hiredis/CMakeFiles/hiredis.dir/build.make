# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.15

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /cygdrive/c/Users/003822/.CLion2019.3/system/cygwin_cmake/bin/cmake.exe

# The command to remove a file.
RM = /cygdrive/c/Users/003822/.CLion2019.3/system/cygwin_cmake/bin/cmake.exe -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /cygdrive/e/idea/project/redis-source/redis6.x

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug

# Include any dependencies generated for this target.
include deps/hiredis/CMakeFiles/hiredis.dir/depend.make

# Include the progress variables for this target.
include deps/hiredis/CMakeFiles/hiredis.dir/progress.make

# Include the compile flags for this target's objects.
include deps/hiredis/CMakeFiles/hiredis.dir/flags.make

deps/hiredis/CMakeFiles/hiredis.dir/async.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/async.c.o: ../deps/hiredis/async.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/async.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/async.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/async.c

deps/hiredis/CMakeFiles/hiredis.dir/async.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/async.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/async.c > CMakeFiles/hiredis.dir/async.c.i

deps/hiredis/CMakeFiles/hiredis.dir/async.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/async.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/async.c -o CMakeFiles/hiredis.dir/async.c.s

deps/hiredis/CMakeFiles/hiredis.dir/dict.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/dict.c.o: ../deps/hiredis/dict.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/dict.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/dict.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/dict.c

deps/hiredis/CMakeFiles/hiredis.dir/dict.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/dict.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/dict.c > CMakeFiles/hiredis.dir/dict.c.i

deps/hiredis/CMakeFiles/hiredis.dir/dict.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/dict.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/dict.c -o CMakeFiles/hiredis.dir/dict.c.s

deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.o: ../deps/hiredis/hiredis.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/hiredis.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/hiredis.c

deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/hiredis.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/hiredis.c > CMakeFiles/hiredis.dir/hiredis.c.i

deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/hiredis.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/hiredis.c -o CMakeFiles/hiredis.dir/hiredis.c.s

deps/hiredis/CMakeFiles/hiredis.dir/net.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/net.c.o: ../deps/hiredis/net.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/net.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/net.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/net.c

deps/hiredis/CMakeFiles/hiredis.dir/net.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/net.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/net.c > CMakeFiles/hiredis.dir/net.c.i

deps/hiredis/CMakeFiles/hiredis.dir/net.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/net.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/net.c -o CMakeFiles/hiredis.dir/net.c.s

deps/hiredis/CMakeFiles/hiredis.dir/read.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/read.c.o: ../deps/hiredis/read.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_5) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/read.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/read.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/read.c

deps/hiredis/CMakeFiles/hiredis.dir/read.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/read.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/read.c > CMakeFiles/hiredis.dir/read.c.i

deps/hiredis/CMakeFiles/hiredis.dir/read.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/read.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/read.c -o CMakeFiles/hiredis.dir/read.c.s

deps/hiredis/CMakeFiles/hiredis.dir/sds.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/sds.c.o: ../deps/hiredis/sds.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_6) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/sds.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/sds.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sds.c

deps/hiredis/CMakeFiles/hiredis.dir/sds.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/sds.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sds.c > CMakeFiles/hiredis.dir/sds.c.i

deps/hiredis/CMakeFiles/hiredis.dir/sds.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/sds.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sds.c -o CMakeFiles/hiredis.dir/sds.c.s

deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.o: deps/hiredis/CMakeFiles/hiredis.dir/flags.make
deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.o: ../deps/hiredis/sockcompat.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_7) "Building C object deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.o"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/hiredis.dir/sockcompat.c.o   -c /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sockcompat.c

deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/hiredis.dir/sockcompat.c.i"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sockcompat.c > CMakeFiles/hiredis.dir/sockcompat.c.i

deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/hiredis.dir/sockcompat.c.s"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && E:/clion/cygwin64/bin/gcc.exe $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis/sockcompat.c -o CMakeFiles/hiredis.dir/sockcompat.c.s

# Object files for target hiredis
hiredis_OBJECTS = \
"CMakeFiles/hiredis.dir/async.c.o" \
"CMakeFiles/hiredis.dir/dict.c.o" \
"CMakeFiles/hiredis.dir/hiredis.c.o" \
"CMakeFiles/hiredis.dir/net.c.o" \
"CMakeFiles/hiredis.dir/read.c.o" \
"CMakeFiles/hiredis.dir/sds.c.o" \
"CMakeFiles/hiredis.dir/sockcompat.c.o"

# External object files for target hiredis
hiredis_EXTERNAL_OBJECTS =

deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/async.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/dict.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/hiredis.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/net.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/read.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/sds.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/sockcompat.c.o
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/build.make
deps/hiredis/cyghiredis.dll: deps/hiredis/CMakeFiles/hiredis.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_8) "Linking C shared library cyghiredis.dll"
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/hiredis.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
deps/hiredis/CMakeFiles/hiredis.dir/build: deps/hiredis/cyghiredis.dll

.PHONY : deps/hiredis/CMakeFiles/hiredis.dir/build

deps/hiredis/CMakeFiles/hiredis.dir/clean:
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis && $(CMAKE_COMMAND) -P CMakeFiles/hiredis.dir/cmake_clean.cmake
.PHONY : deps/hiredis/CMakeFiles/hiredis.dir/clean

deps/hiredis/CMakeFiles/hiredis.dir/depend:
	cd /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /cygdrive/e/idea/project/redis-source/redis6.x /cygdrive/e/idea/project/redis-source/redis6.x/deps/hiredis /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis /cygdrive/e/idea/project/redis-source/redis6.x/cmake-build-debug/deps/hiredis/CMakeFiles/hiredis.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : deps/hiredis/CMakeFiles/hiredis.dir/depend

