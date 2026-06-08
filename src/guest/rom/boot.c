#include "guest/rom/boot.h"
#include "core/filesystem.h"
#include "core/md5.h"
#include "core/option.h"
#include "guest/dreamcast.h"

struct boot {
  struct device;
  uint8_t rom[0x00200000];
};

static const char *boot_bin_path() {
  static char filename[PATH_MAX];

  if (!filename[0]) {
    const char *appdir = fs_appdir();
    snprintf(filename, sizeof(filename), "%s" PATH_SEPARATOR "boot.bin",
             appdir);
  }

  return filename;
}

static int boot_validate(struct boot *boot) {
  static const char *valid_bios_md5[] = {
      "a5c6a00818f97c5e3e91569ee22416dc", /* chinese bios */
      "37c921eb47532cae8fb70e5d987ce91c", /* japanese bios */
      "f2cd29d09f3e29984bcea22ab2e006fe", /* revised bios w/o MIL-CD */
      "e10c53c2f8b90bab96ead2d368858623"  /* original US/EU bios */
  };

  /* compare the rom's md5 against known good bios roms */
  MD5_CTX md5_ctx;
  MD5_Init(&md5_ctx);
  MD5_Update(&md5_ctx, boot->rom, sizeof(boot->rom));
  char result[33];
  MD5_Final(result, &md5_ctx);

  /* re-encode digest bytes as hex string in-place */
  {
    unsigned int a = (unsigned char)result[0]  | ((unsigned char)result[1]  << 8) |
                     ((unsigned char)result[2]  << 16) | ((unsigned char)result[3]  << 24);
    unsigned int b = (unsigned char)result[4]  | ((unsigned char)result[5]  << 8) |
                     ((unsigned char)result[6]  << 16) | ((unsigned char)result[7]  << 24);
    unsigned int c = (unsigned char)result[8]  | ((unsigned char)result[9]  << 8) |
                     ((unsigned char)result[10] << 16) | ((unsigned char)result[11] << 24);
    unsigned int d = (unsigned char)result[12] | ((unsigned char)result[13] << 8) |
                     ((unsigned char)result[14] << 16) | ((unsigned char)result[15] << 24);
    snprintf(result, 33, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
      a&0xff,(a>>8)&0xff,(a>>16)&0xff,(a>>24)&0xff,
      b&0xff,(b>>8)&0xff,(b>>16)&0xff,(b>>24)&0xff,
      c&0xff,(c>>8)&0xff,(c>>16)&0xff,(c>>24)&0xff,
      d&0xff,(d>>8)&0xff,(d>>16)&0xff,(d>>24)&0xff);
  }

  for (int i = 0; i < array_size(valid_bios_md5); ++i) {
    if (strcmp(result, valid_bios_md5[i]) == 0) {
      return 1;
    }
  }

  return 0;
}

static int boot_load_rom(struct boot *boot) {
  const char *filename = boot_bin_path();

  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    LOG_WARNING("failed to load '%s'", filename);
    return 0;
  }

  fseek(fp, 0, SEEK_END);
  int size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (size != (int)sizeof(boot->rom)) {
    LOG_WARNING("boot rom size mismatch, is %d, expected %d", size,
                sizeof(boot->rom));
    fclose(fp);
    return 0;
  }

  int n = (int)fread(boot->rom, sizeof(uint8_t), size, fp);
  CHECK_EQ(n, size);
  fclose(fp);

  if (!boot_validate(boot)) {
    LOG_WARNING("failed to validate boot rom");
    return 0;
  }

  LOG_INFO("boot_load_rom loaded '%s'", filename);

  return 1;
}

static uint32_t boot_rom_read(struct boot *boot, uint32_t addr,
                              uint32_t data_mask) {
  return READ_DATA(&boot->rom[addr]);
}

static int boot_init(struct device *dev) {
  struct boot *boot = (struct boot *)dev;

  /* attempt to load the boot rom, if this fails, the bios code will hle it */
  boot_load_rom(boot);

  return 1;
}

void boot_write(struct boot *boot, int offset, const void *data, int n) {
  CHECK(offset >= 0 && (offset + n) <= (int)sizeof(boot->rom));

  memcpy(&boot->rom[offset], data, n);
}

void boot_read(struct boot *boot, int offset, void *data, int n) {
  CHECK(offset >= 0 && (offset + n) <= (int)sizeof(boot->rom));

  memcpy(data, &boot->rom[offset], n);
}

void boot_destroy(struct boot *boot) {
  dc_destroy_device((struct device *)boot);
}

struct boot *boot_create(struct dreamcast *dc) {
  struct boot *boot =
      dc_create_device(dc, sizeof(struct boot), "boot", &boot_init);
  return boot;
}

/* clang-format off */
AM_BEGIN(struct boot, boot_rom_map);
  AM_RANGE(0x00000000, 0x001fffff) AM_HANDLE("boot rom",
                                             (mmio_read_cb)&boot_rom_read,
                                             NULL,
                                             NULL, NULL)
AM_END();
/* clang-format on */
