CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3
JQ ?= jq
BUILD_DIR ?= build/core
SANITIZE_BUILD_DIR ?= build/core-sanitize
BUILD_JOBS ?= 4
CMAKE_FLAGS ?= -DSHIN_CONTEXT_BUILD_SOURCE_DISCOVERY=ON -DSHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER=ON
TOOL = $(abspath $(BUILD_DIR))/shin-context
REPORT_DIR ?= $(abspath $(BUILD_DIR))/reports
PROFILE_DIR := $(CURDIR)/profiles
# Choose an external directory explicitly; this must not be a managed index.
CONTEXT_OUT ?=
CODEX_HOME ?= $(HOME)/.codex
HERMES_HOME ?= $(HOME)/.hermes
HERMES ?= hermes

.PHONY: all core check test core-check cmake-check safety core-safety cmake-sanitize-check \
	tidy static-analysis-check universality-check release-contract-check portable-install-check \
	core-release-check release-check docs-kb docs-check docs-doctor docs-eval docs-pack-eval \
	docs-validate skill-check hermes-integration-check install-codex-skill install-hermes-integration \
	context-output-check core-perf core-perf-check perf perf-check version

all: core

core:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release $(CMAKE_FLAGS)
	$(CMAKE) --build "$(BUILD_DIR)" --parallel "$(BUILD_JOBS)"

check: core
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure

test core-check cmake-check: check

safety:
	$(CMAKE) -S . -B "$(SANITIZE_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug -DSHIN_ENABLE_SANITIZERS=ON $(CMAKE_FLAGS)
	$(CMAKE) --build "$(SANITIZE_BUILD_DIR)" --parallel "$(BUILD_JOBS)"
	$(CTEST) --test-dir "$(SANITIZE_BUILD_DIR)" --output-on-failure

core-safety cmake-sanitize-check: safety

tidy: core
	$(CMAKE) --build "$(BUILD_DIR)" --target shin-clang-tidy --parallel "$(BUILD_JOBS)"

static-analysis-check: tidy

context-output-check:
	@test -n "$(CONTEXT_OUT)" || { echo 'Set CONTEXT_OUT to a new, external, unmanaged output directory'; exit 2; }
	@$(PYTHON) -c 'from pathlib import Path; import sys; p=Path(sys.argv[1]).resolve(); r=Path.cwd().resolve(); sys.exit("CONTEXT_OUT must be disjoint from the source tree") if p == r or r in p.parents or p in r.parents else None' "$(CONTEXT_OUT)"

docs-kb: core context-output-check
	"$(TOOL)" --root "$(CURDIR)" --profile shin --profile-dir "$(PROFILE_DIR)" --out-dir "$(CONTEXT_OUT)" build

docs-check docs-doctor docs-eval docs-pack-eval docs-validate: core context-output-check
	"$(TOOL)" --root "$(CURDIR)" --profile shin --profile-dir "$(PROFILE_DIR)" --out-dir "$(CONTEXT_OUT)" $(patsubst docs-%,%,$@) $(if $(filter docs-check,$@),,--strict)

docs-validate: docs-kb

universality-check:
	@mkdir -p "$(REPORT_DIR)"
	bash tests/run_universality_tests.sh "$(JQ)" "$(CURDIR)" "$(REPORT_DIR)/universality.json"

release-contract-check: docs-kb
	@mkdir -p "$(REPORT_DIR)"
	bash tests/run_release_contract_tests.sh "$(JQ)" "$(TOOL)" "$(CURDIR)" "$(REPORT_DIR)/release-contract.json" "$(CONTEXT_OUT)/docs/kb/generated/manifest.json"

portable-install-check: check
	@mkdir -p "$(REPORT_DIR)"
	bash tests/run_portable_install_tests.sh "$(CMAKE)" "$(JQ)" "$(CURDIR)" "$(abspath $(BUILD_DIR))" "$(REPORT_DIR)/portable-install.json"

core-perf: docs-kb
	@mkdir -p "$(REPORT_DIR)/perf"
	"$(TOOL)" --root "$(CURDIR)" --profile shin --profile-dir "$(PROFILE_DIR)" --out-dir "$(CONTEXT_OUT)" bench "source facts" --iterations 10 --build-iterations 3 > "$(REPORT_DIR)/perf/shin.json"
	$(JQ) -e '.packEvalCases[] | select(.id == "self-agent-operations-pack") | .request' "$(PROFILE_DIR)/shin.json" > "$(REPORT_DIR)/perf/pack-request.json"
	"$(TOOL)" --root "$(CURDIR)" --profile shin --profile-dir "$(PROFILE_DIR)" --out-dir "$(CONTEXT_OUT)" pack-bench --input "$(REPORT_DIR)/perf/pack-request.json" --projection compact --iterations 10 --warmup 2 --require-current --strict > "$(REPORT_DIR)/perf/agent-context-pack.json"

core-perf-check: core-perf
	bash tests/run_perf_gate.sh "$(JQ)" "docs/machine/shin-context-engine-perf-budgets.json" "$(REPORT_DIR)/perf" "$(REPORT_DIR)/perf-gate.json" core

perf: core-perf
perf-check: core-perf-check

core-release-check: check safety static-analysis-check universality-check release-contract-check docs-validate portable-install-check core-perf-check
release-check: core-release-check

skill-check:
	bash tests/run_skill_contract_tests.sh "$(CURDIR)"

hermes-integration-check:
	PYTHONDONTWRITEBYTECODE=1 "$(PYTHON)" tests/run_hermes_plugin_tests.py "$(CURDIR)"

install-codex-skill: skill-check
	mkdir -p "$(CODEX_HOME)/skills/shin-context"
	cp -R integrations/codex/skills/shin-context/. "$(CODEX_HOME)/skills/shin-context/"

install-hermes-integration: hermes-integration-check
	mkdir -p "$(HERMES_HOME)/plugins/shin-context" "$(HERMES_HOME)/skills/software-development/shin-context"
	cp -R integrations/hermes/plugins/shin-context/. "$(HERMES_HOME)/plugins/shin-context/"
	cp -R integrations/hermes/plugins/shin-context/skills/workflow/. "$(HERMES_HOME)/skills/software-development/shin-context/"
	HERMES_HOME="$(HERMES_HOME)" "$(HERMES)" plugins enable --no-allow-tool-override shin-context

version: core
	"$(TOOL)" --version
