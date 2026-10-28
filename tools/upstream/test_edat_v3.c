/* Focused test for patch 5001 (EDAT reader: guest klicensee from sceNpDrmIsAvailable[2]).
 *
 *   test_edat_v3 EDAT ELF OUT
 *
 * EDAT  an NPD v3, license type 3 EDAT whose klicensee is title-specific
 *       (KZ3 1.14: USRDIR/added_data_ps3.psarc.edat)
 * ELF   the decrypted executable of the same title; on hardware the game passes
 *       16 bytes from it to sceNpDrmIsAvailable2 before opening the EDAT
 * OUT   plaintext output (work/ only, never committed)
 *
 * Steps: (1) without a guest key the reader must refuse with -6 (none of the
 * built-in klicensees verifies dev_hash); (2) find the 16-byte window in ELF that
 * verifies dev_hash -- this stands in for the guest call, the key itself is never
 * printed; (3) register it with edat_add_klicensee, as sceNpDrmIsAvailable2 does;
 * (4) decrypt (every block hash is checked by the reader) and check the output
 * starts with the PSARC magic. Prints PASS/FAIL. Built against a tree with 5001
 * applied by test_edat_v3.sh (edat.c is included so its static AES-CMAC is usable). */
#include "edat.c"

static uint8_t* slurp(const char* p, size_t* n)
{
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int main(int argc, char** argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s EDAT ELF OUT\n", argv[0]); return 2; }
    uint8_t hdr[0x100];
    FILE* f = fopen(argv[1], "rb");
    if (!f || fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { printf("FAIL: cannot read %s\n", argv[1]); return 1; }
    fclose(f);
    printf("NPD version %u license %u type 0x%X flags 0x%X block 0x%X\n",
           be32(hdr + 4), be32(hdr + 8), be32(hdr + 0xC), be32(hdr + 0x80), be32(hdr + 0x84));

    int rc = edat_decrypt_file(argv[1], argv[3]);
    printf("step 1: without a guest klicensee rc=%d (expect -6)\n", rc);
    if (rc != -6) { printf("FAIL: expected -6\n"); return 1; }

    size_t n = 0;
    uint8_t* elf = slurp(argv[2], &n);
    if (!elf || n < 16) { printf("FAIL: cannot read %s\n", argv[2]); return 1; }
    long hit = -1;
    /* 16-byte aligned first (where a compiler puts a const u8[16]), then every byte. */
    for (int pass = 0; pass < 2 && hit < 0; pass++) {
        size_t step = pass == 0 ? 16 : 1;
        for (size_t o = 0; o + 16 <= n; o += step) {
            if (pass == 1 && (o & 15) == 0) continue;
            uint8_t ck[16], mac[16];
            for (int i = 0; i < 16; i++) ck[i] = (uint8_t)(elf[o + i] ^ NP_OMAC_KEY_2[i]);
            aes128_cmac(ck, hdr, 0x60, mac);
            if (memcmp(mac, hdr + 0x60, 16) == 0) { hit = (long)o; break; }
        }
    }
    if (hit < 0) { printf("FAIL: no 16-byte window of the ELF verifies dev_hash\n"); return 1; }
    printf("step 2: klicensee verifies dev_hash at ELF file offset 0x%lX\n", hit);

    edat_add_klicensee(elf + hit);
    rc = edat_decrypt_file(argv[1], argv[3]);
    printf("step 3: with the guest klicensee rc=%d (expect 0)\n", rc);
    if (rc != 0) { printf("FAIL\n"); return 1; }

    uint8_t magic[4] = {0};
    f = fopen(argv[3], "rb");
    if (!f || fread(magic, 1, 4, f) != 4) { printf("FAIL: no output\n"); return 1; }
    fseek(f, 0, SEEK_END);
    printf("step 4: output %ld bytes (header file size %llu), magic %.4s\n",
           ftell(f), (unsigned long long)be64(hdr + 0x88), (const char*)magic);
    fclose(f);
    if (memcmp(magic, "PSAR", 4) != 0) { printf("FAIL: not a PSARC\n"); return 1; }
    printf("PASS\n");
    return 0;
}
