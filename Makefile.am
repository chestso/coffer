# Top-level Makefile.am for coffer

SUBDIRS = src include tests contrib

ACLOCAL_AMFLAGS = -I m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = coffer.pc

EXTRA_DIST = \
    autogen.sh \
    build-aux/git-version.sh \
    version \
    README.md \
    COPYING \
    FOLLOWUPS.md \
    .clang-format

# Keep the version file in sync with the current git state (or COFFER_VERSION
# from configure).  The recipe always runs but only rewrites the file when
# the version actually changes, so a subsequent autoreconf picks up the right
# PACKAGE_VERSION without needing to run autogen.sh first.
.PHONY: version_force
version: version_force
	@v=`"$(srcdir)/build-aux/git-version.sh" "$(srcdir)" 2>/dev/null`; \
	test -n "$$v" || v="$(COFFER_VERSION)"; \
	test -n "$$v" || v="unknown"; \
	printf '%s' "$$v" > "$(srcdir)/version.tmp"; \
	if cmp -s "$(srcdir)/version.tmp" "$(srcdir)/version" 2>/dev/null; then \
	  rm -f "$(srcdir)/version.tmp"; \
	else \
	  mv "$(srcdir)/version.tmp" "$(srcdir)/version"; \
	  echo "  GEN      $@ ($$v)"; \
	fi
version_force:

all-local: version

# Stamp the version file in the dist tarball so tarball builds produce the
# correct version without .git.
dist-hook:
	printf '%s' '$(COFFER_VERSION)' > "$(distdir)/version"

# Format target — runs clang-format on C/C++ and prettier on Markdown.
# Requires the tools to be installed; logs a warning if they are not.
.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
	    echo "Formatting C/C++ with clang-format..."; \
	    find $(top_srcdir)/src $(top_srcdir)/include $(top_srcdir)/tests $(top_srcdir)/contrib \
	        \( -name '*.c' -o -name '*.h' \) -print0 \
	        | xargs -0 clang-format -i; \
	else \
	    echo "clang-format not found - skipping C/C++ formatting." >&2; \
	fi
	@if command -v prettier >/dev/null 2>&1; then \
	    echo "Formatting Markdown with prettier..."; \
	    find $(top_srcdir) -maxdepth 2 -name '*.md' -print0 \
	        | xargs -0 prettier --write; \
	else \
	    echo "prettier not found - skipping Markdown formatting." >&2; \
	fi

# Bear target — produce compile_commands.json for clangd by intercepting
# the build. Cleans first so bear sees every compile invocation.
.PHONY: bear
bear:
	@command -v bear >/dev/null 2>&1 || { \
	    echo "bear not installed - install via your package manager." >&2; \
	    exit 1; \
	}
	$(MAKE) clean
	bear -- $(MAKE)
