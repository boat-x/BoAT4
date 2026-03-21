#include <stdio.h>
#include <string.h>
#include "sha3.h"

static void print_hex(const char *label, const uint8_t *data, size_t len) {
    printf("%s: ", label);
    for (size_t i = 0; i < len; i++) printf("0x%02x,", data[i]);
    printf("\n");
}

int main(void) {
    uint8_t hash[32];

    const char *ts = "TransferSpec(uint32 version,uint32 sourceDomain,uint32 destinationDomain,"
                     "bytes32 sourceContract,bytes32 destinationContract,"
                     "bytes32 sourceToken,bytes32 destinationToken,"
                     "bytes32 sourceDepositor,bytes32 destinationRecipient,"
                     "bytes32 sourceSigner,bytes32 destinationCaller,"
                     "uint256 value,bytes32 salt,bytes hookData)";

    /* BurnIntent typehash = keccak256(BurnIntent_type_string + TransferSpec_type_string) */
    const char *bi_prefix = "BurnIntent(uint256 maxBlockHeight,uint256 maxFee,TransferSpec spec)";

    char bi_full[2048];
    snprintf(bi_full, sizeof(bi_full), "%s%s", bi_prefix, ts);

    keccak_256((const uint8_t *)ts, strlen(ts), hash);
    print_hex("TransferSpec", hash, 32);

    keccak_256((const uint8_t *)bi_full, strlen(bi_full), hash);
    print_hex("BurnIntent  ", hash, 32);

    return 0;
}
