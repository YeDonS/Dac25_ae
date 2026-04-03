#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fiemap.h>
#include <linux/fs.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef TARGET_FOLDER
#define TARGET_FOLDER "./device/"
#endif

#ifndef RESULT_FOLDER
#define RESULT_FOLDER "./result/"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STR1_LEN 7281
#define STR2_LEN 7281
#define STR3_LEN 7281
#define STR4_LEN 7281
#define ROW_PAYLOAD_BYTES 32768ULL
#define ROW_EST_PAGES 8U
#define LOGICAL_PAGE_BYTES 4096ULL
#define DEFAULT_TARGET_BYTES (8ULL << 30)
#define DEFAULT_TABLE_COUNT 80U
#define DEFAULT_WINDOW_TABLES 80U
#define DEFAULT_WINDOW_PAGES_PER_TABLE 960U
#define DEFAULT_WINDOW_PASSES_PER_ROUND 1U
#define DEFAULT_INTERLEAVE_PAGES 209715U
#define DEFAULT_INTERLEAVE_READS 1000U
#define DEFAULT_REFSTYLE_DUMMY_BYTES (88ULL * 1024ULL)
#define DEFAULT_TAG "default"
#define DEFAULT_PAGE_TIER_PATH "/sys/kernel/debug/nvmev/ftl0/page_tier"
#define DEFAULT_PAGE_DIE_PATH "/sys/kernel/debug/nvmev/ftl0/page_die"
#define DEFAULT_PAGE_DIE_TRANSITION_PATH "/sys/kernel/debug/nvmev/ftl0/page_die_transition"
#define DEFAULT_FTL_HOST_PAGE_BYTES 4096ULL
#define DEFAULT_TEST_PHASE_PATH "/sys/kernel/debug/nvmev/ftl0/test_phase"
#define DIE_STATS_RESET_PATH "/sys/module/nvmev/parameters/die_stats_reset"

struct page_die_entry {
	unsigned long long lpn;
	unsigned int die;
};

struct page_tier_entry {
	unsigned long long lpn;
	unsigned int in_slc;
	unsigned int qlc_zone;
	unsigned int qlc_zone_known;
};

struct page_die_transition_entry {
	unsigned long long lpn;
	int initial_die;
	unsigned int current_die;
	unsigned int die_changed;
	unsigned int reason;
};

struct file_extent {
	unsigned long long logical;
	unsigned long long physical;
	unsigned long long length;
};

struct dataset_layout {
	unsigned int table_count;
	unsigned int total_rows;
	unsigned int *rows_per_table;
	unsigned int *row_prefix;
};

struct workload_options {
	unsigned int table_count;
	unsigned int rows_per_table;
	unsigned long long target_bytes;
	unsigned int window_tables;
	unsigned int window_pages_per_table;
	unsigned int window_passes_per_round;
	unsigned int interleave_pages;
	unsigned int interleave_reads;
	unsigned int cold_concurrent_threads;
	unsigned long long ftl_host_page_bytes;
	const char *tag;
	const char *page_tier_path;
	const char *page_die_path;
	const char *page_die_transition_path;
	unsigned int seed;
	const char *dist_name;
	double zipf_alpha;
	double exp_lambda;
	double normal_mean;
	double normal_stddev;
	bool direct_io;
	unsigned long long refstyle_dummy_bytes;
	unsigned int align_pages;
	const char *test_phase_path;
};

struct refstyle_dummy_state {
	int fd;
	void *buf;
	unsigned long long total_bytes_written;
};

struct table_file_state {
	unsigned int table_id;
	unsigned int total_rows;
	unsigned int rows_inserted;
	char db_path[PATH_MAX];
	sqlite3 *db;
	sqlite3_stmt *insert_stmt;
	sqlite3_stmt *scan_stmt;
	unsigned long long *row_reads;
	double *row_latency;
};

struct concurrent_read_ctx {
	unsigned int thread_id;
	unsigned int table_id;
	const char *db_path;
	unsigned int record_id_begin;
	unsigned int record_id_end;
	unsigned int repeats;
	double elapsed_sec;
	unsigned long long rows_read;
};

static const struct option long_opts[] = {
	{"mode", required_argument, NULL, 'm'},
	{"tag", required_argument, NULL, 1000},
	{"table-count", required_argument, NULL, 1001},
	{"rows-per-table", required_argument, NULL, 1002},
	{"target-bytes", required_argument, NULL, 1003},
	{"window-tables", required_argument, NULL, 1004},
	{"window-pages-per-table", required_argument, NULL, 1005},
	{"window-passes-per-round", required_argument, NULL, 1028},
	{"interleave-pages", required_argument, NULL, 1006},
	{"interleave-reads", required_argument, NULL, 1007},
	{"cold-concurrent-threads", required_argument, NULL, 1008},
	{"ftl-host-page-bytes", required_argument, NULL, 1009},
	{"page-die-path", required_argument, NULL, 1010},
	{"page-die-transition-path", required_argument, NULL, 1030},
	{"distribution", required_argument, NULL, 1011},
	{"seed", required_argument, NULL, 's'},
	{"zipf-seed", required_argument, NULL, 1012},
	{"exp-seed", required_argument, NULL, 1013},
	{"normal-seed", required_argument, NULL, 1014},
	{"alpha", required_argument, NULL, 1015},
	{"lambda", required_argument, NULL, 1016},
	{"normal-mean", required_argument, NULL, 1017},
	{"normal-stddev", required_argument, NULL, 1018},
	{"cold-full-read-mode", required_argument, NULL, 1019},
	{"cold-full-read-iters", required_argument, NULL, 1020},
	{"strict-cold-per-select", no_argument, NULL, 1021},
	{"test-phase-path", required_argument, NULL, 1022},
	{"page-tier-path", required_argument, NULL, 1023},
	{"access-count-path", required_argument, NULL, 1024},
	{"direct-io", no_argument, NULL, 1025},
	{"fast-init-profile", no_argument, NULL, 1026},
	{"refstyle-dummy-bytes", required_argument, NULL, 1027},
	{"align-pages", required_argument, NULL, 1029},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static double monotonic_sec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned int next_rand(unsigned int *state)
{
	*state = (*state * 1103515245u + 12345u);
	return *state;
}

static double rand_uniform(unsigned int *state)
{
	return (double)(next_rand(state) & 0x7fffffffU) / 2147483647.0;
}

static void random_string(char *buf, size_t len)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)buf;

	if (!buf || len == 0)
		return;
	for (size_t i = 0; i + 1 < len; ++i)
		buf[i] = table[next_rand(&seed) % (sizeof(table) - 1)];
	buf[len - 1] = '\0';
}

static unsigned long long parse_size_arg(const char *arg)
{
	char *end = NULL;
	double v;
	unsigned long long mult = 1ULL;

	if (!arg || !*arg)
		return 0;

	v = strtod(arg, &end);
	if (end && *end) {
		if (!strcasecmp(end, "k") || !strcasecmp(end, "kb"))
			mult = 1024ULL;
		else if (!strcasecmp(end, "m") || !strcasecmp(end, "mb"))
			mult = 1024ULL * 1024ULL;
		else if (!strcasecmp(end, "g") || !strcasecmp(end, "gb"))
			mult = 1024ULL * 1024ULL * 1024ULL;
	}
	if (v <= 0.0)
		return 0;
	return (unsigned long long)(v * (double)mult);
}

static void join_path(char *dst, size_t len, const char *dir, const char *leaf)
{
	if (!dir || !*dir) {
		snprintf(dst, len, "%s", leaf ? leaf : "");
		return;
	}
	if (!leaf || !*leaf) {
		snprintf(dst, len, "%s", dir);
		return;
	}
	if (dir[strlen(dir) - 1] == '/')
		snprintf(dst, len, "%s%s", dir, leaf);
	else
		snprintf(dst, len, "%s/%s", dir, leaf);
}

static int ensure_directory(const char *path)
{
	struct stat st;

	if (!path || !*path)
		return 0;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -errno;
}

static unsigned int ceil_div_u64_to_u32(unsigned long long num, unsigned long long den)
{
	if (den == 0)
		return 0;
	return (unsigned int)((num + den - 1ULL) / den);
}

static unsigned long long refstyle_dummy_total_bytes(const struct workload_options *opts,
						     unsigned int sqlite_pages)
{
	unsigned long long host_page_bytes;
	unsigned long long bytes;
	unsigned int base_dummy_pages;
	unsigned int rem;

	if (!opts || opts->refstyle_dummy_bytes == 0)
		return 0;

	host_page_bytes = opts->ftl_host_page_bytes ?
		opts->ftl_host_page_bytes : DEFAULT_FTL_HOST_PAGE_BYTES;
	bytes = opts->refstyle_dummy_bytes;
	if (opts->align_pages == 0 || host_page_bytes == 0)
		return bytes;

	base_dummy_pages = ceil_div_u64_to_u32(bytes, host_page_bytes);
	rem = (sqlite_pages + base_dummy_pages) % opts->align_pages;
	if (rem != 0)
		bytes += (unsigned long long)(opts->align_pages - rem) * host_page_bytes;

	return bytes;
}

static int write_string_file(const char *path, const char *value)
{
	FILE *fp;

	if (!path || !*path || !value)
		return -EINVAL;

	fp = fopen(path, "w");
	if (!fp)
		return -errno;
	if (fputs(value, fp) == EOF) {
		int err = errno ? -errno : -EIO;
		fclose(fp);
		return err;
	}
	if (fclose(fp) != 0)
		return -errno;
	return 0;
}

static int set_test_phase_state(const struct workload_options *opts,
				bool enabled, const char *phase)
{
	const char *value = enabled ? "1" : "0";
	int rc;

	if (!opts || !opts->test_phase_path || !opts->test_phase_path[0])
		return -EINVAL;

	rc = write_string_file(opts->test_phase_path, value);
	if (rc != 0) {
		fprintf(stderr,
			"[sqlite_init] failed to set test_phase '%s' for %s via %s (%d)\n",
			enabled ? "on" : "off",
			phase ? phase : "phase",
			opts->test_phase_path,
			rc);
		return rc;
	}

	printf("[sqlite_init] test_phase=%s phase=%s path=%s\n",
	       enabled ? "on" : "off",
	       phase ? phase : "phase",
	       opts->test_phase_path);
	return 0;
}

static void reset_die_stats(void)
{
	int rc = write_string_file(DIE_STATS_RESET_PATH, "1");

	if (rc != 0)
		fprintf(stderr, "[die_test] failed to reset die_stats (%d)\n", rc);
}

static void build_table_name(char *buf, size_t len, unsigned int table_id)
{
	snprintf(buf, len, "tbl_%02u", table_id);
}

static void build_table_db_path(char *buf, size_t len,
				const struct workload_options *opts,
				unsigned int table_id)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_tablefile_%s_tbl_%02u.db",
		 opts->tag ? opts->tag : DEFAULT_TAG, table_id);
	join_path(buf, len, TARGET_FOLDER, name);
}

static int open_refstyle_dummy_file(const struct workload_options *opts,
				    struct refstyle_dummy_state *dummy)
{
	char path[PATH_MAX];

	if (!dummy || opts->refstyle_dummy_bytes == 0)
		return 0;

	memset(dummy, 0, sizeof(*dummy));
	dummy->fd = -1;
	snprintf(path, sizeof(path), "%s/dummy_refstyle_%s.bin",
		 TARGET_FOLDER, opts->tag ? opts->tag : DEFAULT_TAG);
	dummy->fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_APPEND, 0666);
	if (dummy->fd < 0)
		return -errno;

	dummy->total_bytes_written = 0;
	if (posix_memalign(&dummy->buf, 4096, 4096) != 0) {
		close(dummy->fd);
		dummy->fd = -1;
		return -ENOMEM;
	}
	memset(dummy->buf, 0x5A, 4096);
	return 0;
}

static void close_refstyle_dummy_file(struct refstyle_dummy_state *dummy)
{
	if (!dummy)
		return;
	if (dummy->fd >= 0)
		close(dummy->fd);
	free(dummy->buf);
	memset(dummy, 0, sizeof(*dummy));
	dummy->fd = -1;
}

static int write_refstyle_dummy_bytes(struct refstyle_dummy_state *dummy,
				      unsigned long long bytes)
{
	unsigned long long left;

	if (!dummy || dummy->fd < 0 || bytes == 0)
		return 0;

	left = bytes;
	while (left > 0) {
		size_t chunk = left > 4096ULL ? 4096U : (size_t)left;
		ssize_t ret = write(dummy->fd, dummy->buf, chunk);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -EIO;
		left -= (unsigned long long)ret;
		dummy->total_bytes_written += (unsigned long long)ret;
	}
	if (fdatasync(dummy->fd) != 0)
		return -errno;
	return 0;
}

static void build_layout_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_layout_%s.meta",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_table_stats_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_table_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_row_stats_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_row_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_table_tier_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_table_tier_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_page_tier_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_page_tier_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_page_die_transition_csv_path(char *buf, size_t len,
					       const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_page_die_transition_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_table_die_path(char *buf, size_t len, const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_table_die_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static void build_table_die_transition_csv_path(char *buf, size_t len,
						const struct workload_options *opts)
{
	char name[128];

	snprintf(name, sizeof(name), "sqlite_table_die_transition_%s.csv",
		 opts->tag ? opts->tag : DEFAULT_TAG);
	join_path(buf, len, RESULT_FOLDER, name);
}

static int cmp_u64(const void *a, const void *b)
{
	const unsigned long long *ua = a;
	const unsigned long long *ub = b;

	if (*ua < *ub)
		return -1;
	if (*ua > *ub)
		return 1;
	return 0;
}

static bool page_tier_zone_is_fast(unsigned int zone)
{
	return zone == 0U || zone == 1U;
}

static int cmp_tier_entry_lpn(const void *a, const void *b)
{
	const struct page_tier_entry *ea = a;
	const struct page_tier_entry *eb = b;

	if (ea->lpn < eb->lpn)
		return -1;
	if (ea->lpn > eb->lpn)
		return 1;
	return 0;
}

static int cmp_transition_entry_lpn(const void *a, const void *b)
{
	const struct page_die_transition_entry *ea = a;
	const struct page_die_transition_entry *eb = b;

	if (ea->lpn < eb->lpn)
		return -1;
	if (ea->lpn > eb->lpn)
		return 1;
	return 0;
}

static int cmp_page_die_entry(const void *a, const void *b)
{
	const struct page_die_entry *ea = a;
	const struct page_die_entry *eb = b;

	if (ea->lpn < eb->lpn)
		return -1;
	if (ea->lpn > eb->lpn)
		return 1;
	return 0;
}

static int dataset_layout_build(const struct workload_options *opts,
				struct dataset_layout *layout)
{
	unsigned int table_count = opts->table_count ? opts->table_count : DEFAULT_TABLE_COUNT;
	unsigned int rows = opts->rows_per_table;
	unsigned long long total_rows = 0;

	memset(layout, 0, sizeof(*layout));
	if (!rows) {
		unsigned long long bytes = opts->target_bytes ? opts->target_bytes : DEFAULT_TARGET_BYTES;
		unsigned long long per_table = table_count ? bytes / table_count : bytes;

		rows = (unsigned int)(per_table / ROW_PAYLOAD_BYTES);
		if (rows == 0)
			rows = 1;
	}

	layout->rows_per_table = calloc(table_count, sizeof(unsigned int));
	layout->row_prefix = calloc(table_count + 1, sizeof(unsigned int));
	if (!layout->rows_per_table || !layout->row_prefix)
		return -ENOMEM;

	for (unsigned int i = 0; i < table_count; ++i) {
		layout->rows_per_table[i] = rows;
		layout->row_prefix[i] = (unsigned int)total_rows;
		total_rows += rows;
	}
	layout->row_prefix[table_count] = (unsigned int)total_rows;
	layout->table_count = table_count;
	layout->total_rows = (unsigned int)total_rows;
	return 0;
}

static void dataset_layout_destroy(struct dataset_layout *layout)
{
	if (!layout)
		return;
	free(layout->rows_per_table);
	free(layout->row_prefix);
	memset(layout, 0, sizeof(*layout));
}

static int dataset_layout_to_file(const char *path, const struct dataset_layout *layout)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;
	fprintf(fp, "table_count=%u\n", layout->table_count);
	fprintf(fp, "total_rows=%u\n", layout->total_rows);
	fprintf(fp, "table_rows=");
	for (unsigned int i = 0; i < layout->table_count; ++i) {
		fprintf(fp, "%u", layout->rows_per_table[i]);
		if (i + 1 < layout->table_count)
			fputc(',', fp);
	}
	fputc('\n', fp);
	fclose(fp);
	return 0;
}

static int load_page_die_entries(const char *path, struct page_die_entry **entries_out,
				 size_t *count_out, unsigned int *die_slots_out)
{
	FILE *fp;
	struct page_die_entry *entries = NULL;
	size_t count = 0;
	size_t cap = 0;
	unsigned int max_die = 0;
	char line[128];

	*entries_out = NULL;
	*count_out = 0;
	*die_slots_out = 0;
	if (!path || !*path)
		return 0;

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long lpn;
		unsigned int die;

		if (sscanf(line, "%llu %u", &lpn, &die) != 2)
			continue;
		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 4096;
			struct page_die_entry *tmp = realloc(entries, new_cap * sizeof(*entries));

			if (!tmp) {
				free(entries);
				fclose(fp);
				return -ENOMEM;
			}
			entries = tmp;
			cap = new_cap;
		}
		entries[count].lpn = lpn;
		entries[count].die = die;
		if (die > max_die)
			max_die = die;
		count++;
	}

	fclose(fp);
	if (!count) {
		free(entries);
		return 0;
	}

	qsort(entries, count, sizeof(*entries), cmp_page_die_entry);
	*entries_out = entries;
	*count_out = count;
	*die_slots_out = max_die + 1U;
	return 0;
}

static int load_page_tier_entries(const char *path, struct page_tier_entry **entries_out,
				  size_t *count_out)
{
	FILE *fp;
	struct page_tier_entry *entries = NULL;
	size_t count = 0;
	size_t cap = 0;
	char line[128];

	*entries_out = NULL;
	*count_out = 0;
	if (!path || !*path)
		return 0;

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long lpn;
		unsigned int in_slc;
		int qlc_zone = -1;
		int scanned;

		scanned = sscanf(line, "%llu %u %d", &lpn, &in_slc, &qlc_zone);
		if (scanned < 2)
			continue;
		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 4096;
			struct page_tier_entry *tmp = realloc(entries, new_cap * sizeof(*entries));

			if (!tmp) {
				free(entries);
				fclose(fp);
				return -ENOMEM;
			}
			entries = tmp;
			cap = new_cap;
		}

		entries[count].lpn = lpn;
		entries[count].in_slc = in_slc ? 1U : 0U;
		if (scanned >= 3 && qlc_zone >= 0 && qlc_zone < 4) {
			entries[count].qlc_zone = (unsigned int)qlc_zone;
			entries[count].qlc_zone_known = 1U;
		} else {
			entries[count].qlc_zone = 0U;
			entries[count].qlc_zone_known = 0U;
		}
		count++;
	}

	fclose(fp);
	if (!count) {
		free(entries);
		return 0;
	}

	qsort(entries, count, sizeof(*entries), cmp_tier_entry_lpn);
	{
		size_t unique = 1;
		for (size_t i = 1; i < count; ++i) {
			if (entries[i].lpn != entries[unique - 1].lpn)
				entries[unique++] = entries[i];
			else
				entries[unique - 1] = entries[i];
		}
		count = unique;
	}

	*entries_out = entries;
	*count_out = count;
	return 0;
}

static int load_page_die_transition_entries(const char *path,
					    struct page_die_transition_entry **entries_out,
					    size_t *count_out,
					    unsigned int *die_slots_out)
{
	FILE *fp;
	struct page_die_transition_entry *entries = NULL;
	size_t count = 0;
	size_t cap = 0;
	unsigned int max_die = 0;
	char line[160];

	*entries_out = NULL;
	*count_out = 0;
	*die_slots_out = 0;
	if (!path || !*path)
		return 0;

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long long lpn;
		int initial_die;
		unsigned int current_die;
		unsigned int die_changed;
		unsigned int reason;

		if (sscanf(line, "%llu %d %u %u %u",
			   &lpn, &initial_die, &current_die, &die_changed, &reason) != 5)
			continue;
		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 4096;
			struct page_die_transition_entry *tmp =
				realloc(entries, new_cap * sizeof(*entries));

			if (!tmp) {
				free(entries);
				fclose(fp);
				return -ENOMEM;
			}
			entries = tmp;
			cap = new_cap;
		}

		entries[count].lpn = lpn;
		entries[count].initial_die = initial_die;
		entries[count].current_die = current_die;
		entries[count].die_changed = die_changed ? 1U : 0U;
		entries[count].reason = reason;
		if (current_die > max_die)
			max_die = current_die;
		if (initial_die >= 0 && (unsigned int)initial_die > max_die)
			max_die = (unsigned int)initial_die;
		count++;
	}

	fclose(fp);
	if (!count) {
		free(entries);
		return 0;
	}

	qsort(entries, count, sizeof(*entries), cmp_transition_entry_lpn);
	{
		size_t unique = 1;
		for (size_t i = 1; i < count; ++i) {
			if (entries[i].lpn != entries[unique - 1].lpn)
				entries[unique++] = entries[i];
			else
				entries[unique - 1] = entries[i];
		}
		count = unique;
	}

	*entries_out = entries;
	*count_out = count;
	*die_slots_out = max_die + 1U;
	return 0;
}

static bool lookup_page_die(const struct page_die_entry *entries, size_t count,
			    unsigned long long lpn, unsigned int *die_out)
{
	size_t lo = 0;
	size_t hi = count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (entries[mid].lpn < lpn)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= count || entries[lo].lpn != lpn)
		return false;
	*die_out = entries[lo].die;
	return true;
}

static bool lookup_page_tier(const struct page_tier_entry *entries, size_t count,
			     unsigned long long lpn, unsigned int *in_slc_out,
			     unsigned int *qlc_zone_out, bool *qlc_zone_known_out)
{
	size_t lo = 0;
	size_t hi = count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (entries[mid].lpn < lpn)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= count || entries[lo].lpn != lpn)
		return false;
	if (in_slc_out)
		*in_slc_out = entries[lo].in_slc;
	if (qlc_zone_out)
		*qlc_zone_out = entries[lo].qlc_zone;
	if (qlc_zone_known_out)
		*qlc_zone_known_out = entries[lo].qlc_zone_known != 0U;
	return true;
}

static bool lookup_page_die_transition(const struct page_die_transition_entry *entries,
				       size_t count,
				       unsigned long long lpn,
				       struct page_die_transition_entry *entry_out)
{
	size_t lo = 0;
	size_t hi = count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (entries[mid].lpn < lpn)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= count || entries[lo].lpn != lpn)
		return false;
	if (entry_out)
		*entry_out = entries[lo];
	return true;
}

static unsigned long long detect_partition_offset_bytes(const char *path)
{
#if defined(__linux__)
	struct stat st;
	char sysfs[128];
	FILE *fp;
	unsigned long long start_sector = 0;

	if (!path || stat(path, &st) != 0)
		return 0;
	snprintf(sysfs, sizeof(sysfs), "/sys/dev/block/%u:%u/start",
		 major(st.st_dev), minor(st.st_dev));
	fp = fopen(sysfs, "r");
	if (!fp)
		return 0;
	if (fscanf(fp, "%llu", &start_sector) != 1)
		start_sector = 0;
	fclose(fp);
	return start_sector * 512ULL;
#else
	(void)path;
	return 0;
#endif
}

static int load_file_extents(const char *path, struct file_extent **extents_out,
			     size_t *extent_count_out)
{
#if defined(__linux__) && defined(FS_IOC_FIEMAP)
	int fd = -1;
	struct fiemap *fm = NULL;
	struct file_extent *extents = NULL;
	size_t extent_count = 0;
	uint32_t cap = 256;
	int rc = 0;

	*extents_out = NULL;
	*extent_count_out = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	while (1) {
		size_t bytes = sizeof(*fm) + (size_t)cap * sizeof(struct fiemap_extent);

		free(fm);
		fm = calloc(1, bytes);
		if (!fm) {
			rc = -ENOMEM;
			goto out;
		}
		fm->fm_start = 0;
		fm->fm_length = ~0ULL;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = cap;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			rc = -errno;
			goto out;
		}
		extent_count = fm->fm_mapped_extents;
		if (extent_count < cap ||
		    (extent_count > 0 && (fm->fm_extents[extent_count - 1].fe_flags & FIEMAP_EXTENT_LAST)))
			break;
		cap *= 2;
	}

	if (extent_count > 0) {
		extents = calloc(extent_count, sizeof(*extents));
		if (!extents) {
			rc = -ENOMEM;
			goto out;
		}
		for (size_t i = 0; i < extent_count; ++i) {
			extents[i].logical = fm->fm_extents[i].fe_logical;
			extents[i].physical = fm->fm_extents[i].fe_physical;
			extents[i].length = fm->fm_extents[i].fe_length;
		}
	}
	*extents_out = extents;
	*extent_count_out = extent_count;

out:
	free(fm);
	if (fd >= 0)
		close(fd);
	return rc;
#else
	(void)path;
	*extents_out = NULL;
	*extent_count_out = 0;
	return -ENOTSUP;
#endif
}

static int append_lpn_value(unsigned long long lpn, unsigned long long **vec,
			    size_t *count, size_t *cap)
{
	if (*count == *cap) {
		size_t new_cap = *cap ? *cap * 2 : 4096;
		unsigned long long *tmp = realloc(*vec, new_cap * sizeof(**vec));

		if (!tmp)
			return -ENOMEM;
		*vec = tmp;
		*cap = new_cap;
	}
	(*vec)[(*count)++] = lpn;
	return 0;
}

static int collect_file_lpns(const char *path, unsigned long long host_page_bytes,
			     unsigned long long **vec_out, size_t *count_out)
{
	struct file_extent *extents = NULL;
	size_t extent_count = 0;
	unsigned long long *vec = NULL;
	size_t count = 0;
	size_t cap = 0;
	struct stat st;
	int rc;

	*vec_out = NULL;
	*count_out = 0;
	if (stat(path, &st) != 0)
		return -errno;

	rc = load_file_extents(path, &extents, &extent_count);
	if (rc == 0 && extent_count > 0) {
		unsigned long long part_off = detect_partition_offset_bytes(path);

		for (size_t i = 0; i < extent_count; ++i) {
			unsigned long long phys = extents[i].physical + part_off;
			unsigned long long phys_end = phys + extents[i].length - 1ULL;
			unsigned long long lpn_start = phys / host_page_bytes;
			unsigned long long lpn_end = phys_end / host_page_bytes;

			for (unsigned long long lpn = lpn_start; lpn <= lpn_end; ++lpn) {
				rc = append_lpn_value(lpn, &vec, &count, &cap);
				if (rc != 0)
					goto out;
			}
		}
	} else {
		unsigned long long page_count =
			((unsigned long long)st.st_size + LOGICAL_PAGE_BYTES - 1ULL) / LOGICAL_PAGE_BYTES;

		for (unsigned long long lpn = 0; lpn < page_count; ++lpn) {
			rc = append_lpn_value(lpn, &vec, &count, &cap);
			if (rc != 0)
				goto out;
		}
	}

	if (count > 1) {
		qsort(vec, count, sizeof(*vec), cmp_u64);
		size_t unique = 1;
		for (size_t i = 1; i < count; ++i) {
			if (vec[i] != vec[unique - 1])
				vec[unique++] = vec[i];
		}
		count = unique;
	}

	*vec_out = vec;
	*count_out = count;
	vec = NULL;
	rc = 0;

out:
	free(vec);
	free(extents);
	return rc;
}

static int open_table_db(struct table_file_state *table, const struct workload_options *opts)
{
	char sql[256];
	int rc;

	build_table_db_path(table->db_path, sizeof(table->db_path), opts, table->table_id);
	unlink(table->db_path);

	rc = sqlite3_open(table->db_path, &table->db);
	if (rc != SQLITE_OK)
		return -EIO;

	sqlite3_exec(table->db, "PRAGMA journal_mode = off;", NULL, NULL, NULL);
	sqlite3_exec(table->db, "PRAGMA synchronous = on;", NULL, NULL, NULL);

	rc = sqlite3_exec(table->db,
			  "CREATE TABLE DB1(id INTEGER PRIMARY KEY,"
			  "str1 VARCHAR(7281), str2 VARCHAR(7281),"
			  "str3 VARCHAR(7281), str4 VARCHAR(7281));",
			  NULL, NULL, NULL);
	if (rc != SQLITE_OK)
		return -EIO;

	rc = sqlite3_prepare_v2(table->db,
				"INSERT INTO DB1 VALUES(?, ?, ?, ?, ?);",
				-1, &table->insert_stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;

	snprintf(sql, sizeof(sql),
		 "SELECT str1,str2,str3,str4 FROM DB1 WHERE id >= ? ORDER BY id;");
	rc = sqlite3_prepare_v2(table->db, sql, -1, &table->scan_stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;

	return 0;
}

static void close_table_db(struct table_file_state *table)
{
	if (!table)
		return;
	if (table->insert_stmt)
		sqlite3_finalize(table->insert_stmt);
	if (table->scan_stmt)
		sqlite3_finalize(table->scan_stmt);
	if (table->db)
		sqlite3_close(table->db);
	free(table->row_reads);
	free(table->row_latency);
	memset(table, 0, sizeof(*table));
}

static int get_db_page_count(sqlite3 *db, unsigned int *pages_out)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	rc = sqlite3_prepare_v2(db, "PRAGMA page_count;", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		return -EIO;
	}
	*pages_out = (unsigned int)sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return 0;
}

static int insert_rows_range(struct table_file_state *table, unsigned int start_row,
			     unsigned int rows_this_batch)
{
	char rstr1[STR1_LEN + 1];
	char rstr2[STR2_LEN + 1];
	char rstr3[STR3_LEN + 1];
	char rstr4[STR4_LEN + 1];

	for (unsigned int row = start_row; row < start_row + rows_this_batch; ++row) {
		int record_id = (int)(table->total_rows - 1U - row);
		int rc;

		random_string(rstr1, sizeof(rstr1));
		random_string(rstr2, sizeof(rstr2));
		random_string(rstr3, sizeof(rstr3));
		random_string(rstr4, sizeof(rstr4));

		sqlite3_reset(table->insert_stmt);
		sqlite3_clear_bindings(table->insert_stmt);
		sqlite3_bind_int(table->insert_stmt, 1, record_id);
		sqlite3_bind_text(table->insert_stmt, 2, rstr1, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(table->insert_stmt, 3, rstr2, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(table->insert_stmt, 4, rstr3, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(table->insert_stmt, 5, rstr4, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(table->insert_stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "Insert failed (table=%u row=%u): %s\n",
				table->table_id, row, sqlite3_errmsg(table->db));
			return -EIO;
		}
	}
	return 0;
}

static int insert_table_window_by_pages(struct table_file_state *table,
					unsigned int page_budget,
					const struct workload_options *opts,
					struct refstyle_dummy_state *dummy,
					unsigned int *rows_done_out,
					unsigned int *pages_delta_out)
{
	unsigned int start_pages = 0;
	unsigned int current_pages = 0;
	unsigned int rows_done = 0;
	unsigned int dummy_pages = 0;
	int rc;

	if (page_budget == 0)
		page_budget = 1;
	rc = get_db_page_count(table->db, &start_pages);
	if (rc != 0)
		return rc;
	current_pages = start_pages;

	if (opts->refstyle_dummy_bytes == 0) {
		rc = sqlite3_exec(table->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
	}

	if (opts->refstyle_dummy_bytes > 0)
		page_budget = 1;

	while (table->rows_inserted + rows_done < table->total_rows) {
		unsigned int prev_pages = current_pages;
		unsigned int sqlite_pages = 0;

		rc = insert_rows_range(table, table->rows_inserted + rows_done, 1);
		if (rc != 0) {
			if (opts->refstyle_dummy_bytes == 0)
				sqlite3_exec(table->db, "ROLLBACK;", NULL, NULL, NULL);
			return rc;
		}
		rows_done++;
		rc = get_db_page_count(table->db, &current_pages);
		if (rc != 0) {
			if (opts->refstyle_dummy_bytes == 0)
				sqlite3_exec(table->db, "ROLLBACK;", NULL, NULL, NULL);
			return rc;
		}
		sqlite_pages = current_pages >= prev_pages ? current_pages - prev_pages : 0U;
		if (opts->refstyle_dummy_bytes > 0) {
			unsigned long long dummy_bytes = refstyle_dummy_total_bytes(opts, sqlite_pages);

			rc = write_refstyle_dummy_bytes(dummy, dummy_bytes);
			if (rc != 0)
				return rc;
			dummy_pages += ceil_div_u64_to_u32(dummy_bytes,
							 opts->ftl_host_page_bytes ?
							 opts->ftl_host_page_bytes :
							 DEFAULT_FTL_HOST_PAGE_BYTES);
		}
		if (current_pages >= start_pages + page_budget)
			break;
		if (opts->refstyle_dummy_bytes > 0)
			break;
	}

	if (opts->refstyle_dummy_bytes == 0) {
		rc = sqlite3_exec(table->db, "COMMIT;", NULL, NULL, NULL);
		if (rc != SQLITE_OK)
			return -EIO;
	}

	*rows_done_out = rows_done;
	*pages_delta_out = (current_pages >= start_pages ? current_pages - start_pages : 0U) +
			   dummy_pages;
	return 0;
}

static unsigned int pick_table(unsigned int table_count, const struct workload_options *opts,
			       unsigned int *state)
{
	double mean, stddev, u;

	if (!strcasecmp(opts->dist_name, "zipf")) {
		u = rand_uniform(state);
		if (opts->zipf_alpha <= 0.0)
			return next_rand(state) % table_count;
		double norm = 0.0;
		double cumulative = 0.0;

		for (unsigned int i = 1; i <= table_count; ++i)
			norm += 1.0 / pow((double)i, opts->zipf_alpha);
		for (unsigned int i = 1; i <= table_count; ++i) {
			cumulative += (1.0 / pow((double)i, opts->zipf_alpha)) / norm;
			if (u <= cumulative)
				return i - 1;
		}
		return table_count - 1;
	}

	if (!strcasecmp(opts->dist_name, "exp") || !strcasecmp(opts->dist_name, "exponential")) {
		double lambda = opts->exp_lambda > 0.0 ? opts->exp_lambda : 0.0008;

		while (1) {
			u = rand_uniform(state);
			unsigned int idx = (unsigned int)floor(-log(1.0 - u) / lambda);
			if (idx < table_count)
				return idx;
		}
	}

	if (!strcasecmp(opts->dist_name, "normal")) {
		mean = opts->normal_mean >= 0.0 ? opts->normal_mean : ((double)table_count - 1.0) / 2.0;
		stddev = opts->normal_stddev > 0.0 ? opts->normal_stddev : (double)table_count / 6.0;
		while (1) {
			double u1 = rand_uniform(state);
			double u2 = rand_uniform(state);
			double z = sqrt(-2.0 * log(u1 > 1e-12 ? u1 : 1e-12)) * cos(2.0 * M_PI * u2);
			long idx = lround(mean + stddev * z);
			if (idx >= 0 && idx < (long)table_count)
				return (unsigned int)idx;
		}
	}

	return table_count ? (next_rand(state) % table_count) : 0U;
}

static int build_table_read_plan(const struct dataset_layout *layout,
				 const struct workload_options *opts,
				 unsigned int **plan_out)
{
	unsigned int *plan = calloc(layout->table_count, sizeof(*plan));
	unsigned int state = opts->seed ? opts->seed : 42U;

	if (!plan)
		return -ENOMEM;
	for (unsigned int i = 0; i < opts->interleave_reads; ++i)
		plan[pick_table(layout->table_count, opts, &state)]++;
	*plan_out = plan;
	return 0;
}

static void drop_page_cache(void)
{
	FILE *fp;

	sync();
	fp = fopen("/proc/sys/vm/drop_caches", "w");
	if (!fp)
		return;
	fputs("3\n", fp);
	fclose(fp);
}

static void drop_file_cache(const char *path)
{
#if defined(__linux__)
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);
#else
	(void)path;
#endif
}

static int run_read_event(unsigned int event_id,
			  const struct dataset_layout *layout,
			  struct table_file_state *tables,
			  const unsigned int *read_plan,
			  double *table_latency,
			  unsigned long long *table_read_ops,
			  double *elapsed_out)
{
	double start = monotonic_sec();

	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned int reads = read_plan ? read_plan[tbl] : 0U;
		struct table_file_state *table = &tables[tbl];
		double event_latency = 0.0;
		int lower_bound;

		if (reads == 0 || table->rows_inserted == 0)
			continue;

		lower_bound = (int)(table->total_rows - table->rows_inserted);
		if (lower_bound < 0)
			lower_bound = 0;
		drop_file_cache(table->db_path);

		for (unsigned int iter = 0; iter < reads; ++iter) {
			double t0 = monotonic_sec();
			int rc;

			sqlite3_reset(table->scan_stmt);
			sqlite3_clear_bindings(table->scan_stmt);
			sqlite3_bind_int(table->scan_stmt, 1, lower_bound);
			while ((rc = sqlite3_step(table->scan_stmt)) == SQLITE_ROW)
				;
			if (rc != SQLITE_DONE)
				return -EIO;
			event_latency += monotonic_sec() - t0;
			table_read_ops[tbl]++;
		}

		table_latency[tbl] += event_latency;
		for (unsigned int row = 0; row < table->rows_inserted; ++row) {
			table->row_reads[row] += reads;
			table->row_latency[row] += event_latency;
		}
	}

	*elapsed_out = monotonic_sec() - start;
	printf("[sqlite_init] read_event=%u completed tables=%u elapsed=%.6fs\n",
	       event_id, layout->table_count, *elapsed_out);
	return 0;
}

static void *full_scan_worker(void *arg)
{
	struct concurrent_read_ctx *ctx = arg;
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	double start = 0.0;
	char sql[256];

	if (sqlite3_open_v2(ctx->db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
		ctx->elapsed_sec = 0.0;
		return NULL;
	}

	snprintf(sql, sizeof(sql),
		 "SELECT str1,str2,str3,str4 FROM DB1 WHERE id >= ? AND id < ? ORDER BY id;");
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		ctx->elapsed_sec = 0.0;
		return NULL;
	}

	start = monotonic_sec();
	for (unsigned int rep = 0; rep < ctx->repeats; ++rep) {
		int rc;

		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_bind_int(stmt, 1, (int)ctx->record_id_begin);
		sqlite3_bind_int(stmt, 2, (int)ctx->record_id_end);
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
			ctx->rows_read++;
		if (rc != SQLITE_DONE)
			break;
	}

	ctx->elapsed_sec = monotonic_sec() - start;
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return NULL;
}

static double run_cold_full_scan_concurrent(const struct dataset_layout *layout,
					    struct table_file_state *tables,
					    unsigned int threads,
					    const unsigned int *read_plan,
					    double *cold_per_table,
					    unsigned long long *total_rows_out)
{
	pthread_t *tids = NULL;
	struct concurrent_read_ctx *ctxs = NULL;
	double wall_start;
	unsigned long long total_rows = 0;

	if (threads == 0)
		threads = 1;
	tids = calloc(threads, sizeof(*tids));
	ctxs = calloc(threads, sizeof(*ctxs));
	if (!tids || !ctxs) {
		free(tids);
		free(ctxs);
		return 0.0;
	}

	drop_page_cache();
	wall_start = monotonic_sec();

	for (unsigned int batch_begin = 0; batch_begin < layout->table_count; batch_begin += threads) {
		unsigned int launched = 0;

		for (unsigned int slot = 0; slot < threads; ++slot) {
			unsigned int tbl = batch_begin + slot;
			unsigned int row_count;
			unsigned int reads;

			if (tbl >= layout->table_count)
				break;

			row_count = layout->rows_per_table[tbl];
			reads = read_plan ? read_plan[tbl] : 1U;
			if (row_count == 0 || reads == 0)
				continue;

			drop_file_cache(tables[tbl].db_path);
			ctxs[launched].thread_id = launched;
			ctxs[launched].table_id = tbl;
			ctxs[launched].db_path = tables[tbl].db_path;
			ctxs[launched].record_id_begin = 0;
			ctxs[launched].record_id_end = row_count;
			ctxs[launched].repeats = reads;
			ctxs[launched].elapsed_sec = 0.0;
			ctxs[launched].rows_read = 0;
			printf("[sqlite_cold_fileparallel] batch=%u table=%u thread=%u ids=[%u,%u) repeats=%u\n",
			       batch_begin / threads, tbl, launched,
			       ctxs[launched].record_id_begin,
			       ctxs[launched].record_id_end,
			       ctxs[launched].repeats);
			pthread_create(&tids[launched], NULL, full_scan_worker, &ctxs[launched]);
			launched++;
		}

		for (unsigned int i = 0; i < launched; ++i) {
			unsigned int tbl = ctxs[i].table_id;

			pthread_join(tids[i], NULL);
			total_rows += ctxs[i].rows_read;
			cold_per_table[tbl] = ctxs[i].elapsed_sec;
			printf("[sqlite_cold_fileparallel] batch=%u table=%u thread=%u time=%.6fs rows=%llu\n",
			       batch_begin / threads, tbl, i,
			       ctxs[i].elapsed_sec, ctxs[i].rows_read);
		}
	}

	free(tids);
	free(ctxs);
	*total_rows_out = total_rows;
	return monotonic_sec() - wall_start;
}

static int write_row_stats_csv(const char *path, const struct dataset_layout *layout,
			       const struct table_file_state *tables)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;
	fprintf(fp, "table_id,table_name,row_index,logical_row_index,pages,reads,total_latency_sec,avg_latency_sec\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		char table_name[64];

		build_table_name(table_name, sizeof(table_name), tbl);
		for (unsigned int row = 0; row < tables[tbl].total_rows; ++row) {
			unsigned int logical = layout->row_prefix[tbl] + row;
			unsigned long long reads = tables[tbl].row_reads[row];
			double total_lat = tables[tbl].row_latency[row];
			double avg = reads ? total_lat / (double)reads : 0.0;

			fprintf(fp, "%u,%s,%u,%u,%u,%llu,%.9f,%.9f\n",
				tbl, table_name, row, logical, ROW_EST_PAGES,
				reads, total_lat, avg);
		}
	}
	fclose(fp);
	return 0;
}

static int write_table_stats_csv(const char *path, const struct dataset_layout *layout,
				 const double *table_latency,
				 const unsigned long long *table_read_ops)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;
	fprintf(fp, "table_id,table_name,hits,total_latency_sec,avg_latency_sec\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		char table_name[64];
		double avg = table_read_ops[tbl] ?
			table_latency[tbl] / (double)table_read_ops[tbl] : 0.0;

		build_table_name(table_name, sizeof(table_name), tbl);
		fprintf(fp, "%u,%s,%llu,%.9f,%.9f\n",
			tbl, table_name, table_read_ops[tbl], table_latency[tbl], avg);
	}
	fclose(fp);
	return 0;
}

static int write_table_die_csv(const char *path, const struct workload_options *opts,
			       const struct dataset_layout *layout,
			       const struct table_file_state *tables,
			       const struct page_die_entry *page_die_entries,
			       size_t page_die_count, unsigned int die_slots)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;

	fprintf(fp, "table_id,table_name,die,lpn_count,lpn_ratio\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned long long *lpns = NULL;
		size_t lpn_count = 0;
		unsigned long long *die_counts = NULL;
		char table_name[64];
		int rc;

		rc = collect_file_lpns(tables[tbl].db_path, opts->ftl_host_page_bytes, &lpns, &lpn_count);
		if (rc != 0)
			continue;
		die_counts = calloc(die_slots ? die_slots : 1U, sizeof(*die_counts));
		if (!die_counts) {
			free(lpns);
			fclose(fp);
			return -ENOMEM;
		}
		for (size_t i = 0; i < lpn_count; ++i) {
			unsigned int die;

			if (lookup_page_die(page_die_entries, page_die_count, lpns[i], &die) &&
			    die < die_slots)
				die_counts[die]++;
		}

		build_table_name(table_name, sizeof(table_name), tbl);
		for (unsigned int die = 0; die < die_slots; ++die) {
			double ratio;

			if (die_counts[die] == 0)
				continue;
			ratio = lpn_count ? (double)die_counts[die] / (double)lpn_count : 0.0;
			fprintf(fp, "%u,%s,%u,%llu,%.9f\n",
				tbl, table_name, die, die_counts[die], ratio);
		}

		free(die_counts);
		free(lpns);
	}
	fclose(fp);
	return 0;
}

static int write_table_tier_csv(const char *path, const struct workload_options *opts,
				const struct dataset_layout *layout,
				const struct table_file_state *tables,
				const struct page_die_entry *page_die_entries,
				size_t page_die_count, unsigned int die_slots,
				const double *cold_per_table)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;

	fprintf(fp,
		"table_id,table_name,distinct_ftl_lpn,distinct_die,dominant_die,dominant_die_lpn,"
		"dominant_die_lpn_ratio,effective_die_parallelism,cold_time_sec,cold_throughput_mb_s\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned long long *lpns = NULL;
		size_t lpn_count = 0;
		unsigned long long *die_counts = NULL;
		unsigned int distinct_die = 0;
		int dominant_die = -1;
		unsigned long long dominant_lpn = 0;
		double parallelism = 0.0;
		char table_name[64];
		int rc;

		rc = collect_file_lpns(tables[tbl].db_path, opts->ftl_host_page_bytes, &lpns, &lpn_count);
		if (rc != 0)
			continue;
		die_counts = calloc(die_slots ? die_slots : 1U, sizeof(*die_counts));
		if (!die_counts) {
			free(lpns);
			fclose(fp);
			return -ENOMEM;
		}

		for (size_t i = 0; i < lpn_count; ++i) {
			unsigned int die;

			if (lookup_page_die(page_die_entries, page_die_count, lpns[i], &die) &&
			    die < die_slots)
				die_counts[die]++;
		}

		double sq_sum = 0.0;
		for (unsigned int die = 0; die < die_slots; ++die) {
			if (die_counts[die] == 0)
				continue;
			distinct_die++;
			if (die_counts[die] > dominant_lpn) {
				dominant_lpn = die_counts[die];
				dominant_die = (int)die;
			}
			if (lpn_count > 0) {
				double p = (double)die_counts[die] / (double)lpn_count;
				sq_sum += p * p;
			}
		}
		if (sq_sum > 0.0)
			parallelism = 1.0 / sq_sum;

		build_table_name(table_name, sizeof(table_name), tbl);
		double tbl_mb = (double)layout->rows_per_table[tbl] * ROW_PAYLOAD_BYTES / (1024.0 * 1024.0);
		double cold_tp = cold_per_table[tbl] > 0.0 ? tbl_mb / cold_per_table[tbl] : 0.0;
		fprintf(fp, "%u,%s,%zu,%u,%d,%llu,%.9f,%.9f,%.9f,%.2f\n",
			tbl, table_name, lpn_count, distinct_die, dominant_die, dominant_lpn,
			lpn_count ? (double)dominant_lpn / (double)lpn_count : 0.0,
			parallelism, cold_per_table[tbl], cold_tp);
		free(die_counts);
		free(lpns);
	}
	fclose(fp);
	return 0;
}

static const char *page_tier_label(unsigned int in_slc, bool qlc_zone_known,
				   unsigned int qlc_zone)
{
	if (in_slc)
		return "slc";
	if (!qlc_zone_known)
		return "qlc-unknown";
	return page_tier_zone_is_fast(qlc_zone) ? "qlc-fast" : "qlc-slow";
}

static const char *die_change_reason_label(unsigned int reason)
{
	switch (reason) {
	case 0U:
		return "none";
	case 1U:
		return "host-append";
	case 2U:
		return "host-overwrite";
	case 3U:
		return "gc";
	case 4U:
		return "slc-to-qlc";
	case 5U:
		return "repromote";
	case 6U:
		return "qlc-rebalance";
	default:
		return "unknown";
	}
}

static int write_page_tier_csv(const char *path, const struct workload_options *opts,
			       const struct dataset_layout *layout,
			       const struct table_file_state *tables,
			       const struct page_tier_entry *tier_entries,
			       size_t tier_count,
			       const struct page_die_entry *page_die_entries,
			       size_t page_die_count)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;

	fprintf(fp,
		"table_id,table_name,lpn,die,die_known,in_slc,qlc_zone,qlc_zone_known,tier_label\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned long long *lpns = NULL;
		size_t lpn_count = 0;
		char table_name[64];
		int rc;

		rc = collect_file_lpns(tables[tbl].db_path, opts->ftl_host_page_bytes, &lpns, &lpn_count);
		if (rc != 0)
			continue;

		build_table_name(table_name, sizeof(table_name), tbl);
		for (size_t i = 0; i < lpn_count; ++i) {
			unsigned int die = 0;
			unsigned int in_slc = 0;
			unsigned int qlc_zone = 0;
			bool die_known = false;
			bool qlc_zone_known = false;
			bool tier_known = false;

			die_known = lookup_page_die(page_die_entries, page_die_count, lpns[i], &die);
			tier_known = lookup_page_tier(tier_entries, tier_count, lpns[i],
						      &in_slc, &qlc_zone, &qlc_zone_known);
			fprintf(fp, "%u,%s,%llu,%d,%u,%u,%d,%u,%s\n",
				tbl, table_name, lpns[i],
				die_known ? (int)die : -1, die_known ? 1U : 0U,
				tier_known ? in_slc : 0U,
				(tier_known && !in_slc && qlc_zone_known) ? (int)qlc_zone : -1,
				(tier_known && !in_slc && qlc_zone_known) ? 1U : 0U,
				tier_known ? page_tier_label(in_slc, qlc_zone_known, qlc_zone) : "unknown");
		}

		free(lpns);
	}

	fclose(fp);
	return 0;
}

static int write_page_die_transition_csv(const char *path,
					 const struct workload_options *opts,
					 const struct dataset_layout *layout,
					 const struct table_file_state *tables,
					 const struct page_die_transition_entry *transition_entries,
					 size_t transition_count,
					 const struct page_tier_entry *tier_entries,
					 size_t tier_count)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;

	fprintf(fp,
		"table_id,table_name,lpn,initial_die,initial_die_known,final_die,final_die_known,"
		"die_changed,change_reason,change_reason_label,in_slc,qlc_zone,qlc_zone_known,tier_label\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned long long *lpns = NULL;
		size_t lpn_count = 0;
		char table_name[64];
		int rc;

		rc = collect_file_lpns(tables[tbl].db_path, opts->ftl_host_page_bytes, &lpns, &lpn_count);
		if (rc != 0)
			continue;

		build_table_name(table_name, sizeof(table_name), tbl);
		for (size_t i = 0; i < lpn_count; ++i) {
			struct page_die_transition_entry transition;
			unsigned int in_slc = 0;
			unsigned int qlc_zone = 0;
			bool transition_known = false;
			bool initial_known = false;
			bool qlc_zone_known = false;
			bool tier_known = false;

			transition_known = lookup_page_die_transition(transition_entries, transition_count,
							      lpns[i], &transition);
			initial_known = transition_known && transition.initial_die >= 0;
			tier_known = lookup_page_tier(tier_entries, tier_count, lpns[i],
						      &in_slc, &qlc_zone, &qlc_zone_known);
			fprintf(fp, "%u,%s,%llu,%d,%u,%d,%u,%u,%u,%s,%u,%d,%u,%s\n",
				tbl, table_name, lpns[i],
				initial_known ? transition.initial_die : -1,
				initial_known ? 1U : 0U,
				transition_known ? (int)transition.current_die : -1,
				transition_known ? 1U : 0U,
				transition_known ? transition.die_changed : 0U,
				transition_known ? transition.reason : 0U,
				transition_known ? die_change_reason_label(transition.reason) : "unknown",
				tier_known ? in_slc : 0U,
				(tier_known && !in_slc && qlc_zone_known) ? (int)qlc_zone : -1,
				(tier_known && !in_slc && qlc_zone_known) ? 1U : 0U,
				tier_known ? page_tier_label(in_slc, qlc_zone_known, qlc_zone) : "unknown");
		}

		free(lpns);
	}

	fclose(fp);
	return 0;
}

static int write_table_die_transition_csv(const char *path,
					  const struct workload_options *opts,
					  const struct dataset_layout *layout,
					  const struct table_file_state *tables,
					  const struct page_die_transition_entry *transition_entries,
					  size_t transition_count,
					  unsigned int die_slots)
{
	FILE *fp = fopen(path, "w");

	if (!fp)
		return -errno;

	fprintf(fp,
		"table_id,table_name,initial_die,initial_die_known,final_die,page_count,page_ratio\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		unsigned long long *lpns = NULL;
		size_t lpn_count = 0;
		unsigned long long *matrix = NULL;
		unsigned int row_count = die_slots + 1U;
		char table_name[64];
		int rc;

		rc = collect_file_lpns(tables[tbl].db_path, opts->ftl_host_page_bytes, &lpns, &lpn_count);
		if (rc != 0)
			continue;

		matrix = calloc((size_t)row_count * die_slots, sizeof(*matrix));
		if (!matrix) {
			free(lpns);
			fclose(fp);
			return -ENOMEM;
		}

		for (size_t i = 0; i < lpn_count; ++i) {
			struct page_die_transition_entry transition;
			unsigned int initial_bucket;

			if (!lookup_page_die_transition(transition_entries, transition_count,
							lpns[i], &transition))
				continue;
			if (transition.current_die >= die_slots)
				continue;

			if (transition.initial_die >= 0 &&
			    (unsigned int)transition.initial_die < die_slots)
				initial_bucket = (unsigned int)transition.initial_die + 1U;
			else
				initial_bucket = 0U;

			matrix[(size_t)initial_bucket * die_slots + transition.current_die]++;
		}

		build_table_name(table_name, sizeof(table_name), tbl);
		for (unsigned int initial_bucket = 0; initial_bucket < row_count; ++initial_bucket) {
			for (unsigned int final_die = 0; final_die < die_slots; ++final_die) {
				unsigned long long count =
					matrix[(size_t)initial_bucket * die_slots + final_die];
				int initial_die = initial_bucket == 0U ?
					-1 : (int)(initial_bucket - 1U);

				if (count == 0)
					continue;
				fprintf(fp, "%u,%s,%d,%u,%u,%llu,%.9f\n",
					tbl, table_name, initial_die,
					initial_bucket == 0U ? 0U : 1U, final_die, count,
					lpn_count ? (double)count / (double)lpn_count : 0.0);
			}
		}

		free(matrix);
		free(lpns);
	}

	fclose(fp);
	return 0;
}

static void report_cold_tail_latency(const struct dataset_layout *layout,
				     const struct table_file_state *tables,
				     const double *table_latency,
				     const unsigned long long *table_read_ops)
{
	unsigned int count = layout->table_count;
	unsigned int tail = count / 10U;
	double avg = 0.0;
	unsigned int selected = 0;
	unsigned long long *heat_sum = calloc(count, sizeof(*heat_sum));
	unsigned int *order = calloc(count, sizeof(*order));

	if (!heat_sum || !order) {
		free(heat_sum);
		free(order);
		return;
	}
	if (tail == 0 && count > 0)
		tail = 1;
	for (unsigned int tbl = 0; tbl < count; ++tbl) {
		order[tbl] = tbl;
		for (unsigned int row = 0; row < tables[tbl].total_rows; ++row)
			heat_sum[tbl] += tables[tbl].row_reads[row];
	}
	for (unsigned int i = 0; i < count; ++i) {
		for (unsigned int j = i + 1; j < count; ++j) {
			if (heat_sum[order[j]] < heat_sum[order[i]]) {
				unsigned int tmp = order[i];
				order[i] = order[j];
				order[j] = tmp;
			}
		}
	}
	for (unsigned int i = 0; i < count && selected < tail; ++i) {
		unsigned int tbl = order[i];

		if (table_read_ops[tbl] == 0)
			continue;
		avg += table_latency[tbl] / (double)table_read_ops[tbl];
		selected++;
	}
	if (selected)
		avg /= (double)selected;
	printf("[sqlite_init] coldest_10pct_tables=%u avg_latency=%.6fs\n", selected, avg);
	free(heat_sum);
	free(order);
}

static int run_init_mode(const struct workload_options *opts)
{
	struct dataset_layout layout = {};
	struct table_file_state *tables = NULL;
	unsigned int *active = NULL;
	unsigned int *round_order = NULL;
	unsigned int *read_plan = NULL;
	double *table_latency = NULL;
	double *cold_per_table = NULL;
	unsigned long long *table_read_ops = NULL;
	struct page_die_entry *page_die_entries = NULL;
	struct page_tier_entry *page_tier_entries = NULL;
	struct page_die_transition_entry *page_die_transition_entries = NULL;
	size_t page_die_count = 0;
	size_t page_tier_count = 0;
	size_t page_die_transition_count = 0;
	unsigned int die_slots = 0;
	unsigned int transition_die_slots = 0;
	unsigned long long total_rows = 0;
	unsigned long long rows_written = 0;
	unsigned long long grown_pages_total = 0;
	unsigned long long next_read_event_pages;
	unsigned int active_count;
	unsigned int round_size = 0;
	unsigned int round_index = 0;
	unsigned int round_pass = 0;
	unsigned int window_cursor = 0;
	unsigned int read_events = 0;
	unsigned int effective_pages_per_row = ROW_EST_PAGES;
	double estimated_pages_per_table = 0.0;
	double estimated_rounds = 0.0;
	double total_read_time = 0.0;
	double cold_read_time = 0.0;
	unsigned long long cold_rows = 0;
	struct refstyle_dummy_state ref_dummy = { .fd = -1 };
	char layout_path[PATH_MAX];
	char row_stats_path[PATH_MAX];
	char table_stats_path[PATH_MAX];
	char table_tier_path[PATH_MAX];
	char table_die_path[PATH_MAX];
	char page_tier_csv_path[PATH_MAX];
	char page_die_transition_csv_path[PATH_MAX];
	char table_die_transition_csv_path[PATH_MAX];
	int rc = 0;
	bool test_phase_toggled = false;

	rc = dataset_layout_build(opts, &layout);
	if (rc != 0)
		return rc;

	tables = calloc(layout.table_count, sizeof(*tables));
	active = malloc(layout.table_count * sizeof(*active));
	table_latency = calloc(layout.table_count, sizeof(*table_latency));
	cold_per_table = calloc(layout.table_count, sizeof(*cold_per_table));
	table_read_ops = calloc(layout.table_count, sizeof(*table_read_ops));
	if (!tables || !active || !table_latency || !cold_per_table || !table_read_ops) {
		rc = -ENOMEM;
		goto out;
	}

	for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl) {
		tables[tbl].table_id = tbl;
		tables[tbl].total_rows = layout.rows_per_table[tbl];
		tables[tbl].row_reads = calloc(tables[tbl].total_rows ? tables[tbl].total_rows : 1U,
					       sizeof(unsigned long long));
		tables[tbl].row_latency = calloc(tables[tbl].total_rows ? tables[tbl].total_rows : 1U,
					       sizeof(double));
		if (!tables[tbl].row_reads || !tables[tbl].row_latency) {
			rc = -ENOMEM;
			goto out;
		}
		rc = open_table_db(&tables[tbl], opts);
		if (rc != 0)
			goto out;
		active[tbl] = tbl;
		total_rows += tables[tbl].total_rows;
	}

	active_count = layout.table_count;
	next_read_event_pages = opts->interleave_pages ? opts->interleave_pages : DEFAULT_INTERLEAVE_PAGES;

	build_layout_path(layout_path, sizeof(layout_path), opts);
	build_row_stats_path(row_stats_path, sizeof(row_stats_path), opts);
	build_table_stats_path(table_stats_path, sizeof(table_stats_path), opts);
	build_table_tier_path(table_tier_path, sizeof(table_tier_path), opts);
	build_table_die_path(table_die_path, sizeof(table_die_path), opts);
	build_page_tier_path(page_tier_csv_path, sizeof(page_tier_csv_path), opts);
	build_page_die_transition_csv_path(page_die_transition_csv_path,
					    sizeof(page_die_transition_csv_path), opts);
	build_table_die_transition_csv_path(table_die_transition_csv_path,
					     sizeof(table_die_transition_csv_path), opts);

	rc = build_table_read_plan(&layout, opts, &read_plan);
	if (rc != 0)
		goto out;
	rc = open_refstyle_dummy_file(opts, &ref_dummy);
	if (rc != 0) {
		fprintf(stderr, "Failed to open refstyle dummy file (%d)\n", rc);
		goto out;
	}

	if (opts->refstyle_dummy_bytes > 0)
		effective_pages_per_row += ceil_div_u64_to_u32(refstyle_dummy_total_bytes(opts, ROW_EST_PAGES),
							 opts->ftl_host_page_bytes ?
							 opts->ftl_host_page_bytes :
							 DEFAULT_FTL_HOST_PAGE_BYTES);

	estimated_pages_per_table =
		(double)layout.rows_per_table[0] * (double)effective_pages_per_row;
	{
		if (opts->refstyle_dummy_bytes > 0) {
			estimated_rounds = (double)layout.rows_per_table[0];
		} else {
			unsigned int pages_per_pass = opts->window_pages_per_table ?
				opts->window_pages_per_table : DEFAULT_WINDOW_PAGES_PER_TABLE;
			unsigned int passes_per_round = opts->window_passes_per_round ?
				opts->window_passes_per_round : DEFAULT_WINDOW_PASSES_PER_ROUND;
			double macro_pages_per_table = (double)pages_per_pass * (double)passes_per_round;

			estimated_rounds = macro_pages_per_table > 0.0 ?
				(estimated_pages_per_table / macro_pages_per_table) : 0.0;
		}
	}

	printf("[sqlite_init] config tables=%u total_rows=%llu logical_row_bytes=%llu est_row_pages=%u interleave_pages=%u "
	       "window_tables=%u window_pages_per_table=%u window_passes_per_round=%u read_ops_per_event=%u direct_io=%u multifile=1\n",
	       layout.table_count, total_rows, (unsigned long long)ROW_PAYLOAD_BYTES,
	       ROW_EST_PAGES, opts->interleave_pages, opts->window_tables,
	       opts->window_pages_per_table, opts->window_passes_per_round, opts->interleave_reads,
	       opts->direct_io ? 1U : 0U);
	printf("[sqlite_init] tablefile_pageflow=1 table_files=%u target=%llu rows_per_table=%u\n",
	       layout.table_count,
	       opts->target_bytes ? opts->target_bytes : DEFAULT_TARGET_BYTES,
	       layout.table_count ? layout.rows_per_table[0] : 0U);
	printf("[sqlite_init] cold_parallel_model=fileparallel one-thread-per-table-batch threads=%u\n",
	       opts->cold_concurrent_threads ? opts->cold_concurrent_threads : 1U);
	printf("[sqlite_init] refstyle_dummy_bytes=%llu align_pages=%u mode=%s\n",
	       opts->refstyle_dummy_bytes,
	       opts->align_pages,
	       opts->refstyle_dummy_bytes ?
	       "per-row-autocommit-dummy-row-roundrobin" : "window-transaction");
	printf("[sqlite_init] test_phase_path=%s\n",
	       opts->test_phase_path ? opts->test_phase_path : "(null)");
	if (opts->refstyle_dummy_bytes > 0) {
		printf("[sqlite_init] estimated_pages_per_table=%.0f estimated_rounds=%.2f "
		       "(one macro round = each active table grows by ~1 row = %u host pages: %u SQLite + %u dummy)\n",
		       estimated_pages_per_table, estimated_rounds,
		       effective_pages_per_row, ROW_EST_PAGES,
		       effective_pages_per_row > ROW_EST_PAGES ?
		       effective_pages_per_row - ROW_EST_PAGES : 0U);
	} else {
		printf("[sqlite_init] estimated_pages_per_table=%.0f estimated_rounds=%.2f "
		       "(one macro round = each active table grows by ~%u SQLite pages via %u passes x %u pages)\n",
		       estimated_pages_per_table, estimated_rounds,
		       (opts->window_pages_per_table ? opts->window_pages_per_table :
			DEFAULT_WINDOW_PAGES_PER_TABLE) *
		       (opts->window_passes_per_round ? opts->window_passes_per_round :
			DEFAULT_WINDOW_PASSES_PER_ROUND),
		       opts->window_passes_per_round ? opts->window_passes_per_round :
		       DEFAULT_WINDOW_PASSES_PER_ROUND,
		       opts->window_pages_per_table ? opts->window_pages_per_table :
		       DEFAULT_WINDOW_PAGES_PER_TABLE);
	}

	while (rows_written < total_rows) {
		unsigned int passes_per_round = opts->refstyle_dummy_bytes ?
			1U :
			(opts->window_passes_per_round ?
			 opts->window_passes_per_round : DEFAULT_WINDOW_PASSES_PER_ROUND);

		if (round_order && round_index >= round_size) {
			round_index = 0;
			round_pass++;
		}
		if (!round_order || round_pass >= passes_per_round) {
			free(round_order);
			round_size = opts->refstyle_dummy_bytes ?
				active_count :
				(opts->window_tables ? opts->window_tables : DEFAULT_WINDOW_TABLES);
			if (round_size > active_count)
				round_size = active_count;
			round_order = malloc(round_size * sizeof(*round_order));
			if (!round_order) {
				rc = -ENOMEM;
				goto out;
			}
			for (unsigned int i = 0; i < round_size; ++i)
				round_order[i] = active[(window_cursor + i) % active_count];
			window_cursor = active_count ? ((window_cursor + round_size) % active_count) : 0;
			round_index = 0;
			round_pass = 0;
		}

		unsigned int table_id = round_order[round_index++];
		struct table_file_state *table = &tables[table_id];
		unsigned int rows_step = 0;
		unsigned int pages_step = 0;

		if (table->rows_inserted >= table->total_rows)
			continue;

		rc = insert_table_window_by_pages(table,
						  opts->window_pages_per_table ? opts->window_pages_per_table :
						  DEFAULT_WINDOW_PAGES_PER_TABLE,
						  opts, &ref_dummy,
						  &rows_step, &pages_step);
		if (rc != 0)
			goto out;

		table->rows_inserted += rows_step;
		rows_written += rows_step;
		grown_pages_total += pages_step;

		if (table->rows_inserted >= table->total_rows) {
			for (unsigned int i = 0; i < active_count; ++i) {
				if (active[i] == table_id) {
					active[i] = active[active_count - 1];
					active_count--;
					break;
				}
			}
		}

		if ((next_read_event_pages && grown_pages_total >= next_read_event_pages) ||
		    rows_written == total_rows) {
			double event_elapsed = 0.0;

			read_events++;
			rc = run_read_event(read_events, &layout, tables, read_plan,
					    table_latency, table_read_ops, &event_elapsed);
			if (rc != 0)
				goto out;
			total_read_time += event_elapsed;
			while (next_read_event_pages && next_read_event_pages <= grown_pages_total)
				next_read_event_pages += opts->interleave_pages;
		}
	}

	rc = set_test_phase_state(opts, true, "cold-read-start");
	if (rc != 0)
		goto out;
	test_phase_toggled = true;
	reset_die_stats();

	cold_read_time = run_cold_full_scan_concurrent(&layout, tables,
						       opts->cold_concurrent_threads,
						       read_plan, cold_per_table, &cold_rows);

	rc = set_test_phase_state(opts, false, "cold-read-end");
	if (rc != 0)
		goto out;
	test_phase_toggled = false;

	rc = load_page_die_entries(opts->page_die_path, &page_die_entries, &page_die_count, &die_slots);
	if (rc != 0)
		fprintf(stderr, "warning: failed to load page_die entries (%d)\n", rc);
	rc = 0;
	rc = load_page_tier_entries(opts->page_tier_path, &page_tier_entries, &page_tier_count);
	if (rc != 0)
		fprintf(stderr, "warning: failed to load page_tier entries (%d)\n", rc);
	rc = 0;
	rc = load_page_die_transition_entries(opts->page_die_transition_path,
					      &page_die_transition_entries,
					      &page_die_transition_count,
					      &transition_die_slots);
	if (rc != 0)
		fprintf(stderr, "warning: failed to load page_die_transition entries (%d)\n", rc);
	rc = 0;
	if (transition_die_slots > die_slots)
		die_slots = transition_die_slots;

	write_row_stats_csv(row_stats_path, &layout, tables);
	write_table_stats_csv(table_stats_path, &layout, table_latency, table_read_ops);
	if (page_die_entries && die_slots > 0) {
		write_table_die_csv(table_die_path, opts, &layout, tables,
				    page_die_entries, page_die_count, die_slots);
		write_table_tier_csv(table_tier_path, opts, &layout, tables,
				     page_die_entries, page_die_count, die_slots,
				     cold_per_table);
	}
	write_page_tier_csv(page_tier_csv_path, opts, &layout, tables,
			   page_tier_entries, page_tier_count,
			   page_die_entries, page_die_count);
	if (page_die_transition_entries && die_slots > 0)
		write_table_die_transition_csv(table_die_transition_csv_path, opts, &layout, tables,
					       page_die_transition_entries,
					       page_die_transition_count, die_slots);
	if (page_die_transition_entries)
		write_page_die_transition_csv(page_die_transition_csv_path, opts, &layout, tables,
					      page_die_transition_entries,
					      page_die_transition_count,
					      page_tier_entries, page_tier_count);
	report_cold_tail_latency(&layout, tables, table_latency, table_read_ops);
	dataset_layout_to_file(layout_path, &layout);

	{
		double cold_mb = (double)cold_rows * ROW_PAYLOAD_BYTES / (1024.0 * 1024.0);
		double tp = cold_read_time > 0.0 ? cold_mb / cold_read_time : 0.0;
		double table_gib = (double)layout.total_rows * (double)ROW_PAYLOAD_BYTES /
				   (1024.0 * 1024.0 * 1024.0);
		double dummy_gib = (double)ref_dummy.total_bytes_written /
				   (1024.0 * 1024.0 * 1024.0);
		const char *table_die_transition_out = page_die_transition_entries ?
			table_die_transition_csv_path : "(unavailable)";
		const char *page_die_transition_out = page_die_transition_entries ?
			page_die_transition_csv_path : "(unavailable)";

		printf("[sqlite_init] table_bytes=%.2fGiB dummy_bytes=%.2fGiB combined_write_bytes=%.2fGiB\n",
		       table_gib, dummy_gib, table_gib + dummy_gib);
		printf("[sqlite_init] tag=%s tables=%u total_rows=%u read_events=%u interleaved_read_time=%.6fs "
		       "cold_full_read=%.6fs cold_full_read_tp=%.2fMB/s cold_mode=full-scan-concurrent multifile=1\n",
		       opts->tag, layout.table_count, layout.total_rows, read_events,
		       total_read_time, cold_read_time, tp);
		printf("[sqlite_init] row_stats=%s table_stats=%s table_tier=%s table_die=%s "
		       "page_tier=%s table_die_transition=%s page_die_transition=%s\n",
		       row_stats_path, table_stats_path, table_tier_path, table_die_path,
		       page_tier_csv_path, table_die_transition_out,
		       page_die_transition_out);
	}

out:
	if (test_phase_toggled)
		set_test_phase_state(opts, false, "cleanup");
	free(page_die_entries);
	free(page_tier_entries);
	free(page_die_transition_entries);
	free(read_plan);
	free(round_order);
	free(active);
	free(table_latency);
	free(cold_per_table);
	free(table_read_ops);
	close_refstyle_dummy_file(&ref_dummy);
	if (tables) {
		for (unsigned int i = 0; i < layout.table_count; ++i)
			close_table_db(&tables[i]);
		free(tables);
	}
	dataset_layout_destroy(&layout);
	return rc;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --mode init [options]\n"
		"  --table-count N\n"
		"  --rows-per-table N\n"
		"  --target-bytes SIZE\n"
		"  --refstyle-dummy-bytes SIZE\n"
		"  --align-pages N\n"
		"  --window-tables N\n"
		"  --window-pages-per-table N\n"
		"  --window-passes-per-round N\n"
		"  --interleave-pages N\n"
		"  --interleave-reads N\n"
		"  --cold-concurrent-threads N\n"
		"  --page-die-transition-path PATH\n"
		"  --tag TAG\n", prog);
}

static void configure_options(int argc, char **argv, struct workload_options *opts)
{
	int c;

	memset(opts, 0, sizeof(*opts));
	opts->table_count = DEFAULT_TABLE_COUNT;
	opts->rows_per_table = 0;
	opts->target_bytes = DEFAULT_TARGET_BYTES;
	opts->window_tables = DEFAULT_WINDOW_TABLES;
	opts->window_pages_per_table = DEFAULT_WINDOW_PAGES_PER_TABLE;
	opts->window_passes_per_round = DEFAULT_WINDOW_PASSES_PER_ROUND;
	opts->interleave_pages = DEFAULT_INTERLEAVE_PAGES;
	opts->interleave_reads = DEFAULT_INTERLEAVE_READS;
	opts->cold_concurrent_threads = 1U;
	opts->ftl_host_page_bytes = DEFAULT_FTL_HOST_PAGE_BYTES;
	opts->tag = DEFAULT_TAG;
	opts->page_tier_path = DEFAULT_PAGE_TIER_PATH;
	opts->page_die_path = DEFAULT_PAGE_DIE_PATH;
	opts->page_die_transition_path = DEFAULT_PAGE_DIE_TRANSITION_PATH;
	opts->seed = 42U;
	opts->dist_name = "normal";
	opts->zipf_alpha = 1.2;
	opts->exp_lambda = 0.0008;
	opts->normal_mean = -1.0;
	opts->normal_stddev = 8.0;
	opts->direct_io = false;
	opts->refstyle_dummy_bytes = DEFAULT_REFSTYLE_DUMMY_BYTES;
	opts->align_pages = 0;
	opts->test_phase_path = DEFAULT_TEST_PHASE_PATH;

	while ((c = getopt_long(argc, argv, "m:s:h", long_opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (strcasecmp(optarg, "init") != 0) {
				fprintf(stderr, "Only --mode init is supported in tablefile_pageflow\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 's':
			opts->seed = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1000:
			opts->tag = optarg;
			break;
		case 1001:
			opts->table_count = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1002:
			opts->rows_per_table = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1003:
			opts->target_bytes = parse_size_arg(optarg);
			break;
		case 1004:
			opts->window_tables = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1005:
			opts->window_pages_per_table = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1028:
			opts->window_passes_per_round = (unsigned int)strtoul(optarg, NULL, 10);
			if (opts->window_passes_per_round == 0)
				opts->window_passes_per_round = DEFAULT_WINDOW_PASSES_PER_ROUND;
			break;
		case 1006:
			opts->interleave_pages = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1007:
			opts->interleave_reads = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1008:
			opts->cold_concurrent_threads = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1009:
			opts->ftl_host_page_bytes = parse_size_arg(optarg);
			if (opts->ftl_host_page_bytes == 0)
				opts->ftl_host_page_bytes = DEFAULT_FTL_HOST_PAGE_BYTES;
			break;
		case 1010:
			opts->page_die_path = optarg;
			break;
		case 1030:
			opts->page_die_transition_path = optarg;
			break;
		case 1011:
			opts->dist_name = optarg;
			break;
		case 1012:
		case 1013:
		case 1014:
			break;
		case 1015:
			opts->zipf_alpha = strtod(optarg, NULL);
			break;
		case 1016:
			opts->exp_lambda = strtod(optarg, NULL);
			break;
		case 1017:
			opts->normal_mean = strtod(optarg, NULL);
			break;
		case 1018:
			opts->normal_stddev = strtod(optarg, NULL);
			break;
		case 1019:
		case 1020:
		case 1021:
			break;
		case 1022:
			opts->test_phase_path = optarg;
			break;
		case 1023:
			opts->page_tier_path = optarg;
			break;
		case 1024:
			break;
		case 1025:
			opts->direct_io = true;
			break;
		case 1026:
			break;
		case 1027:
			if (!strcmp(optarg, "0")) {
				opts->refstyle_dummy_bytes = 0;
				break;
			}
			opts->refstyle_dummy_bytes = parse_size_arg(optarg);
			if (opts->refstyle_dummy_bytes == 0)
				opts->refstyle_dummy_bytes = DEFAULT_REFSTYLE_DUMMY_BYTES;
			break;
		case 1029:
			opts->align_pages = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'h':
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char **argv)
{
	struct workload_options opts;

	configure_options(argc, argv, &opts);
	if (ensure_directory(TARGET_FOLDER) != 0 || ensure_directory(RESULT_FOLDER) != 0) {
		fprintf(stderr, "Failed to create target/result directories\n");
		return EXIT_FAILURE;
	}
	srand(opts.seed ? opts.seed : (unsigned int)time(NULL));
	return run_init_mode(&opts) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
