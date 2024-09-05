#ifndef DELTA_APPLY_H
#define DELTA_APPLY_H
 
#include <zephyr/kernel.h>
#if defined(CONFIG_DELTA_UPDATE)
   #include <zephyr/delta/delta.h>
#endif
 
/**
 * @brief Initialize all the callbacks for the delta API and then apply the delta algorithm.
 *
 * @param delta_api Pointer to the delta api structure.
 * @return 0 on success, negative error code on failure.
 */
int delta_apply_init(struct delta_api_t *delta_api);
 
/**
 * @brief Apply the patch using the delta algorithm defined in backend and write the magic to define the new
 * firmware as a bootable image.
 *
 * @param delta_api Pointer to the delta api structure.
 * @return 0 on success, negative error code on failure.
 */
int delta_apply_algo(struct delta_api_t *delta_api);
 
/**
 * @brief Initialize the delta API and then apply the patch using the delta algorithm defined in backend.
 * Reboot the device after writing the new FW.
 *
 * @param void
 * @return 0 on success, negative error code on failure.
 */
int delta_apply(struct delta_api_t *delta_api);
 
#endif /* DELTA_APPLY_H */
 