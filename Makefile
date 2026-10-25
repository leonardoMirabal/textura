# SPDX-License-Identifier: GPL-3.0-only
# Textura: host tools for the engines (wave 0). Everything generated goes under build/, tools under .local/.
#
#   make tx-test       the checks (txtest with UBSan, no-float source scan, no libcalls on a 32-bit target)
#   make tx-render     every engine x every standard scene -> build/wav/   (one: make tx-render ENGINE=test SCENE=tap)
#                      (through the CHAMBER: build/bench/txrender ENGINE SCENE -c -> build/wav/ENGINE-SCENE-chamber.wav)
#   make tx-bench      CPU per block + the FM-1 estimate (runs tx-calib first; CALIB=0 skips the calibration;
#                      REPEAT=3: each scene measured 3 times, the median kept: no flakes from a slow host moment;
#                      timing jobs wait for the shared HEAVY lock, tools/lock.py; LOCK=0 skips it)
#   make tx-calib      Frodhes' worst scene timed on this host, its code pinned at FRODHES_REV (read from ../frodhes'
#                      git, never changed; FRODHES=path to change)
#   make tx-budget     the CPU budget of the whole audio interrupt on the FM-1: the platform's costs (Frodhes'
#                      measurements), the mix / CHAMBER, the effects reserve, the sources' pool, every SOURCE preset's
#                      cost curve and the mixes (build/bench/budget.txt, budget.csv): a report, nothing changed
#   make tx-activity   each landscape sound as a player lane (the generator on) at ACTIVITY 0 / 9 / 64 / 118 / 127:
#                      events/s, LUFS-S, gaps, a cloud at the top or not (ARGS="-s SECS -n SEEDS -f NAME")
#   make tx-grain      a granular-delay prototype's cost on the FM-1 and its buffer formats' noise (costing only)
#   make tx-player     the browser player -> build/audition/ (Emscripten from .local/emsdk)
#   make tx-player-check   the player in headless Chromium (Playwright in .local/venv, browsers in .local/playwright)
#   make tx-ab         the player's A/B data: ours vs the local reference recordings (.local/reference), loudness-
#                      matched, with spectrograms -> build/audition/ab/ (local only, never committed)
#   make tx-baseline   rewrite bench/baselines/renders.sha256 (after a reviewed sound change)
#   make tx-gen        the lane generator's long run: 30 min (MIN=...) of the water lane at EVOLVE 2 h, fast, with its
#                      checks (no stuck state, no runaway) -> build/gen/longrun.csv and a plot build/gen/longrun.png
#   make tx-tables     regenerate engines/tx_tables.[ch]
#   make tx-fx-tables  regenerate engines/tx_fx_tables.[ch] (the effects' tables)
#   make tx-drive-tables   regenerate engines/tx_drive_tables.h (the DRIVE TYPEs' tables, from tools/drive_gen/: numpy)
#   make tx-size       code size of each engines/ object (host arm64 -Os, a rough proxy for the device)
#   make fw-host       the firmware on this computer (plans: UI-PLAN): build/fw/textura-host, a fake FM-1
#                      (tests/host.c): build/fw/textura-host OUT [SCRIPT [FPS]] -> OUT.wav, OUT-<ms>.ppm
#   make fw-test       the firmware's checks: tests/lanes_test.c (the lanes = the player's chain, sample for sample)
#                      and every host script in tests/scripts/ (its expects)
#   make fw-fonts      regenerate firmware/gen/felucca_font.h (every ASCII glyph of Terminus 8x16) with Frodhes'
#                      tools/gen_font.py at FW_FONT_REV, read from its git (Frodhes' checkout is never changed)
CC       ?= cc
FRODHES  ?= ../frodhes
CALIB    ?= 1
REPEAT   ?= 1
CALIB_RUNS ?= 9
# the timing jobs (tx-calib, tx-bench, the player check's CPU meter) wait for the shared HEAVY lock (tools/lock.py, in
# the main checkout's build/; a copy without git: TX_LOCK_ROOT=<main checkout>), so two agents never time at once.
# LOCK=0 runs them without it. Nested calls inherit the holder's token (TX_LOCK_HEAVY) and go on at once.
LOCK     ?= 1
HEAVY     = $(if $(filter 1,$(LOCK)),python3 -I tools/lock.py run HEAVY --why "make $@" --wait 3600 --,)
B        := build/bench
CSTD     := -std=c99 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Wall -Wextra -Iengines -Ibench
ENG_SRC  := $(wildcard engines/*.c)
ENG_HDR  := $(wildcard engines/*.h)
HOST_SRC := bench/tx_host.c bench/tx_script.c
HOST_HDR := bench/tx_host.h bench/tx_script.h
UBSAN    := -fsanitize=undefined -fno-sanitize-recover=undefined

.PHONY: fw-host fw-test fw-fonts tx-drive-tables tx-gen tx-test tx-render tx-bench _tx-bench-run tx-budget _tx-budget-run tx-activity tx-grain tx-calib _tx-calib-run tx-player tx-player-check tx-ab tx-baseline tx-tables tx-fx-tables tx-size tx-libcalls tx-clean

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

$(B)/gen_fx_tables: bench/gen_fx_tables.c
	@mkdir -p $(B)
	$(CC) -O2 -Wall -o $@ $< -lm

tx-fx-tables: $(B)/gen_fx_tables
	$(B)/gen_fx_tables engines/tx_fx_tables.h engines/tx_fx_tables.c

tx-drive-tables:
	.local/venv/bin/python tools/gen_drive_tables.py

tx-test: $(B)/txtest tx-libcalls
	$(B)/txtest

tx-baseline: $(B)/txtest
	$(B)/txtest --update

tx-render: $(B)/txrender
	@if [ -n "$(ENGINE)" ]; then $(B)/txrender $(ENGINE) $(or $(SCENE),max); else $(B)/txrender all; fi

# ---- the firmware (UI-PLAN): firmware/src/textura.c, one compilation unit with Dinghy's platform (platform.c and
# tests/host.c, from Frodhes v0.9.5, unchanged); the engines link beside it as their own objects
FW        := build/fw
FW_DEPS   := $(wildcard firmware/src/*.c firmware/src/*.h firmware/gen/*.h firmware/hal/*.h) tests/host.c
CFLAGS_FW := -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER='"dev"' -Ifirmware/src -Ifirmware/gen -Iengines
FW_FONT_REV ?= v0.9.5

$(FW)/textura-host: $(FW_DEPS) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(FW)
	$(CC) $(CFLAGS_FW) -o $@ firmware/src/textura.c $(ENG_SRC) -lm

$(FW)/lanes_test: tests/lanes_test.c firmware/src/lanes.c $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(FW)
	$(CC) $(CSTD) -Wno-unused-function -Ifirmware/src -O1 -g $(UBSAN) -o $@ tests/lanes_test.c $(ENG_SRC) -lm

fw-host: $(FW)/textura-host

fw-test: $(FW)/lanes_test $(FW)/textura-host
	@$(FW)/lanes_test || exit 1; \
	fail=0; for s in tests/scripts/*.txt; do n=$$(basename $$s .txt); \
	  if $(FW)/textura-host $(FW)/$$n $$s > $(FW)/$$n.log 2>&1; then echo "ok    host $$n"; \
	  else echo "FAIL  host $$n ($(FW)/$$n.log)"; fail=1; fi; done; exit $$fail

fw-fonts:
	@rm -rf $(FW)/fonts && mkdir -p $(FW)/fonts
	git -C $(FRODHES) archive $(FW_FONT_REV) tools/gen_font.py assets/fonts/ter-u16n.bdf | tar -x -C $(FW)/fonts
	python3 -I $(FW)/fonts/tools/gen_font.py --full firmware/gen/felucca_font.h

# engines/ (and the firmware's lanes.c) compiled for a 32-bit soft-float target: any call to a 64-bit division or a float helper fails the check
# (the device has no libgcc); 64-bit multiply instructions are counted for information (inner loops should have none)
tx-libcalls:
	@mkdir -p build/arm
	@if ! echo 'int x;' | $(CC) --target=armv7m-none-eabi -ffreestanding -x c -S -o /dev/null - 2>/dev/null; then \
	  echo "skip  libcalls: $(CC) has no 32-bit ARM target"; exit 0; fi; \
	fail=0; for f in $(ENG_SRC) firmware/src/lanes.c; do o=build/arm/$$(basename $$f .c).s; \
	  $(CC) --target=armv7m-none-eabi -mfloat-abi=soft -ffreestanding -O2 -Iengines -Wno-unused-function -S -o $$o $$f || exit 1; \
	  bad=$$(grep -E 'bl[[:space:]]+(__aeabi_(l|ul)divmod|__aeabi_[fd][a-z]+|__aeabi_[iul]+2[fd]|__aeabi_[fd]2|__(u)?(div|mod)di3|__.*(sf|df)[0-9]?)' $$o | sort -u); \
	  m64=$$(grep -cE '^[[:space:]]+(smull|umull|smlal|umlal)' $$o); \
	  if [ -n "$$bad" ]; then echo "FAIL  libcalls $$f: $$bad"; fail=1; else echo "ok    libcalls $$f (64-bit multiplies: $$m64)"; fi; \
	done; exit $$fail

# ---- calibration: Frodhes' own sources at FRODHES_REV, read from its git (git archive: Frodhes' checkout is never
# changed) into build/calib/fr, host.c's audio call rewritten to a timed one (bench/calib/frcal_hook.h), built as its
# Makefile's host build does, run CALIB_RUNS times. Pinned: FM1_WORST_US (3620 us, bench/txbench.c) was measured on
# the FM-1 with exactly this revision's code (9 voices); a later Frodhes (0.9.5: 10 voices, other effects and CCs)
# runs another scene, so its host time would no longer match that figure. Change both together, never one.
FRODHES_REV ?= e68d1a7
build/calib/frodhes: bench/calib/frcal_hook.h build/calib/rev-$(FRODHES_REV)
	@rm -rf build/calib/fr && mkdir -p build/calib/fr
	git -C $(FRODHES) archive --format=tar $(FRODHES_REV) firmware/src firmware/gen tests/host.c | tar -x -C build/calib/fr
	@mv build/calib/fr/tests/host.c build/calib/fr/tests/host.orig
	sed 's/app_audio(blk, HALF_FRAMES);/FRCAL_AUDIO(blk, HALF_FRAMES);/' build/calib/fr/tests/host.orig > build/calib/fr/tests/host.c && rm build/calib/fr/tests/host.orig
	@grep -q FRCAL_AUDIO build/calib/fr/tests/host.c || { echo "host.c's app_audio call not found: calibration hook not applied"; exit 1; }
	git -C $(FRODHES) log -1 --format='%h %ci' $(FRODHES_REV) > build/calib/frodhes.rev
	$(CC) -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER='"dev"' -include bench/calib/frcal_hook.h \
	  -Ibuild/calib/fr/firmware/src -Ibuild/calib/fr/firmware/gen -o $@ build/calib/fr/firmware/src/frodhes.c -lm

# a stamp per pinned revision: changing FRODHES_REV rebuilds the calibration binary
build/calib/rev-$(FRODHES_REV):
	@git -C $(FRODHES) cat-file -e '$(FRODHES_REV)^{commit}' 2>/dev/null || \
	  { echo "no commit $(FRODHES_REV) in $(FRODHES) (FRODHES=path to Frodhes' checkout)"; exit 1; }
	@mkdir -p build/calib && rm -f build/calib/rev-* && touch $@

tx-calib: build/calib/frodhes
	@$(HEAVY) $(MAKE) -s _tx-calib-run

_tx-calib-run:
	@rm -rf build/calib/run && mkdir -p build/calib/run
	@i=1; while [ $$i -le $(CALIB_RUNS) ]; do \
	  (cd build/calib/run && FRCAL_OUT=w$$i.txt ../frodhes out ../../../bench/calib/frodhes_worst.txt > log$$i.txt) || exit 1; \
	  i=$$((i + 1)); done; echo "tx-calib: $(CALIB_RUNS) runs of Frodhes $$(cat build/calib/frodhes.rev) in build/calib/run"

tx-bench: $(B)/txbench
	@$(HEAVY) $(MAKE) -s _tx-bench-run

_tx-bench-run:
	@if [ "$(CALIB)" = 1 ]; then $(MAKE) -s tx-calib || echo "calibration failed: host times only"; fi
	$(B)/txbench --repeat $(REPEAT) | tee $(B)/bench.txt

$(B)/txbudget: bench/txbudget.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txbudget.c $(HOST_SRC) $(ENG_SRC) -lm

tx-budget: $(B)/txbudget build/calib/frodhes
	@$(HEAVY) $(MAKE) -s _tx-budget-run

_tx-budget-run:
	@[ -f build/calib/run/w1.txt ] || $(MAKE) -s _tx-calib-run
	$(B)/txbudget | tee $(B)/budget.txt

# a granular-delay prototype, for costing only (.private): FM-1 us per block per option, the buffer
# formats' noise
$(B)/txgrain: bench/txgrain.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txgrain.c $(HOST_SRC) $(ENG_SRC) -lm

tx-grain: $(B)/txgrain build/calib/frodhes
	@[ -f build/calib/run/w1.txt ] || $(MAKE) -s tx-calib
	@$(HEAVY) $(B)/txgrain

# the lane fader's range per sound, as the player drives a lane (the generator on): events/s, short-term loudness,
# gaps, whether the top reads as a cloud (bench/txactivity.c; ARGS="-s 30 -n 3 -f name" ...)
$(B)/txactivity: bench/txactivity.c $(HOST_SRC) $(HOST_HDR) $(ENG_SRC) $(ENG_HDR)
	@mkdir -p $(B)
	$(CC) $(CSTD) -O2 -o $@ bench/txactivity.c $(HOST_SRC) $(ENG_SRC) -lm

tx-activity: $(B)/txactivity build/calib/frodhes
	@[ -f build/calib/run/w1.txt ] || $(MAKE) -s tx-calib
	@$(HEAVY) $(B)/txactivity $(ARGS) | tee $(B)/activity.txt

tx-player:
	sh audition/build.sh

tx-player-check:
	@[ -f $(B)/calib.json ] || $(MAKE) -s tx-bench   # the check reads the FM-1 calibration (check_player.py)
	$(MAKE) -s tx-player
	$(HEAVY) env PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python audition/check_player.py

tx-ab: $(B)/txrender
	.local/venv/bin/python -I audition/ab_prepare.py

tx-size:
	@mkdir -p build/size; for f in $(ENG_SRC); do $(CC) -Os -Iengines -c -o build/size/$$(basename $$f .c).o $$f; done; \
	size build/size/*.o

tx-clean:
	rm -rf build/bench build/wav build/calib build/arm build/size build/audition build/audition-check build/fw
