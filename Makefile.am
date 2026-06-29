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
    .clang-format \
    .gitattributes

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
