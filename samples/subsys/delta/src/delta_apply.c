/**
 * @file delta_apply.c
 * @brief Implementation of a Delta Firmware Update using the delta API
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
 
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
 
#if defined(CONFIG_DELTA_UPDATE)
   #include <zephyr/delta/delta.h>
#endif

#include <string.h>

#define BSDIFF_MAGIC "BSDIFFHS"
#define BSDIFF_MAGIC_SIZE 8
#define TARGET_FIRMWARE_SIZE 8
#define HEATSHRINK_WINDOW_SZ2_SIZE 1
#define HEATSHRINK_LOOKAHEAD_SZ2_SIZE 1
#define BSDIFF_HEADER_SIZE  BSDIFF_MAGIC_SIZE + TARGET_FIRMWARE_SIZE + HEATSHRINK_WINDOW_SZ2_SIZE + HEATSHRINK_LOOKAHEAD_SZ2_SIZE
 
LOG_MODULE_REGISTER(delta_apply, CONFIG_DELTA_UPDATE_LOG_LEVEL);
 
/**
 * @brief Implementation of the callback delta_read_t to read the flash memory in slot 0 partition to get
 * current firmware's data and read in the RAM memory to get patch's data.
 *
 * @param arg_p Pointer to the delta memory structure.
 * @param buf_p Buffer to store the read data.
 * @param size Number of bytes to read.
 * @return 0 on success, negative error code on failure.
 */
static int delta_mem_read(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size);
 
/**
 * @brief Implementation of the callback delta_write_t to write on the flash memory (on Slot 1 partition).
 *
 * @param arg_p Pointer to the delta memory structure.
 * @param buf_p Buffer containing the data to write.
 * @param size Number of bytes to write.
 * @param flush Whether to flush the data to storage.
 * @return 0 on success, negative error code on failure.
 */
static int delta_mem_write(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size, bool flush);
 
/**
 * @brief Implementation of the callback delta_mem_erase_t to erase the flash memory.
 *
 * @param arg_p Pointer to the delta memory structure.
 * @param offset Start offset of the region to erase.
 * @param size Number of bytes to erase.
 * @return 0 on success, negative error code on failure.
 */
static int delta_mem_erase(delta_memory_struct_t *arg_p, uint64_t offset, size_t size);
 
/**
 * @brief Implementation of the callback delta_seek_t to seek to a specific position in the delta memory for
 * source and patch.
 *
 * @param arg_p Pointer to the delta memory structure.
 * @param source_offset Offset for the source firmware.
 * @param patch_offset Offset for the patch data.
 * @return 0 on success.
 */
static int delta_mem_seek(delta_memory_struct_t *arg_p,
                          size_t                 source_offset,
                          size_t                 patch_offset,
                          size_t                 target_offset);
 

/**
 * @brief Check that a valid patch is present.
 * Header of the patch :
 * MAGIC : BSDIFFHS (8 bytes)
 * Size of the target firmware (8 bytes)
 * Heatshrink window_sz2 configuration (1 byte)
 * Heatshrink lookahead_sz2 configuration (1 byte)
 *
 * @param delta_api Pointer to the delta api structure.
 * @return 0 on success, negative error code on failure.
 */
static bool valid_patch_present(struct delta_api_t *delta_api)
{
    uint8_t header[BSDIFF_HEADER_SIZE];
    int ret;

    /* Read in the partition where the patch is stored */
	 delta_api->memory.slot = PATCH_STORAGE;

    ret = delta_api->read(&delta_api->memory, header, BSDIFF_HEADER_SIZE);
    if (ret != 0) {
        LOG_ERR("Failed to read patch (%d)", ret);
        return false;
    }

    /* Check magic */
    if (memcmp(header, BSDIFF_MAGIC, BSDIFF_MAGIC_SIZE) != 0) {
        LOG_INF("No valid patch in patch_partition (magic mismatch)");
        return false;
    }

    /* Read new_size (little-endian 16-bit) */
    uint16_t new_size = header[8] | (header[9] << 8);
    delta_api->memory.offset.new_size  = new_size;

    delta_api->heatshrink_window_sz2 = header[16];
    delta_api->heatshrink_lookahead_sz2 = header[17];

    LOG_DBG("window_sz2 = %d, lookahead_sz2 = %d", delta_api->heatshrink_window_sz2, delta_api->heatshrink_lookahead_sz2);

    LOG_INF("Valid patch detected in patch_partition");
    LOG_INF("Patch new_size = %u bytes", new_size);

    delta_api->memory.offset.patch    = BSDIFF_HEADER_SIZE;
     
    return true;
}


/**
 * @brief Initialize all the callbacks for the delta API and then apply the delta algorithm.
 *
 * @param delta_api Pointer to the delta api structure.
 * @return 0 on success, negative error code on failure.
 */
int delta_apply_init(struct delta_api_t *delta_api)
{
   int ret;
 
   if(!delta_api)
   {
      LOG_ERR("Pointer delta_api in delta_apply_init is null");
      return -ENODEV;
   }
 
   /* Init flash img to write the new firmware (on slot 1) */
   ret = flash_img_init(&delta_api->memory.flash.img_ctx);
   if(ret != 0)
   {
      LOG_ERR("Can't initialise flash img, ret = %d ", ret);
      return ret;
   }
 
   /* Init all the callbacks for the delta api */
   ret = delta_apply_patch_init(delta_api, delta_mem_read, delta_mem_write, delta_mem_seek, delta_mem_erase);
   if(ret != 0)
   {
      LOG_ERR("delta apply patch failed during initialization, ret : %d\n", ret);
      return ret;
   }
 
   /* Init the offsets for source, patch and target at 0 */
   ret = delta_api->seek(&delta_api->memory, 0, delta_api->memory.offset.patch, 0);
   if(ret != 0)
   {
      LOG_ERR("delta api seek offset failed, ret = %d", ret);
      return ret;
   }
 
   return ret;
}
 
/**
 * @brief Apply the patch using the delta algorithm defined in backend and write the magic to define the new
 * firmware as a bootable image.
 *
 * @param delta_api Pointer to the delta api structure.
 * @return 0 on success, negative error code on failure.
 */
int delta_apply_algo(struct delta_api_t *delta_api)
{
   int ret;
 
   if(!delta_api)
   {
      LOG_ERR("Pointer delta_api in delta_apply_algo is null");
      return -ENODEV;
   }
 
   /* Apply the patch using a delta algorithm */
   ret = delta_api->backend.patch(delta_api);
   if(ret != 0)
   {
      LOG_ERR("apply patch failed\n");
      return ret;
   }
 
   ret = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
   if(ret != 0)
   {
      LOG_ERR("Boot request error : %d\n", ret);
      return ret;
   }
 
   return ret;
}
 
/**
 * @brief Initialize the delta API and then apply the patch using the delta algorithm defined in backend.
 * Reboot the device after writing the new FW.
 *
 * @param void
 * @return 0 on success, negative error code on failure.
 */
int delta_apply(struct delta_api_t *delta_api)
{
   int ret;
 
   if(!delta_api)
   {
      LOG_ERR("Pointer delta_api in delta_apply is null");
      return -ENODEV;
   }
 
   ret = delta_apply_init(delta_api);
   if(ret != 0)
   {
      LOG_ERR("The delta API initialization failed, ret = %d", ret);
      return ret;
   }

   if (!valid_patch_present(delta_api)) {
        LOG_ERR("Skipping delta update: no patch valid present");
        return -ECANCELED;
   }
 
   ret = delta_apply_algo(delta_api);
   if(ret != 0)
   {
      LOG_ERR("The delta application algorithm failed, ret = %d", ret);
      return ret;
   }
   LOG_INF("The new FW was successfully written, now rebooting...");
 
#if defined(CONFIG_DELTA_UPDATE_LOG_LEVEL_DBG)
   /* flush the logs before reboot */
   log_panic();
#endif

   sys_reboot(SYS_REBOOT_COLD);
 
   return ret;
}
 
static int delta_mem_read(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size)
{
   int ret = 0;
 
   if(arg_p->slot == SLOT_0)
   {
      ret = flash_area_open(arg_p->slot, &arg_p->flash.flash_area);
      if(ret != 0)
      {
         LOG_ERR("Can not open the flash area for %d", arg_p->slot);
         return -ENODEV;
      }
 
      ret = flash_area_read(arg_p->flash.flash_area, arg_p->offset.source, buf_p, size);
      if(ret != 0)
      {
         LOG_ERR("Can not read %d bytes at offset : %llu", size, arg_p->offset.source);
         return -EINVAL;
      }
      flash_area_close(arg_p->flash.flash_area);
   }
   else if(arg_p->slot == PATCH_STORAGE)
   {
      if(arg_p->offset.patch + size > CONFIG_DELTA_TRANSFER_PATCH_SIZE)
      {
         LOG_ERR("Read operation exceeds buffer boundaries");
         return -EINVAL;
      }

      ret = flash_area_open(arg_p->slot, &arg_p->flash.flash_area);
      if(ret != 0)
      {
         LOG_ERR("Can not open the flash area for %d", arg_p->slot);
         return -ENODEV;
      }
 
      ret = flash_area_read(arg_p->flash.flash_area, arg_p->offset.patch, buf_p, size);
      if(ret != 0)
      {
         LOG_ERR("Can not read %d bytes at offset : %llu", size, arg_p->offset.patch);
         return -EINVAL;
      }
      flash_area_close(arg_p->flash.flash_area);
   }
 
   return ret;
}
 
static int delta_mem_write(delta_memory_struct_t *arg_p, uint8_t *buf_p, size_t size, bool flush)
{
   int ret;
 
   ret = flash_img_buffered_write(&arg_p->flash.img_ctx, buf_p, size, flush);
   if(ret != 0)
   {
      LOG_ERR("Flash write error");
      return -EINVAL;
   }
 
   return ret;
}
 
static int delta_mem_erase(delta_memory_struct_t *arg_p, uint64_t offset, size_t size)
{
   int ret;
 
   ret = flash_area_open(arg_p->slot, &arg_p->flash.flash_area);
   if(ret != 0)
   {
      LOG_ERR("Can not open the flash area for %d", arg_p->slot);
      return -ENODEV;
   }
 
   ret = flash_area_erase(arg_p->flash.flash_area, offset, size);
   if(ret != 0)
   {
      LOG_ERR("Can not erase the flash area for slot1 partition, ret = %d\n", ret);
      return -EINVAL;
   }
   flash_area_close(arg_p->flash.flash_area);
 
   return ret;
}
 
static int delta_mem_seek(delta_memory_struct_t *arg_p,
                          size_t                 source_offset,
                          size_t                 patch_offset,
                          size_t                 target_offset)
{
   arg_p->offset.source = source_offset;
   arg_p->offset.patch  = patch_offset;
   arg_p->offset.target = target_offset;
 
   return 0;
}
 