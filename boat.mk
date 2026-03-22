# BoAT v4 — GNU Make includable fragment
# Usage: include path/to/BoAT4/boat.mk
# Then add $(BOAT_SRCS) to your source list, $(BOAT_INCS) to -I flags,
# and $(BOAT_CFLAGS) to CFLAGS.

BOAT4_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

BOAT_INCS := \
    -I$(BOAT4_DIR)include \
    -I$(BOAT4_DIR)third-party/crypto
ifndef BOAT_VENDOR_CJSON
BOAT_INCS += -I$(BOAT4_DIR)third-party/cJSON
endif

BOAT_CFLAGS :=

# Core (always included)
BOAT_SRCS := \
    $(BOAT4_DIR)src/core/boat_util.c \
    $(BOAT4_DIR)src/core/boat_key.c \
    $(BOAT4_DIR)src/core/boat_key_soft.c \
    $(BOAT4_DIR)src/core/boat_rpc.c
ifndef BOAT_VENDOR_CJSON
BOAT_SRCS += $(BOAT4_DIR)third-party/cJSON/cJSON.c
endif

# EVM
ifdef BOAT_EVM
BOAT_CFLAGS += -DBOAT_EVM_ENABLED=1
BOAT_SRCS += \
    $(BOAT4_DIR)src/evm/evm_rpc.c \
    $(BOAT4_DIR)src/evm/evm_tx.c \
    $(BOAT4_DIR)src/evm/evm_abi.c
else
BOAT_CFLAGS += -DBOAT_EVM_ENABLED=0
endif

# Solana
ifdef BOAT_SOL
BOAT_CFLAGS += -DBOAT_SOL_ENABLED=1
BOAT_SRCS += \
    $(BOAT4_DIR)src/sol/sol_rpc.c \
    $(BOAT4_DIR)src/sol/sol_tx.c \
    $(BOAT4_DIR)src/sol/sol_spl.c \
    $(BOAT4_DIR)src/sol/sol_ix.c \
    $(BOAT4_DIR)src/sol/sol_borsh.c
else
BOAT_CFLAGS += -DBOAT_SOL_ENABLED=0
endif

# Payment protocols (require EVM)
ifdef BOAT_PAY_X402
BOAT_CFLAGS += -DBOAT_PAY_X402_ENABLED=1
BOAT_SRCS += $(BOAT4_DIR)src/pay/pay_common.c $(BOAT4_DIR)src/pay/pay_x402.c
endif

ifdef BOAT_PAY_NANO
BOAT_CFLAGS += -DBOAT_PAY_NANO_ENABLED=1
BOAT_SRCS += $(BOAT4_DIR)src/pay/pay_common.c $(BOAT4_DIR)src/pay/pay_nano.c
endif

ifdef BOAT_PAY_GATEWAY
BOAT_CFLAGS += -DBOAT_PAY_GATEWAY_ENABLED=1
BOAT_SRCS += $(BOAT4_DIR)src/pay/pay_common.c $(BOAT4_DIR)src/pay/pay_gateway.c
ifdef BOAT_SOL
BOAT_SRCS += $(BOAT4_DIR)src/pay/pay_gateway_sol.c
ifdef BOAT_EVM
BOAT_SRCS += $(BOAT4_DIR)src/pay/pay_gateway_cross.c
endif
endif
endif

# Deduplicate pay_common.c if multiple payment modules enabled
BOAT_SRCS := $(sort $(BOAT_SRCS))

# PAL
ifdef BOAT_PAL_LINUX
BOAT_SRCS += $(BOAT4_DIR)src/pal/linux/pal_linux.c
endif

# Crypto (trezor-crypto)
BOAT_SRCS += $(wildcard $(BOAT4_DIR)third-party/crypto/*.c)
