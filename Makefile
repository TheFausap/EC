# EC - Enhanced C
#
#   make            build the ec preprocessor
#   make test       run the test suite
#   make examples   build and run the programs in examples/
#   make clean

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
ECC      = ./ec

all: ec

ec: ec.c
	$(CC) $(CFLAGS) -o $@ $<

# .ec -> .c
%.c: %.ec ec
	$(ECC) -o $@ $<

# .ec -> executable (translate, then compile)
%: %.ec ec
	$(ECC) -o $*.c $<
	$(CC) $(CFLAGS) $*.c -o $@ $(LDLIBS)

.PRECIOUS: %.c

test: ec
	@CC="$(CC)" CFLAGS="$(CFLAGS)" sh tests/run.sh

# examples may ask for ec flags on their first line:  // ec: --intel
examples: ec
	@for f in examples/*.ec; do \
	    n=$${f%.ec}; \
	    flags=`sed -n '1s|^.*// *ec: *||p' $$f`; \
	    echo "== $$f $$flags"; \
	    $(ECC) $$flags -o $$n.c $$f && $(CC) $(CFLAGS) $$n.c -o $$n && ./$$n || exit 1; \
	done

clean:
	rm -f ec
	rm -f examples/*.c
	rm -f examples/args examples/cmp2 examples/gcd examples/hello examples/bits
	rm -f tests/*.c tests/tmp.* 2>/dev/null || true

.PHONY: all test examples clean
