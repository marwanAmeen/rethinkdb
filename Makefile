# Copyright 2010-2013 RethinkDB, all rights reserved.

# Build instructions for rethinkdb are available on the rethinkdb website:
# http://www.rethinkdb.com/docs/build/

# There is additional information about the build system on the wiki:
# https://github.com/rethinkdb/rethinkdb/wiki/Build-System

# This Makefile sets up the environment before delegating to mk/main.mk

# To be able to make from a subdirectory, a Makefile with
# two lines is required:
#   TOP := <relative path to the top of the rethinkdb source tree>
#   include $(TOP)/Makefile
# local.mk includes a rule to build such a Makefile

ifeq (,$(filter else-if,$(.FEATURES)))
	$(error GNU Make >= 3.8.1 is required)
endif

# $(TOP) is the root of the rethinkdb source tree
TOP ?= .

ifeq (/,$(firstword $(subst /,/ ,$(TOP))))
  # if $(TOP) is absolute, make $(CWD) absolute
  CWD := $(shell pwd)
else
  # if $(TOP) is relative, $(CWD) is $(TOP) followed by the relative path from $(TOP) to the working directory
  CWD_ABSPATH := $(shell pwd)
  ROOT_ABSPATH := $(abspath $(CWD_ABSPATH)/$(TOP))
  CWD := $(patsubst $(ROOT_ABSPATH)%,$(patsubst %/,%,$(TOP))%,$(CWD_ABSPATH))
endif

# Prefix $(CWD) to $1 and collapse unecessary ../'s
fixpath = $(patsubst ./%,%,$(shell echo $(CWD)/$1 | sed 's|[^/]\+/\.\./||'))

MAKECMDGOALS ?=

# Build the make command line
MAKE_CMD_LINE = $(MAKE) -f $(TOP)/mk/main.mk
MAKE_CMD_LINE += --no-print-directory
MAKE_CMD_LINE += --warn-undefined-variables
MAKE_CMD_LINE += --no-builtin-rules
MAKE_CMD_LINE += --no-builtin-variables
MAKE_CMD_LINE += TOP=$(TOP) CWD=$(CWD)

# Makefiles can override the goals by setting OVERRIDE_GOALS=<goal>=<replacement>
OVERRIDE_GOALS ?=
NEW_MAKECMDGOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),default-goal)
comma := ,
$(foreach _, $(OVERRIDE_GOALS), $(eval NEW_MAKECMDGOALS := $$(patsubst $(subst =,$(comma),$_), $$(NEW_MAKECMDGOALS))))

# Call fixpath on all goals that aren't phony
MAKE_GOALS = $(foreach goal,$(filter-out $(PHONY_LIST),$(NEW_MAKECMDGOALS)),$(call fixpath,$(goal))) $(filter $(PHONY_LIST),$(NEW_MAKECMDGOALS))

# Delegate the build to mk/main.mk
.PHONY: make
make:
	@$(CHECK_ARG_VARIABLES)
	+@$(MAKE_CMD_LINE) COUNTDOWN_TOTAL=$(COUNTDOWN_TOTAL) $(MAKE_GOALS)

.PHONY: command-line
command-line:
	@echo $(MAKE_CMD_LINE)

%: make
	@true

# List all rules
.PHONY: dump-db
dump-db:
	+@$(CHECK_ARG_VARIABLES)
	+@$(MAKE_CMD_LINE) --print-data-base --question JUST_SCAN_MAKEFILES=1 || true

# Load the configuration
include $(TOP)/mk/configure.mk

# Require CHECK_ARG_VARIABLES
include $(TOP)/mk/check-env.mk

# Require pipe-stderr
include $(TOP)/mk/pipe-stderr.mk

# The cached list of phony targets
-include $(TOP)/mk/gen/phony-list.mk

.PHONY: debug-count
debug-count:
	@$(eval MAKE_GOALS := $(filter-out $@,$(MAKE_GOALS)))$(COUNTDOWN_COMMAND)

COUNTDOWN_COMMAND = MAKEFLAGS='$(MAKEFLAGS)' $(MAKE_CMD_LINE) $(MAKE_GOALS) --dry-run JUST_SCAN_MAKEFILES=1 -j1
ifeq (1,$(SHOW_COUNTDOWN))
  # See mk/lib.mk for JUST_SCAN_MAKEFILES
  COUNTDOWN_TOTAL = $(firstword $(shell $(COUNTDOWN_COMMAND) 2>&1 | grep "[!!!]" | wc -l 2>/dev/null))
else
  COUNTDOWN_TOTAL :=
endif

# Build the list of phony targets
$(TOP)/mk/gen/phony-list.mk: $(CONFIG)
	+@MAKEFLAGS= $(MAKE_CMD_LINE) --print-data-base var-MAKEFILE_LIST JUST_SCAN_MAKEFILES=1 \
	  | egrep '^.PHONY: |MAKEFILE_LIST = ' \
	  | sed 's/^.PHONY:/PHONY_LIST :=/' \
	  | sed 's|^MAKEFILE_LIST = \(.*\)$$|$$(TOP)/mk/gen/phony-list.mk: $$(patsubst $(TOP)/%,$$(TOP)/%,$$(filter-out %.d,\1))|' \
	  > $@

# Don't try to rebuild any of the Makefiles
Makefile:
	@true

%/Makefile:
	@true

%.mk:
	@true

##### Cancel builtin rules

.SUFFIXES:

%: %,v
%: RCS/%,v
%: RCS/%
%: s.%
%: SCCS/s.%
