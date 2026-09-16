# Native (host) unit tests. Deliberately separate from the top-level Makefile,
# which cross-builds the whole MiSTer binary for ARM. Only dependency-free
# modules can be tested here: anything including a MiSTer header pulls in
# linux/cdrom.h, which does not exist on the Windows dev machine.

CXX      ?= g++
CXXFLAGS  = -std=gnu++14 -Wall -Wextra -O1 -g
BUILDDIR  = build

SRC = ../support/minimig/minimig_savestate.cpp \
      ../support/minimig/minimig_a2065_crc32.cpp \
      ../support/minimig/minimig_profile.cpp \
      ../support/minimig/akiko_pace.cpp

TESTS = test_savestate_format test_amigacd_profile test_akiko_pace

# Shell tests, run before the compiled ones because they need no compiler and
# because of what they cover: prune_list() in scripts/deploy-core.sh is the only
# code in this repository that DELETES files, on the user's SD card, over ssh,
# without asking. It lives here rather than in a CI step of its own so that
# `make -C tests` covers it too -- a delete rule that is only checked on a push
# is checked after the push that broke it.
SHELL_TESTS = test_deploy_core_prune.sh

.PHONY: all clean shell-tests
all: shell-tests $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(BUILDDIR)/$$t || exit 1; done

shell-tests:
	@for t in $(SHELL_TESTS); do echo "== $$t"; bash $$t || exit 1; done

$(BUILDDIR)/%: %.cpp $(SRC)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

clean:
	rm -rf $(BUILDDIR)
