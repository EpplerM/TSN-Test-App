LLC ?= llc
CLANG ?= clang

BPF_CFLAGS ?=

# List of files that will be compiled into the bpf program
BPF_SRC := eth_type_filter.c
BPF_OBJ := ${BPF_SRC:.c=.o}

$(BPF_OBJ): %.o : %.c Makefile
	$(CLANG) -S \
		-target bpf \
		-D __BPF_TRACING__ \
		$(BPF_CFLAGS) \
		-Wall \
		-Wno-unused-value \
		-Wno-pointer-sign \
		-Wno-compare-distinct-pointer-types \
		-Werror \
		-O2 -emit-llvm -c -g -o ${@:.o=.ll} $<
	$(LLC) -march=bpf -filetype=obj -o $@ ${@:.o=.ll}

clean:
	rm -rf $(BPF_OBJ) $(BPF_OBJ:.o=.ll)
