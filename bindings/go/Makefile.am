#
# Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2021. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

if HAVE_GO

EXTRA_DIST = \
	src \
	tests

GOOBJDIR=$(abs_top_builddir)/bindings/go/$(objdir)
CGOCFLAGS=-I$(abs_top_builddir)/src -I$(abs_top_srcdir)/src
CGOLDFLAGS=-L$(abs_top_builddir)/src/ucp/$(objdir) -lucp -L$(abs_top_builddir)/src/ucs/$(objdir) -lucs

if HAVE_CUDA
CGOLDFLAGS+=$(CUDA_LDFLAGS) $(CUDA_LIBS) $(CUDART_LIBS)
CGOCFLAGS+=$(CUDA_CPPFLAGS)
GOTAGS=cuda
else
GOTAGS=
endif

UCX_SOPATH=$(abs_top_builddir)/src/ucp/$(objdir):$(abs_top_builddir)/src/ucs/$(objdir):$(abs_top_builddir)/src/ucm/$(objdir):$(abs_top_builddir)/src/uct/$(objdir)
GOTMPDIR=$(GOOBJDIR)/tmp
GOCACHE=$(GOTMPDIR)

export GOTMPDIR
export GOCACHE
export CGO_CFLAGS=$(CGOCFLAGS)
export CGO_LDFLAGS=$(CGOLDFLAGS)

$(GOTMPDIR):
	$(AM_V_at)mkdir -p $(GOTMPDIR)

build: $(GOTMPDIR)
	cd $(abs_top_srcdir)/bindings/go/src/ucx ;\
	$(GO) build

clean-local:
	$(AM_V_at)-rm -rf $(GOOBJDIR)

test: $(GOTMPDIR)
	cd $(abs_top_srcdir)/bindings/go/tests ;\
	LD_LIBRARY_PATH=$(UCX_SOPATH):${LD_LIBRARY_PATH} $(GO) test -v --tags=$(GOTAGS)

bench: $(GOTMPDIR)
	cd $(abs_top_srcdir)/bindings/go/tests ;\
	LD_LIBRARY_PATH=$(UCX_SOPATH):${LD_LIBRARY_PATH} $(GO) test -v --tags=$(GOTAGS) -bench=.

goperftest: $(GOTMPDIR)
	cd $(abs_top_srcdir)/bindings/go/src/examples/perftest ;\
	$(GO) build --tags=$(GOTAGS) -o ${GOTMPDIR}/goperftest

run-perftest: goperftest
	cd $(abs_top_srcdir)/bindings/go/src/examples/perftest ;\
	LD_LIBRARY_PATH=$(UCX_SOPATH):${LD_LIBRARY_PATH} ${GOTMPDIR}/goperftest ${ARGS}

install-exec-hook: goperftest
	$(INSTALL) ${GOTMPDIR}/goperftest $(DESTDIR)$(bindir)

uninstall-hook:
	$(RM) $(DESTDIR)$(bindir)/goperftest

all: goperftest build

.PHONY: all build run_perftest test bench

endif
