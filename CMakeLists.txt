cmake_minimum_required(VERSION 3.12)
project(BoAT4 C)

set(CMAKE_C_STANDARD 11)

#-----------------------------------------------------------------------------
# Options
#-----------------------------------------------------------------------------
option(BOAT_EVM      "Enable EVM chain support"          ON)
option(BOAT_SOL      "Enable Solana chain support"       ON)
option(BOAT_PAY_X402 "Enable x402 payment protocol"      OFF)
option(BOAT_PAY_NANO "Enable Circle Nanopayments"         OFF)
option(BOAT_PAY_GATEWAY "Enable Circle Gateway"           OFF)
option(BOAT_VENDOR_CJSON "Use vendor-provided cJSON (skip bundled cJSON)" OFF)
set(BOAT_PAL "linux" CACHE STRING "Platform abstraction layer (linux)")

#-----------------------------------------------------------------------------
# Compile definitions from options
#-----------------------------------------------------------------------------
set(BOAT_DEFS "")
if(BOAT_EVM)
    list(APPEND BOAT_DEFS BOAT_EVM_ENABLED=1)
else()
    list(APPEND BOAT_DEFS BOAT_EVM_ENABLED=0)
endif()

if(BOAT_SOL)
    list(APPEND BOAT_DEFS BOAT_SOL_ENABLED=1)
else()
    list(APPEND BOAT_DEFS BOAT_SOL_ENABLED=0)
endif()

if(BOAT_PAY_X402)
    list(APPEND BOAT_DEFS BOAT_PAY_X402_ENABLED=1)
endif()
if(BOAT_PAY_NANO)
    list(APPEND BOAT_DEFS BOAT_PAY_NANO_ENABLED=1)
endif()
if(BOAT_PAY_GATEWAY)
    list(APPEND BOAT_DEFS BOAT_PAY_GATEWAY_ENABLED=1)
endif()

#-----------------------------------------------------------------------------
# Source files
#-----------------------------------------------------------------------------
set(BOAT_CORE_SRCS
    src/core/boat_util.c
    src/core/boat_key.c
    src/core/boat_key_soft.c
    src/core/boat_rpc.c
)

set(BOAT_EVM_SRCS "")
if(BOAT_EVM)
    set(BOAT_EVM_SRCS
        src/evm/evm_rpc.c
        src/evm/evm_tx.c
        src/evm/evm_abi.c
    )
endif()

set(BOAT_SOL_SRCS "")
if(BOAT_SOL)
    set(BOAT_SOL_SRCS
        src/sol/sol_rpc.c
        src/sol/sol_tx.c
        src/sol/sol_spl.c
        src/sol/sol_ix.c
        src/sol/sol_borsh.c
    )
endif()

set(BOAT_PAY_SRCS "")
if(BOAT_PAY_X402 OR BOAT_PAY_NANO OR BOAT_PAY_GATEWAY)
    list(APPEND BOAT_PAY_SRCS src/pay/pay_common.c)
endif()
if(BOAT_PAY_X402)
    list(APPEND BOAT_PAY_SRCS src/pay/pay_x402.c)
endif()
if(BOAT_PAY_NANO)
    list(APPEND BOAT_PAY_SRCS src/pay/pay_nano.c)
endif()
if(BOAT_PAY_GATEWAY)
    list(APPEND BOAT_PAY_SRCS src/pay/pay_gateway.c)
endif()

# PAL
set(BOAT_PAL_SRCS "")
if(BOAT_PAL STREQUAL "linux")
    set(BOAT_PAL_SRCS src/pal/linux/pal_linux.c)
endif()

# Third-party
file(GLOB BOAT_CRYPTO_SRCS third-party/crypto/*.c)
set(BOAT_CJSON_SRCS "")
if(NOT BOAT_VENDOR_CJSON)
    set(BOAT_CJSON_SRCS third-party/cJSON/cJSON.c)
endif()

set(BOAT_ALL_SRCS
    ${BOAT_CORE_SRCS}
    ${BOAT_EVM_SRCS}
    ${BOAT_SOL_SRCS}
    ${BOAT_PAY_SRCS}
    ${BOAT_PAL_SRCS}
    ${BOAT_CJSON_SRCS}
    ${BOAT_CRYPTO_SRCS}
)

#-----------------------------------------------------------------------------
# Library target
#-----------------------------------------------------------------------------
if(DEFINED ESP_PLATFORM)
    # ESP-IDF component mode
    set(_INCLUDE_DIRS include third-party/crypto)
    if(NOT BOAT_VENDOR_CJSON)
        list(APPEND _INCLUDE_DIRS third-party/cJSON)
    endif()
    idf_component_register(
        SRCS ${BOAT_ALL_SRCS}
        INCLUDE_DIRS ${_INCLUDE_DIRS}
        PRIV_REQUIRES mbedtls
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${BOAT_DEFS})
else()
    add_library(boat4 STATIC ${BOAT_ALL_SRCS})

    target_include_directories(boat4 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(boat4 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/crypto
    )
    if(NOT BOAT_VENDOR_CJSON)
        target_include_directories(boat4 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/cJSON
        )
    endif()
    target_compile_definitions(boat4 PUBLIC ${BOAT_DEFS})
    target_compile_definitions(boat4 PRIVATE RAND_PLATFORM_INDEPENDENT)

    # Platform dependencies
    if(BOAT_PAL STREQUAL "linux")
        find_package(CURL REQUIRED)
        target_link_libraries(boat4 PRIVATE CURL::libcurl pthread m)
    endif()
endif()

#-----------------------------------------------------------------------------
# Examples (standalone build only)
#-----------------------------------------------------------------------------
if(NOT DEFINED ESP_PLATFORM)
    option(BOAT_BUILD_EXAMPLES "Build example programs" OFF)
    if(BOAT_BUILD_EXAMPLES)
        add_executable(evm_transfer examples/evm_transfer.c)
        target_link_libraries(evm_transfer boat4)

        add_executable(evm_erc20 examples/evm_erc20.c)
        target_link_libraries(evm_erc20 boat4)

        add_executable(evm_erc20_transferfrom
            examples/evm_erc20_transferfrom.c
            examples/erc20_codec.c)
        target_link_libraries(evm_erc20_transferfrom boat4)

        if(BOAT_SOL)
            add_executable(sol_transfer examples/sol_transfer.c)
            target_link_libraries(sol_transfer boat4)
        endif()

        if(BOAT_PAY_X402)
            add_executable(pay_x402_demo examples/pay_x402_demo.c)
            target_link_libraries(pay_x402_demo boat4)
        endif()

        if(BOAT_PAY_NANO)
            add_executable(pay_nano_demo examples/pay_nano_demo.c)
            target_link_libraries(pay_nano_demo boat4)
        endif()

        if(BOAT_PAY_GATEWAY)
            add_executable(pay_gateway_demo examples/pay_gateway_demo.c)
            target_link_libraries(pay_gateway_demo boat4)
        endif()
    endif()

    #-------------------------------------------------------------------------
    # Testnet integration tests
    #-------------------------------------------------------------------------
    option(BOAT_BUILD_TESTS "Build testnet integration tests" OFF)
    if(BOAT_BUILD_TESTS)
        add_executable(test_eth_transfer tests/test_eth_transfer.c)
        target_link_libraries(test_eth_transfer boat4)

        add_executable(test_erc20_transfer tests/test_erc20_transfer.c)
        target_link_libraries(test_erc20_transfer boat4)

        if(BOAT_SOL)
            add_executable(test_sol_transfer tests/test_sol_transfer.c)
            target_link_libraries(test_sol_transfer boat4)

            add_executable(test_spl_transfer tests/test_spl_transfer.c)
            target_link_libraries(test_spl_transfer boat4)
        endif()

        if(BOAT_PAY_X402)
            add_executable(test_x402_payment tests/test_x402_payment.c)
            target_link_libraries(test_x402_payment boat4)
        endif()

        if(BOAT_PAY_GATEWAY)
            add_executable(test_gateway tests/test_gateway.c)
            target_link_libraries(test_gateway boat4)
        endif()

        if(BOAT_PAY_NANO)
            add_executable(test_nano tests/test_nano.c)
            target_link_libraries(test_nano boat4)
        endif()
    endif()
endif()
