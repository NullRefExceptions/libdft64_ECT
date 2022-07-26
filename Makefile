# Dependencies:
# sudo apt-get install gcc-multilib g++-multilib

CC=clang
CXX=clang++

LIBDFT_SRC			= src
LIBDFT_TOOL			= tools
SSA_FLAG			= SSA_GC 			#SSA_GC | SSA_PROFILE | SSA_NOGC
TAINT_FLAG			=   				#-DTAINT_PROFILE | -DTAINT_VERIFY | -DTAINT_COUNT(only work on ssa tag)
TAG_FLAG			= -DTAG_SSA		#-DTAG_SSA | -DTAG_BDD | -DTAG_EWAH | -DTAG_SET | -DTAG_UINT8
export PIN_ROOT=/home/xd/jzz/projects/generator_ssa/tools/pin-3.19

.PHONY: all
all: dftsrc tool #test

.PHONY: dftsrc mytool
dftsrc: $(LIBDFT_SRC)
	cd $< && CPPFLAGS=$(CPPFLAGS) TAINT_FLAG=$(TAINT_FLAG) SSA_FLAG=$(SSA_FLAG) TAG_FLAG=$(TAG_FLAG) make -j 64

tool: $(LIBDFT_TOOL)
	# cd $< && TARGET=ia32 CPPFLAGS=$(CPPFLAGS)  make
	cd $< && TARGET=intel64 CPPFLAGS=$(CPPFLAGS) TAINT_FLAG=$(TAINT_FLAG) SSA_FLAG=$(SSA_FLAG) TAG_FLAG=$(TAG_FLAG) make

.PHONY: clean
clean:
	cd $(LIBDFT_SRC) && make clean
	cd $(LIBDFT_TOOL) && make clean
