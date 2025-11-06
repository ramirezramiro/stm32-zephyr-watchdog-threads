#include "persist_state.h"
#include "log_utils.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

LOG_MODULE_REGISTER(persist_state, LOG_LEVEL_INF);

#define PERSIST_MAGIC 0x4C454452u /* 'LEDR' */
#define PERSIST_RECORD_ID 1

#define STORAGE_PARTITION_NODE DT_NODELABEL(storage_partition)
#define PERSIST_RETRY_LIMIT 3
#define PERSIST_RETRY_DELAY_MS 10

struct persist_blob {
	uint32_t magic;
	uint32_t consecutive_watchdog;
	uint32_t total_watchdog;
	uint32_t watchdog_override_ms;
};

static struct {
	struct nvs_fs fs;
	struct persist_blob blob;
	bool loaded;
} g_state;

static K_MUTEX_DEFINE(state_lock);

static int persist_commit_locked(void)
{
	int rc = nvs_write(&g_state.fs, PERSIST_RECORD_ID,
			   &g_state.blob, sizeof(g_state.blob));

	if (rc < 0) {
		LOG_ERR("Persistent write failed: %d", rc);
		LOG_EVT(ERR, "PERSIST", "WRITE_FAIL", "rc=%d", rc);
		return rc;
	}

	return 0;
}

static int init_fs_if_needed(void)
{
	if (g_state.loaded) {
		return 0;
	}

	const struct flash_area *fa = NULL;
	int rc = 0;

	for (int attempt = 1; attempt <= PERSIST_RETRY_LIMIT; ++attempt) {
		rc = flash_area_open(DT_FIXED_PARTITION_ID(STORAGE_PARTITION_NODE), &fa);
		if (rc == 0) {
			if (attempt > 1) {
				LOG_EVT(INF, "PERSIST", "FLASH_OPEN_RECOVERED", "attempt=%d", attempt);
			}
			break;
		}

		LOG_WRN("Failed to open storage partition (attempt %d/%d): %d",
			attempt, PERSIST_RETRY_LIMIT, rc);
		LOG_EVT(WRN, "PERSIST", "FLASH_OPEN_RETRY", "attempt=%d,rc=%d", attempt, rc);

		if (attempt < PERSIST_RETRY_LIMIT) {
			k_msleep(PERSIST_RETRY_DELAY_MS);
		}
	}

	if (rc != 0) {
		LOG_EVT(ERR, "PERSIST", "FLASH_OPEN_FAIL", "rc=%d", rc);
		return rc;
	}

	g_state.fs.flash_device = fa->fa_dev;
	if (!device_is_ready(g_state.fs.flash_device)) {
		LOG_ERR("Storage flash device not ready");
		LOG_EVT(ERR, "PERSIST", "FLASH_NOT_READY", "dev_id=%u",
			DT_FIXED_PARTITION_ID(STORAGE_PARTITION_NODE));
		flash_area_close(fa);
		return -EBUSY;
	}

	g_state.fs.offset = fa->fa_off;

	struct flash_pages_info page_info;
	rc = flash_get_page_info_by_offs(g_state.fs.flash_device,
					 g_state.fs.offset, &page_info);
	if (rc != 0) {
		LOG_ERR("flash_get_page_info_by_offs failed: %d", rc);
		LOG_EVT(ERR, "PERSIST", "FLASH_PAGE_INFO_FAIL", "rc=%d", rc);
		flash_area_close(fa);
		return rc;
	}

	g_state.fs.sector_size = page_info.size;
	g_state.fs.sector_count = fa->fa_size / page_info.size;

	flash_area_close(fa);

	for (int attempt = 1; attempt <= PERSIST_RETRY_LIMIT; ++attempt) {
		rc = nvs_mount(&g_state.fs);
		if (rc == 0) {
			if (attempt > 1) {
				LOG_EVT(INF, "PERSIST", "NVS_MOUNT_RECOVERED", "attempt=%d", attempt);
			}
			break;
		}

		LOG_WRN("Failed to mount NVS (attempt %d/%d): %d",
			attempt, PERSIST_RETRY_LIMIT, rc);
		LOG_EVT(WRN, "PERSIST", "NVS_MOUNT_RETRY", "attempt=%d,rc=%d", attempt, rc);

		if (attempt < PERSIST_RETRY_LIMIT) {
			k_msleep(PERSIST_RETRY_DELAY_MS);
		}
	}

	if (rc != 0) {
		LOG_EVT(ERR, "PERSIST", "NVS_MOUNT_FAIL", "rc=%d", rc);
		return rc;
	}

	struct persist_blob tmp;
	rc = nvs_read(&g_state.fs, PERSIST_RECORD_ID, &tmp, sizeof(tmp));
	if (rc == sizeof(tmp) && tmp.magic == PERSIST_MAGIC) {
		g_state.blob = tmp;
	} else {
		g_state.blob.magic = PERSIST_MAGIC;
		g_state.blob.consecutive_watchdog = 0U;
		g_state.blob.total_watchdog = 0U;
		g_state.blob.watchdog_override_ms = 0U;
		(void)persist_commit_locked();
	}

	g_state.loaded = true;
	LOG_INF("Persistent state loaded: consecutive=%u total=%u override=%u",
		g_state.blob.consecutive_watchdog,
		g_state.blob.total_watchdog,
		g_state.blob.watchdog_override_ms);
	return 0;
}

int persist_state_init(void)
{
	int rc;

	k_mutex_lock(&state_lock, K_FOREVER);
	rc = init_fs_if_needed();
	k_mutex_unlock(&state_lock);

	return rc;
}

void persist_state_record_boot(bool watchdog_reset)
{
	k_mutex_lock(&state_lock, K_FOREVER);

	if (init_fs_if_needed() != 0) {
		k_mutex_unlock(&state_lock);
		return;
	}

	if (watchdog_reset) {
		g_state.blob.consecutive_watchdog++;
		g_state.blob.total_watchdog++;
	} else if (g_state.blob.consecutive_watchdog != 0U) {
		g_state.blob.consecutive_watchdog = 0U;
	}

	(void)persist_commit_locked();
	k_mutex_unlock(&state_lock);
}

void persist_state_clear_watchdog_counter(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);

	if (init_fs_if_needed() == 0 &&
	    g_state.blob.consecutive_watchdog != 0U) {
		g_state.blob.consecutive_watchdog = 0U;
		(void)persist_commit_locked();
	}

	k_mutex_unlock(&state_lock);
}

uint32_t persist_state_get_consecutive_watchdog(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	uint32_t value = g_state.blob.consecutive_watchdog;
	k_mutex_unlock(&state_lock);
	return value;
}

uint32_t persist_state_get_total_watchdog(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	uint32_t value = g_state.blob.total_watchdog;
	k_mutex_unlock(&state_lock);
	return value;
}

bool persist_state_is_fallback_active(void)
{
	return persist_state_get_consecutive_watchdog() >=
	       (uint32_t)CONFIG_APP_RESET_WATCHDOG_THRESHOLD;
}

uint32_t persist_state_get_watchdog_override(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	uint32_t value = g_state.blob.watchdog_override_ms;
	k_mutex_unlock(&state_lock);
	return value;
}

int persist_state_set_watchdog_override(uint32_t timeout_ms)
{
	int rc = 0;

	k_mutex_lock(&state_lock, K_FOREVER);

	if (init_fs_if_needed() != 0) {
		k_mutex_unlock(&state_lock);
		return -EIO;
	}

	if (g_state.blob.watchdog_override_ms != timeout_ms) {
		g_state.blob.watchdog_override_ms = timeout_ms;
		rc = persist_commit_locked();
	}

	k_mutex_unlock(&state_lock);
	return rc;
}

#if defined(CONFIG_ZTEST)
void persist_state_test_reset(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);

	if (init_fs_if_needed() == 0) {
		(void)nvs_clear(&g_state.fs);
	}

	memset(&g_state.fs, 0, sizeof(g_state.fs));
	memset(&g_state.blob, 0, sizeof(g_state.blob));
	g_state.loaded = false;
	k_mutex_unlock(&state_lock);
}

void persist_state_test_reload(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	memset(&g_state.fs, 0, sizeof(g_state.fs));
	memset(&g_state.blob, 0, sizeof(g_state.blob));
	g_state.loaded = false;
	k_mutex_unlock(&state_lock);
}
#endif
