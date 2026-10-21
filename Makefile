# SPDX-License-Identifier: GPL-3.0-only
# Textura: host tools for the engines (wave 0). Everything generated goes under build/, tools under .local/.
#
#   make tx-test       the checks (txtest with UBSan, no-float source scan, no libcalls on a 32-bit target)
#   make tx-render     every engine x every standard scene -> build/wav/   (one: make tx-render ENGINE=test SCENE=tap)
#                      (through the CHAMBER: build/bench/txrender ENGINE SCENE -c -> build/wav/ENGINE-SCENE-chamber.wav)
#   make tx-bench      CPU per block + the FM-1 estimate (runs tx-calib first; CALIB=0 skips the calibration;
#                      REPEAT=3: each scene measured 3 times, the median kept: no flakes from a slow host moment)
#   make tx-calib      Frodhes' worst scene timed on this host (needs ../frodhes, read-only; FRODHES=path to change)
#   make tx-player     the browser player -> build/audition/ (Emscripten from .local/emsdk)
#   make tx-player-check   the player in headless Chromium (Playwright in .local/venv, browsers in .local/playwright)
#   make tx-ab         the player's A/B data: ours vs the local reference recordings (.local/reference), loudness-
#                      matched, with spectrograms -> build/audition/ab/ (local only, never committed)
#   make tx-baseline   rewrite bench/baselines/renders.sha256 (after a reviewed sound change)
#   make tx-gen        the lane generator's long run: 30 min (MIN=...) of the water lane at EVOLVE 2 h, fast, with its
#                      checks (no stuck state, no runaway) -> build/gen/longrun.csv and a plot build/gen/longrun.png
#   make tx-tables     regenerate engines/tx_tables.[ch]
#   make tx-size       code size of each engines/ object (host arm64 -Os, a rough proxy for the device)
CC       ?= cc
FRODHES  ?= ../frodhes
CALIB    ?= 1
REPEAT   ?= 1
CALIB_RUNS ?= 9
B        := build/bench
CSTD     := -std=c99 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra -Iengines -Ibench
ENG_SRC  := $(wildcard engines/*.c)
ENG_HDR  := $(wildcard engines/*.h)
HOST_SRC := bench/tx_host.c bench/tx_script.c
HOST_HDR := bench/tx_host.h bench/tx_script.h
UBSAN    := -fsanitize=undefined -fno-sanitize-recover=undefined

.PHONY: tx-gen tx-test tx-render tx-bench tx-calib tx-player tx-player-check tx-ab tx-baseline tx-tables tx-size tx-libcalls tx-clean

$(B)/txrender: bench/txrender.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txrender.c $(HOST_SRC) $(ENG_SRC) -lm

$(B)/txtest: bench/txtest.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O1 -g $(UBSAN) -o $@ bench/txtest.c $(HOST_SRC) $(ENG_SRC) -lm

$(B)/txbench: bench/txbench.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txbench.c $(HOST_SRC) $(ENG_SRC) -lm

$(B)/txgen: bench/txgen.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txgen.c $(HOST_SRC) $(ENG_SRC) -lm

tx-gen: $(B)/txgen
	$(B)/txgen $(or $(MIN),30) 3 127 -o build/gen/longrun.csv
	.local/venv/bin/python bench/txgen_plot.py build/gen/longrun.csv build/gen/longrun.png

$(B)/gen_tables: bench/gen_tables.c
	@mkdir -p $(B)
	$(CC) -O2 -Wall -o $@ $< -lm

tx-tables: $(B)/gen_tables
	$(B)/gen_tables engines/tx_tables.h engines/tx_tables.c

tx-test: $(B)/txtest tx-libcalls
	$(B)/txtest

tx-baseline: $(B)/txtest
	$(B)/txtest --update

tx-render: $(B)/txrender
	@if [ -n "$(ENGINE)" ]; then $(B)/txrender $(ENGINE) $(or $(SCENE),max); else $(B)/txrender all; fi

# engines/ compiled for a 32-bit soft-float target: any call to a 64-bit division or a float helper fails the check
# (the device has no libgcc); 64-bit multiply instructions are counted for information (inner loops should have none)
tx-libcalls:
	@mkdir -p build/arm
	@if ! echo 'int x;' | $(CC) --target=armv7m-none-eabi -ffreestanding -x c -S -o /dev/null - 2>/dev/null; then \
	  echo "skip  libcalls: $(CC) has no 32-bit ARM target"; exit 0; fi; \
	fail=0; for f in $(ENG_SRC); do o=build/arm/$$(basename $$f .c).s; \
	  $(CC) --target=armv7m-none-eabi -mfloat-abi=soft -ffreestanding -O2 -Iengines -S -o $$o $$f || exit 1; \
	  bad=$$(grep -E 'bl[[:space:]]+(__aeabi_(l|ul)divmod|__aeabi_[fd][a-z]+|__aeabi_[iul]+2[fd]|__aeabi_[fd]2|__(u)?(div|mod)di3|__.*(sf|df)[0-9]?)' $$o | sort -u); \
	  m64=$$(grep -cE '^[[:space:]]+(smull|umull|smlal|umlal)' $$o); \
	  if [ -n "$$bad" ]; then echo "FAIL  libcalls $$f: $$bad"; fail=1; else echo "ok    libcalls $$f (64-bit multiplies: $$m64)"; fi; \
	done; exit $$fail

# ---- calibration: Frodhes' own sources copied into build/calib/fr (never changed in place), host.c's audio call
# rewritten to a timed one (bench/calib/frcal_hook.h), built as its Makefile's host build does, run CALIB_RUNS times
build/calib/frodhes: bench/calib/frcal_hook.h
	@test -f $(FRODHES)/firmware/src/frodhes.c || { echo "no $(FRODHES)/firmware/src/frodhes.c (FRODHES=path)"; exit 1; }
	@rm -rf build/calib/fr && mkdir -p build/calib/fr/firmware build/calib/fr/tests
	cp -R $(FRODHES)/firmware/src $(FRODHES)/firmware/gen build/calib/fr/firmware/
	sed 's/app_audio(blk, HALF_FRAMES);/FRCAL_AUDIO(blk, HALF_FRAMES);/' $(FRODHES)/tests/host.c > build/calib/fr/tests/host.c
	@grep -q FRCAL_AUDIO build/calib/fr/tests/host.c || { echo "host.c's app_audio call not found: calibration hook not applied"; exit 1; }
	@(cd $(FRODHES) && git log -1 --format='%h %ci' 2>/dev/null) > build/calib/frodhes.rev || true
	$(CC) -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER='"dev"' -include bench/calib/frcal_hook.h \
	  -Ibuild/calib/fr/firmware/src -Ibuild/calib/fr/firmware/gen -o $@ build/calib/fr/firmware/src/frodhes.c -lm

tx-calib: build/calib/frodhes
	@rm -rf build/calib/run && mkdir -p build/calib/run
	@i=1; while [ $$i -le $(CALIB_RUNS) ]; do \
	  (cd build/calib/run && FRCAL_OUT=w$$i.txt ../frodhes out ../../../bench/calib/frodhes_worst.txt > log$$i.txt) || exit 1; \
	  i=$$((i + 1)); done; echo "tx-calib: $(CALIB_RUNS) runs of Frodhes $$(cat build/calib/frodhes.rev) in build/calib/run"

tx-bench: $(B)/txbench
	@if [ "$(CALIB)" = 1 ]; then $(MAKE) -s tx-calib || echo "calibration failed: host times only"; fi
	$(B)/txbench --repeat $(REPEAT) | tee $(B)/bench.txt

tx-player:
	sh audition/build.sh

tx-player-check:
	@[ -f $(B)/calib.json ] || $(MAKE) -s tx-bench   # the check reads the FM-1 calibration (check_player.py)
	$(MAKE) -s tx-player
	PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python audition/check_player.py

tx-ab: $(B)/txrender
	.local/venv/bin/python -I audition/ab_prepare.py

tx-size:
	@mkdir -p build/size; for f in $(ENG_SRC); do $(CC) -Os -Iengines -c -o build/size/$$(basename $$f .c).o $$f; done; \
	size build/size/*.o

tx-clean:
	rm -rf build/bench build/wav build/calib build/arm build/size build/audition build/audition-check
