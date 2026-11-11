CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -pthread -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS := -pthread

# ── OpenSSL ───────────────────────────────────────────────────────────────
# Use pkg-config when available; fall back to brew --prefix openssl@3 on macOS.
UNAME := $(shell uname)
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)
ifeq ($(OPENSSL_LIBS),)
  ifeq ($(UNAME), Darwin)
    OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || echo /usr/local)
    OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
    OPENSSL_LIBS   := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  else
    OPENSSL_LIBS   := -lssl -lcrypto
  endif
endif
CFLAGS  += $(OPENSSL_CFLAGS)
LDFLAGS += $(OPENSSL_LIBS)

SRCDIR  := src
OBJDIR  := obj
BINDIR  := bin

SERVER_SRCS := $(SRCDIR)/server.c \
               $(SRCDIR)/accounts.c \
               $(SRCDIR)/wal.c \
               $(SRCDIR)/auth.c \
               $(SRCDIR)/audit.c \
               $(SRCDIR)/threadpool.c \
               $(SRCDIR)/netutil.c \
               $(SRCDIR)/tlsutil.c

CLIENT_SRCS := $(SRCDIR)/client.c \
               $(SRCDIR)/netutil.c \
               $(SRCDIR)/tlsutil.c

SERVER_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SERVER_SRCS))
CLIENT_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(CLIENT_SRCS))

SERVER_BIN  := $(BINDIR)/ledgercore_server
CLIENT_BIN  := $(BINDIR)/ledgercore_client

UNIT_SRCS := tests/unit.c \
             $(SRCDIR)/accounts.c \
             $(SRCDIR)/wal.c \
             $(SRCDIR)/auth.c \
             $(SRCDIR)/audit.c \
             $(SRCDIR)/netutil.c \
             $(SRCDIR)/tlsutil.c

UNIT_BIN  := $(BINDIR)/ledgercore_unit

LOADGEN_SRCS := tests/loadgen.c
LOADGEN_BIN  := $(BINDIR)/ledgercore_loadgen
CRASH_BIN    := $(BINDIR)/ledgercore_crash_test
STRESS_BIN   := $(BINDIR)/ledgercore_stress

DEFAULT_PORT := 18080
TLS_CERT     := ledgercore.crt
TLS_KEY      := ledgercore.key

.PHONY: all server client clean tsan asan unit check loadgen bench crash-test stress \
        gen-cert tls-smoke

all: server client

server: $(SERVER_BIN)

client: $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

# ── sanitizer builds ──────────────────────────────────────────────────────

# ThreadSanitizer: detects data races and lock-order violations.
tsan: | $(BINDIR)
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=thread \
	    -I$(SRCDIR) $(SERVER_SRCS) -o $(BINDIR)/ledgercore_server_tsan $(LDFLAGS) \
	    -fsanitize=thread
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=thread \
	    -I$(SRCDIR) $(CLIENT_SRCS) -o $(BINDIR)/ledgercore_client_tsan $(LDFLAGS) \
	    -fsanitize=thread

# AddressSanitizer + UBSan: detects memory errors and undefined behaviour.
asan: | $(BINDIR)
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=address,undefined \
	    -I$(SRCDIR) $(SERVER_SRCS) -o $(BINDIR)/ledgercore_server_asan $(LDFLAGS)
	$(CC) -Wall -Wextra -Werror -pthread -O1 -g -std=c11 -D_GNU_SOURCE \
	    $(OPENSSL_CFLAGS) -fsanitize=address,undefined \
	    -I$(SRCDIR) $(CLIENT_SRCS) -o $(BINDIR)/ledgercore_client_asan $(LDFLAGS)

unit: $(UNIT_BIN)

$(UNIT_BIN): $(UNIT_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) $(UNIT_SRCS) -o $@ $(LDFLAGS)

loadgen: $(LOADGEN_BIN)

crash-test: $(CRASH_BIN)

$(CRASH_BIN): tests/crash_test.c | $(BINDIR)
	$(CC) $(CFLAGS) tests/crash_test.c -o $@ $(LDFLAGS)

stress: $(STRESS_BIN)

$(STRESS_BIN): tests/stress.c | $(BINDIR)
	$(CC) $(CFLAGS) tests/stress.c -o $@ $(LDFLAGS)

$(LOADGEN_BIN): $(LOADGEN_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) $(LOADGEN_SRCS) -o $@ $(LDFLAGS)

# bench: run against an isolated data directory and stop only this server.
bench: server loadgen
	@set -e; work_dir=$$(mktemp -d); server_bin="$$(pwd)/$(SERVER_BIN)"; \
	  (cd "$$work_dir" && "$$server_bin" $(DEFAULT_PORT)) & server_pid=$$!; \
	  trap 'kill "$$server_pid" 2>/dev/null || true; wait "$$server_pid" 2>/dev/null || true; rm -rf "$$work_dir"' EXIT; \
	  sleep 0.8; \
	  $(LOADGEN_BIN) -h 127.0.0.1 -p $(DEFAULT_PORT) -t 12 -c 16 -w 2 -m -L

# gen-cert: generate a self-signed dev certificate (valid 10 years)
gen-cert:
	bash gen_cert.sh

# tls-smoke: run the TLS server in an isolated data directory.
tls-smoke: server gen-cert
	@set -e; work_dir=$$(mktemp -d); server_bin="$$(pwd)/$(SERVER_BIN)"; \
	  cert="$$(pwd)/$(TLS_CERT)"; key="$$(pwd)/$(TLS_KEY)"; \
	  (cd "$$work_dir" && "$$server_bin" $(DEFAULT_PORT) "$$cert" "$$key") & server_pid=$$!; \
	  trap 'kill "$$server_pid" 2>/dev/null || true; wait "$$server_pid" 2>/dev/null || true; rm -rf "$$work_dir"' EXIT; \
	  sleep 0.8; \
	  bash tests/tls_smoke.sh $(DEFAULT_PORT) $(TLS_CERT)

check: unit
	@echo "--- unit tests ---"
	./$(UNIT_BIN)
	@echo "--- concurrency test ---"
	bash test_concurrency.sh
	@echo "--- 20-account transfer stress ---"
	bash tests/stress.sh
	@echo "--- crash recovery test ---"
	bash tests/crash_test.sh

clean:
	rm -rf $(OBJDIR) $(BINDIR) \
	    ledgercore.snap ledgercore.snap.tmp \
	    ledgercore.wal \
	    ledgercore_audit.log ledgercore.lock \
	    ledgercore.crt ledgercore.key \
	    /tmp/ledgercore_unit.snap /tmp/ledgercore_unit.wal
