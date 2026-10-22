# Native (host) unit tests. Deliberately separate from the top-level Makefile,
# which cross-builds the whole MiSTer binary for ARM. Only dependency-free
# modules can be tested here: anything including a MiSTer header pulls in
# linux/cdrom.h, which does not exist on the Windows dev machine.

CXX      ?= g++
# -MMD -MP writes a .d file listing every header each binary actually included,
# which is pulled back in below. Without it a test binary depended on the .cpp
# files alone, so editing a HEADER rebuilt nothing and `make` happily re-ran the
# previous binary -- which silently defeats the first thing anyone does to a new
# check, mutating the constant it is about and expecting a failure.
CXXFLAGS  = -std=gnu++14 -Wall -Wextra -O1 -g -MMD -MP
BUILDDIR  = build

SRC = ../support/minimig/minimig_savestate.cpp \
      ../support/minimig/minimig_a2065_crc32.cpp \
      ../support/minimig/minimig_profile.cpp \
      ../support/minimig/akiko_pace.cpp \
      ../support/minimig/akiko_framing.cpp \
      ../support/minimig/akiko_discfmt.cpp \
      ../support/minimig/rom_crypt.cpp \
      ../support/minimig/mgl_rbf.cpp \
      ../support/minimig/mgl_cfg.cpp \
      ../support/minimig/cd_savename.cpp \
      ../support/minimig/vadjust_info.cpp \
      ../support/minimig/mgl_path.cpp \
      ../osd_rowguard.cpp

TESTS = test_savestate_format test_amigacd_profile test_akiko_pace test_rom_crypt \
        test_akiko_framing \
        replay_akiko_log \
        test_akiko_discfmt \
        test_mgl_rbf test_mgl_cfg test_cd_savename test_vadjust_info \
        test_osd_rowguard \
        test_dms test_mgl_path test_minimig_status

# test_dms #includes support/minimig/dms.cpp rather than linking it -- its
# encoders need the decoder's file-local window model and tables -- so dms.cpp
# is deliberately NOT in SRC above. Adding it there would define dms_unpack
# twice in that one binary.

# Shell tests, run before the compiled ones because they need no compiler and
# because of what they cover: prune_list() in scripts/deploy-core.sh is the only
# code in this repository that DELETES files, on the user's SD card, over ssh,
# without asking. It lives here rather than in a CI step of its own so that
# `make -C tests` covers it too -- a delete rule that is only checked on a push
# is checked after the push that broke it.
SHELL_TESTS = test_deploy_core_prune.sh test_deploy_menu_check.sh test_release_notes.sh

.PHONY: all clean shell-tests compiled sanitize
all: shell-tests compiled

compiled: $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(BUILDDIR)/$$t || exit 1; done

# The compiled tests again under AddressSanitizer and UndefinedBehaviorSanitizer,
# in a build directory of their own. Some of what these tests guard is
# undefined behaviour that glibc happens to get right -- mgl_rbf.cpp's memmove
# of a tail that overlaps itself passes as memcpy too, and no ordinary check can
# tell (docs/state-todo.md U5) -- and ASan's memcpy interceptor reports the
# overlap itself. -fno-sanitize-recover makes every finding fatal, so a report
# cannot scroll past in a green run.
SAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
sanitize:
	@$(MAKE) --no-print-directory BUILDDIR=build-san CXXFLAGS="$(CXXFLAGS) $(SAN_FLAGS)" compiled

shell-tests:
	@for t in $(SHELL_TESTS); do echo "== $$t"; bash $$t || exit 1; done

$(BUILDDIR)/%: %.cpp $(SRC)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

-include $(addprefix $(BUILDDIR)/,$(addsuffix .d,$(TESTS)))

clean:
	rm -rf $(BUILDDIR) build-san
