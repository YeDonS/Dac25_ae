#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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

#define ROW_PAYLOAD_BYTES        262144ULL
#define DEFAULT_TABLE_COUNT      1U
#define DEFAULT_TARGET_BYTES     (10ULL << 30)
#define DEFAULT_READS            5000U
#define TRACE_HEADER_PREFIX      "# sqlite_trace v1"
#define DB_FILENAME_FMT          "sqlite_dataset_%s.db"
#define LAYOUT_FILENAME_FMT      "sqlite_layout_%s.meta"
#define TRACE_FILENAME_FMT       "sqlite_trace_%s_%s.trace"
#define HEAT_FILENAME_FMT        "sqlite_heat_%s.csv"
#define ROW_STATS_FILENAME_FMT   "sqlite_row_%s.csv"
#define TABLE_STATS_FILENAME_FMT "sqlite_table_%s.csv"
#define DEFAULT_TAG              "default"
#define MAX_TABLE_NAME           64
#define NORMAL_MEAN_SENTINEL     (-1.0)
#define NORMAL_STDDEV_SENTINEL   (-1.0)
#define STR1_LEN                 65536
#define STR2_LEN                 65536
#define STR3_LEN                 65536
#define STR4_LEN                 65534
#define SCAN_ITER_DEFAULT        10U
#define LOGICAL_PAGE_BYTES       4096ULL
#define LPN_PER_ROW              (ROW_PAYLOAD_BYTES / LOGICAL_PAGE_BYTES)
#define DEFAULT_INTERLEAVE_ROWS  1000U
#define DEFAULT_READS_PER_EVENT  1000U
#define DEFAULT_COLD_SCAN_ITERS  3U

typedef unsigned int UINT32;

static const struct option long_opts[] = {
	{"mode", required_argument, NULL, 'm'},
	{"distribution", required_argument, NULL, 'd'},
	{"reads", required_argument, NULL, 'r'},
	{"alpha", required_argument, NULL, 'a'},
	{"lambda", required_argument, NULL, 'l'},
	{"seed", required_argument, NULL, 's'},
	{"zipf-seed", required_argument, NULL, 1000},
	{"exp-seed", required_argument, NULL, 1001},
	{"normal-seed", required_argument, NULL, 1002},
	{"log", required_argument, NULL, 'o'},
	{"heatmap", required_argument, NULL, 'H'},
	{"human-log", no_argument, NULL, 'U'},
	{"normal-mean", required_argument, NULL, 'M'},
	{"normal-stddev", required_argument, NULL, 'S'},
	{"table-count", required_argument, NULL, 1003},
	{"rows-per-table", required_argument, NULL, 1004},
	{"target-bytes", required_argument, NULL, 1005},
	{"tag", required_argument, NULL, 1006},
	{"trace-dir", required_argument, NULL, 1007},
	{"trace-mode", required_argument, NULL, 1008},
	{"trace-path", required_argument, NULL, 1009},
	{"suppress-report", no_argument, NULL, 1010},
	{"scan-iters", required_argument, NULL, 1011},
	{"interleave-rows", required_argument, NULL, 1012},
	{"interleave-reads", required_argument, NULL, 1013},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

enum workload_mode {
	MODE_INIT = 0,
	MODE_READ,
	MODE_SCAN,
};

enum distribution_type {
	DIST_UNIFORM = 0,
	DIST_ZIPF,
	DIST_EXPONENTIAL,
	DIST_NORMAL,
	DIST_SEQUENTIAL,
};

enum trace_mode {
	TRACE_MODE_NONE = 0,
	TRACE_MODE_RECORD,
	TRACE_MODE_REPLAY,
};

struct workload_options {
	enum workload_mode mode;
	enum distribution_type dist;
	unsigned int reads;
	double zipf_alpha;
	double exp_lambda;
	unsigned int seed;
	unsigned int zipf_seed;
	unsigned int exp_seed;
	unsigned int normal_seed;
	const char *log_path;
	const char *heatmap_path;
	bool human_log;
	double normal_mean;
	double normal_stddev;
	unsigned int table_count;
	unsigned int rows_per_table;
	unsigned long long target_bytes;
	const char *tag;
	const char *trace_dir;
	enum trace_mode trace_mode;
	const char *trace_path;
	char trace_path_buf[PATH_MAX];
	bool suppress_report;
	unsigned int scan_iters;
	bool reads_explicit;
	unsigned int interleave_rows;
	unsigned int init_reads_per_event;
};

struct zipf_sampler {
	double *cdf;
	unsigned int length;
	double alpha;
};

struct pragma_snapshot {
	char journal_mode[32];
	char synchronous[32];
};

struct text_slot {
	char *buf;
	size_t len;
};

struct dataset_layout {
	unsigned int table_count;
	unsigned int total_rows;
	unsigned int max_rows_per_table;
	unsigned int *rows_per_table;
	unsigned int *row_prefix; /* length = table_count + 1 */
};

struct request_sequence {
	unsigned int *logical_idx;
	unsigned int count;
};

struct table_state {
	unsigned int table_id;
	unsigned int total_rows;
	unsigned int rows_inserted;
	unsigned long long *row_reads;
	double *row_latency;
	sqlite3_stmt *insert_stmt;
	sqlite3_stmt *select_stmt;
};

static void join_path(char *dst, size_t len, const char *dir, const char *leaf);
static const char *effective_tag(const struct workload_options *opts);
static int build_trace_with_heat(enum distribution_type dist, unsigned int reads,
				 const struct workload_options *opts,
				 const struct dataset_layout *layout,
				 unsigned int *heat);
static void build_db_path(char *buf, size_t len, const struct workload_options *opts);
static void build_layout_path(char *buf, size_t len, const struct workload_options *opts);
static void build_trace_path_for_dist(char *buf, size_t len,
				      const struct workload_options *opts,
				      enum distribution_type dist);
static void build_heat_path(char *buf, size_t len, const struct workload_options *opts);
static void build_row_stats_path(char *buf, size_t len, const struct workload_options *opts);
static void build_table_stats_path(char *buf, size_t len, const struct workload_options *opts);
static void drop_page_cache(void);


static int save_heat_csv(const char *path, const unsigned int *heat, unsigned int total_rows)
{
	FILE *fp = fopen(path, "w");

	if (!fp) {
		perror("fopen heat csv");
		return -1;
	}

	fprintf(fp, "logical_index,heat\n");
	for (unsigned int i = 0; i < total_rows; ++i)
		fprintf(fp, "%u,%u\n", i, heat[i]);

	fclose(fp);
	return 0;
}


static double monotonic_sec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		perror("clock_gettime");
		return 0.0;
	}
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void drop_page_cache(void)
{
	int rc = system("sync");
	(void)rc;
	FILE *fp = fopen("/proc/sys/vm/drop_caches", "w");
	if (!fp) {
		perror("drop_caches");
		return;
	}
	if (fputs("3\n", fp) < 0)
		perror("drop_caches write");
	fclose(fp);
}

static unsigned int next_rand(unsigned int *state)
{
	return rand_r(state);
}

static double rand_uniform(unsigned int *state)
{
	return (double)next_rand(state) / (double)RAND_MAX;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --mode init [--table-count N] [--rows-per-table N]\n"
		"                 [--target-bytes SZ] [--reads N]\n"
		"                 [--interleave-rows N] [--interleave-reads N]\n"
		"                 [--zipf-seed S] [--exp-seed S] [--normal-seed S]\n"
		"                 [--tag NAME] [--trace-dir DIR]\n"
		"  %s --mode read --distribution zipf|exp|uniform|normal|sequential\n"
		"                 [--reads N] [--alpha A] [--lambda L]\n"
		"                 [--seed S] [--trace-mode record|replay]\n"
		"                 [--trace-path PATH] [--log path] [--heatmap path]\n"
		"                 [--human-log] [--suppress-report]\n"
		"  %s --mode scan [--scan-iters N]\n",
		prog, prog, prog);
}

static void copy_text(char *dst, size_t len, const char *src)
{
	if (len == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, len - 1);
	dst[len - 1] = '\0';
}

static void random_string(char *buffer, size_t len)
{
	for (size_t i = 0; i < len; ++i)
		buffer[i] = (char)('a' + (rand() % 26));
	buffer[len] = '\0';
}

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

static const char *dist_name(enum distribution_type dist)
{
	switch (dist) {
	case DIST_UNIFORM:
		return "uniform";
	case DIST_ZIPF:
		return "zipf";
	case DIST_EXPONENTIAL:
		return "exp";
	case DIST_NORMAL:
		return "normal";
	case DIST_SEQUENTIAL:
		return "sequential";
	default:
		return "unknown";
	}
}

static enum distribution_type parse_distribution(const char *arg)
{
	if (strcasecmp(arg, "zipf") == 0 || strcasecmp(arg, "zipfian") == 0)
		return DIST_ZIPF;
	if (strcasecmp(arg, "exp") == 0 || strcasecmp(arg, "exponential") == 0)
		return DIST_EXPONENTIAL;
	if (strcasecmp(arg, "uniform") == 0)
		return DIST_UNIFORM;
	if (strcasecmp(arg, "normal") == 0 || strcasecmp(arg, "gaussian") == 0)
		return DIST_NORMAL;
	if (strcasecmp(arg, "sequential") == 0 || strcasecmp(arg, "seq") == 0)
		return DIST_SEQUENTIAL;

	fprintf(stderr, "Unknown distribution '%s'\n", arg);
	exit(EXIT_FAILURE);
}

static unsigned long long parse_size_arg(const char *arg)
{
	char *end = NULL;
	unsigned long long value;
	unsigned long long multiplier = 1;

	errno = 0;
	value = strtoull(arg, &end, 10);
	if (errno != 0) {
		perror("strtoull");
		exit(EXIT_FAILURE);
	}

	if (end && *end) {
		while (*end && isspace((unsigned char)*end))
			end++;
		if (*end) {
			switch (tolower((unsigned char)*end)) {
			case 'k':
				multiplier = 1ULL << 10;
				break;
			case 'm':
				multiplier = 1ULL << 20;
				break;
			case 'g':
				multiplier = 1ULL << 30;
				break;
			case 't':
				multiplier = 1ULL << 40;
				break;
			default:
				fprintf(stderr, "Unknown size suffix '%c'\n", *end);
				exit(EXIT_FAILURE);
			}
		}
	}

	return value * multiplier;
}

static const char *effective_tag(const struct workload_options *opts)
{
	if (opts->tag && opts->tag[0])
		return opts->tag;
	return DEFAULT_TAG;
}

static void join_path(char *dst, size_t len, const char *dir, const char *leaf)
{
	if (!dir || !*dir) {
		snprintf(dst, len, "%s", leaf ? leaf : "");
		return;
	}

	size_t dlen = strlen(dir);
	if (dir[dlen - 1] == '/')
		snprintf(dst, len, "%s%s", dir, leaf ? leaf : "");
	else
		snprintf(dst, len, "%s/%s", dir, leaf ? leaf : "");
}


static void build_default_trace_path(char *buf, size_t len, const struct workload_options *opts)
{
	build_trace_path_for_dist(buf, len, opts, opts->dist);
}

static unsigned int resolve_effective_reads(const struct workload_options *opts,
					    unsigned int total_rows)
{
	if (opts->reads_explicit && opts->reads > 0)
		return opts->reads;

	if (total_rows > 0)
		return total_rows;

	return opts->reads ? opts->reads : DEFAULT_READS;
}

static int ensure_directory(const char *path)
{
	struct stat st;

	if (!path || !*path)
		return 0;

	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return 0;
		errno = ENOTDIR;
		return -1;
	}

	if (mkdir(path, 0755) == 0)
		return 0;

	if (errno == EEXIST)
		return 0;

	return -1;
}

static void build_table_name(char *buf, size_t len, unsigned int table_id)
{
	snprintf(buf, len, "tbl_%02u", table_id);
}

static int init_zipf_sampler(struct zipf_sampler *zp, unsigned int n, double alpha)
{
	double norm = 0.0;

	if (alpha <= 0.0) {
		fprintf(stderr, "zipf alpha must be > 0\n");
		return -EINVAL;
	}

	zp->cdf = calloc(n, sizeof(double));
	if (!zp->cdf)
		return -ENOMEM;

	zp->length = n;
	zp->alpha = alpha;

	for (unsigned int k = 1; k <= n; ++k)
		norm += 1.0 / pow((double)k, alpha);

	double cumulative = 0.0;
	for (unsigned int k = 1; k <= n; ++k) {
		cumulative += (1.0 / pow((double)k, alpha)) / norm;
		zp->cdf[k - 1] = cumulative;
	}

	zp->cdf[n - 1] = 1.0;
	return 0;
}

static void destroy_zipf_sampler(struct zipf_sampler *zp)
{
	free(zp->cdf);
	zp->cdf = NULL;
	zp->length = 0;
}

static unsigned int sample_zipf(const struct zipf_sampler *zp, double u)
{
	unsigned int lo = 0, hi = zp->length - 1, mid;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (u <= zp->cdf[mid])
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

static unsigned int sample_exponential(unsigned int n, double lambda, unsigned int *state)
{
	if (lambda <= 0.0)
		lambda = 0.0008;

	while (1) {
		double u = rand_uniform(state);
		if (u == 1.0)
			continue;
		unsigned int idx = (unsigned int)floor(-log(1.0 - u) / lambda);
		if (idx < n)
			return idx;
	}
}

static double sample_standard_normal(unsigned int *state)
{
	double u1, u2;

	do {
		u1 = rand_uniform(state);
	} while (u1 <= 1e-12);
	u2 = rand_uniform(state);

	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static unsigned int sample_normal(unsigned int n, double mean, double stddev, unsigned int *state)
{
	if (stddev <= 0.0)
		stddev = (double)n / 6.0;
	if (mean < 0.0 || mean >= (double)n)
		mean = ((double)n - 1.0) / 2.0;

	while (1) {
		double z = sample_standard_normal(state);
		long candidate = lround(mean + stddev * z);
		if (candidate >= 0 && candidate < (long)n)
			return (unsigned int)candidate;
	}
}

static void dataset_layout_destroy(struct dataset_layout *layout)
{
	if (!layout)
		return;

	free(layout->rows_per_table);
	free(layout->row_prefix);
	memset(layout, 0, sizeof(*layout));
}

static int dataset_layout_build(struct dataset_layout *layout, unsigned int table_count,
				const unsigned int *counts)
{
	unsigned int total = 0;
	unsigned int max_rows = 0;

	layout->rows_per_table = calloc(table_count, sizeof(unsigned int));
	layout->row_prefix = calloc(table_count + 1, sizeof(unsigned int));
	if (!layout->rows_per_table || !layout->row_prefix) {
		dataset_layout_destroy(layout);
		return -ENOMEM;
	}

	for (unsigned int i = 0; i < table_count; ++i) {
		layout->rows_per_table[i] = counts[i];
		layout->row_prefix[i] = total;
		total += counts[i];
		if (counts[i] > max_rows)
			max_rows = counts[i];
	}
	layout->row_prefix[table_count] = total;

	layout->table_count = table_count;
	layout->total_rows = total;
	layout->max_rows_per_table = max_rows;
	return 0;
}

static int dataset_layout_from_options(const struct workload_options *opts,
				       struct dataset_layout *layout)
{
	unsigned int table_count = opts->table_count ? opts->table_count : DEFAULT_TABLE_COUNT;
	unsigned int *counts = NULL;
	unsigned long long target_rows;
	int rc = 0;

	if (opts->rows_per_table) {
		target_rows = (unsigned long long)opts->rows_per_table * table_count;
	} else {
		unsigned long long bytes = opts->target_bytes ? opts->target_bytes : DEFAULT_TARGET_BYTES;
		target_rows = bytes / ROW_PAYLOAD_BYTES;
	}

	if (target_rows < table_count)
		target_rows = table_count;

	unsigned int base = (unsigned int)(target_rows / table_count);
	unsigned int extra = (unsigned int)(target_rows % table_count);

	if (base == 0)
		base = 1;

	counts = calloc(table_count, sizeof(unsigned int));
	if (!counts)
		return -ENOMEM;

	for (unsigned int i = 0; i < table_count; ++i)
		counts[i] = base + (i < extra ? 1 : 0);

	rc = dataset_layout_build(layout, table_count, counts);
	free(counts);
	return rc;
}

static int dataset_layout_from_file(const char *path, struct dataset_layout *layout)
{
	FILE *fp = fopen(path, "r");
	char line[4096];
	unsigned int table_count = 0;
	unsigned int total_rows = 0;
	char *rows_line = NULL;
	int rc = 0;

	if (!fp) {
		perror("fopen layout");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "table_count=", 12) == 0) {
			table_count = (unsigned int)strtoul(line + 12, NULL, 10);
		} else if (strncmp(line, "total_rows=", 11) == 0) {
			total_rows = (unsigned int)strtoul(line + 11, NULL, 10);
		} else if (strncmp(line, "table_rows=", 11) == 0) {
			free(rows_line);
			rows_line = strdup(line + 11);
		}
	}
	fclose(fp);

	if (!table_count || !rows_line) {
		fprintf(stderr, "Invalid layout metadata %s\n", path);
		free(rows_line);
		return -1;
	}

	unsigned int *counts = calloc(table_count, sizeof(unsigned int));
	if (!counts) {
		free(rows_line);
		return -ENOMEM;
	}

	char *token;
	char *saveptr = NULL;
	unsigned int idx = 0;

	for (token = strtok_r(rows_line, ",\n", &saveptr);
	     token && idx < table_count;
	     token = strtok_r(NULL, ",\n", &saveptr), idx++)
		counts[idx] = (unsigned int)strtoul(token, NULL, 10);

	if (idx != table_count) {
		fprintf(stderr, "Layout rows mismatch\n");
		free(rows_line);
		free(counts);
		return -1;
	}

	free(rows_line);
	rc = dataset_layout_build(layout, table_count, counts);
	free(counts);

	if (rc != 0)
		return rc;

	if (total_rows && layout->total_rows != total_rows) {
		fprintf(stderr, "Layout total rows mismatch (meta=%u actual=%u)\n",
			total_rows, layout->total_rows);
		dataset_layout_destroy(layout);
		return -1;
	}

	return 0;
}

static int dataset_layout_to_file(const char *path, const struct dataset_layout *layout)
{
	FILE *fp = fopen(path, "w");

	if (!fp) {
		perror("fopen layout");
		return -1;
	}

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

static int layout_lookup(const struct dataset_layout *layout, unsigned int logical_idx,
			 unsigned int *table_id, unsigned int *row_idx, int *record_id)
{
	if (logical_idx >= layout->total_rows)
		return -ERANGE;

	unsigned int lo = 0;
	unsigned int hi = layout->table_count;

	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;
		unsigned int start = layout->row_prefix[mid];
		unsigned int end = layout->row_prefix[mid + 1];

		if (logical_idx < start)
			hi = mid;
		else if (logical_idx >= end)
			lo = mid + 1;
		else {
			lo = mid;
			break;
		}
	}

	unsigned int tbl = lo;
	unsigned int base = layout->row_prefix[tbl];
	unsigned int within = logical_idx - base;

	if (table_id)
		*table_id = tbl;
	if (row_idx)
		*row_idx = within;
	if (record_id)
		*record_id = (int)(layout->rows_per_table[tbl] - 1 - within);

	return 0;
}

static unsigned int effective_seed(const struct workload_options *opts,
				   enum distribution_type dist)
{
	switch (dist) {
	case DIST_ZIPF:
		return opts->zipf_seed ? opts->zipf_seed : opts->seed;
	case DIST_EXPONENTIAL:
		return opts->exp_seed ? opts->exp_seed : opts->seed;
	case DIST_NORMAL:
		return opts->normal_seed ? opts->normal_seed : opts->seed;
	case DIST_SEQUENTIAL:
	case DIST_UNIFORM:
	default:
		return opts->seed;
	}
}

static void resolve_normal_params(const struct dataset_layout *layout,
				  const struct workload_options *opts,
				  double *mean_out, double *stddev_out)
{
	double mean = opts->normal_mean;
	double stddev = opts->normal_stddev;
	double fallback_mean = ((double)layout->total_rows - 1.0) / 2.0;
	double fallback_stddev = (double)layout->total_rows / 6.0;

	if (mean <= NORMAL_MEAN_SENTINEL || mean >= (double)layout->total_rows || mean < 0.0)
		mean = fallback_mean;
	if (stddev <= NORMAL_STDDEV_SENTINEL)
		stddev = fallback_stddev;

	*mean_out = mean;
	*stddev_out = stddev;
}

static int fill_request_sequence(struct request_sequence *seq,
				 const struct dataset_layout *layout,
				 const struct workload_options *opts)
{
	unsigned int seed = effective_seed(opts, opts->dist);
	struct zipf_sampler zipf = {};
	bool zipf_initialized = false;
	unsigned int total_rows = layout->total_rows;
	double normal_mean, normal_stddev;
	int rc = 0;

	if (opts->reads == 0) {
		fprintf(stderr, "reads must be > 0\n");
		return -EINVAL;
	}

	seq->logical_idx = calloc(opts->reads, sizeof(unsigned int));
	if (!seq->logical_idx)
		return -ENOMEM;
	seq->count = opts->reads;

	unsigned int rng_state = seed ? seed : 42U;
	if (opts->dist == DIST_ZIPF) {
		rc = init_zipf_sampler(&zipf, total_rows, opts->zipf_alpha);
		if (rc != 0) {
			free(seq->logical_idx);
			seq->logical_idx = NULL;
			return rc;
		}
	}

	resolve_normal_params(layout, opts, &normal_mean, &normal_stddev);

	for (unsigned int i = 0; i < seq->count; ++i) {
		unsigned int logical_idx;

		switch (opts->dist) {
		case DIST_SEQUENTIAL:
			logical_idx = total_rows ? (i % total_rows) : 0U;
			break;
		case DIST_ZIPF:
			logical_idx = sample_zipf(&zipf, rand_uniform(&rng_state));
			break;
		case DIST_EXPONENTIAL:
			logical_idx = sample_exponential(total_rows, opts->exp_lambda, &rng_state);
			break;
		case DIST_NORMAL:
			logical_idx = sample_normal(total_rows, normal_mean, normal_stddev, &rng_state);
			break;
		case DIST_UNIFORM:
		default:
			logical_idx = next_rand(&rng_state) % total_rows;
			break;
		}

		seq->logical_idx[i] = logical_idx;
	}

	destroy_zipf_sampler(&zipf);
	return 0;
}

static void free_request_sequence(struct request_sequence *seq)
{
	if (!seq)
		return;
	free(seq->logical_idx);
	seq->logical_idx = NULL;
	seq->count = 0;
}

static void destroy_table_states(struct table_state *tables, unsigned int count)
{
	if (!tables)
		return;
	for (unsigned int i = 0; i < count; ++i) {
		if (tables[i].insert_stmt)
			sqlite3_finalize(tables[i].insert_stmt);
		if (tables[i].select_stmt)
			sqlite3_finalize(tables[i].select_stmt);
		free(tables[i].row_reads);
		free(tables[i].row_latency);
		tables[i].row_reads = NULL;
		tables[i].row_latency = NULL;
	}
	free(tables);
}

static int prepare_table_states(sqlite3 *db, const struct dataset_layout *layout,
				const struct workload_options *opts,
				struct table_state **tables_out,
				unsigned long long *total_rows_out)
{
	struct table_state *tables = NULL;
	unsigned long long total_rows = 0;
	int rc = 0;

	(void)opts;

	if (!layout || !tables_out)
		return -EINVAL;

	tables = calloc(layout->table_count, sizeof(*tables));
	if (!tables)
		return -ENOMEM;

	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		char sql[256];
		char table_name[MAX_TABLE_NAME];
		unsigned int rows = layout->rows_per_table[tbl];

		tables[tbl].table_id = tbl;
		tables[tbl].total_rows = rows;
		tables[tbl].rows_inserted = 0;
		tables[tbl].row_reads = calloc(rows ? rows : 1U, sizeof(unsigned long long));
		tables[tbl].row_latency = calloc(rows ? rows : 1U, sizeof(double));
		if (!tables[tbl].row_reads || !tables[tbl].row_latency) {
			rc = -ENOMEM;
			goto out;
		}

		build_table_name(table_name, sizeof(table_name), tbl);
		snprintf(sql, sizeof(sql),
			 "CREATE TABLE %s("
			 "id INT PRIMARY KEY,"
			 "str1 VARCHAR(%d),"
			 "str2 VARCHAR(%d),"
			 "str3 VARCHAR(%d),"
			 "str4 VARCHAR(%d));",
			 table_name, STR1_LEN, STR2_LEN, STR3_LEN, STR4_LEN);
		rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to create table %s (%s)\n",
				table_name, sqlite3_errmsg(db));
			goto out;
		}

		snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES(?, ?, ?, ?, ?);", table_name);
		rc = sqlite3_prepare_v2(db, sql, -1, &tables[tbl].insert_stmt, NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to prepare insert for %s (%s)\n",
				table_name, sqlite3_errmsg(db));
			goto out;
		}

		snprintf(sql, sizeof(sql),
			 "SELECT str1,str2,str3,str4 FROM %s WHERE id >= ? ORDER BY id;",
			 table_name);
		rc = sqlite3_prepare_v2(db, sql, -1, &tables[tbl].select_stmt, NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to prepare read for %s (%s)\n",
				table_name, sqlite3_errmsg(db));
			goto out;
		}

		total_rows += rows;
	}

	*tables_out = tables;
	if (total_rows_out)
		*total_rows_out = total_rows;
	return 0;

out:
	destroy_table_states(tables, layout->table_count);
	return rc;
}

static int insert_rows_batch(struct table_state *table, unsigned int rows_this_batch)
{
	char rstr1[STR1_LEN + 1];
	char rstr2[STR2_LEN + 1];
	char rstr3[STR3_LEN + 1];
	char rstr4[STR4_LEN + 1];
	sqlite3_stmt *stmt;
	unsigned int limit;
	int rc;

	if (!table || !table->insert_stmt)
		return -EINVAL;
	if (rows_this_batch == 0)
		return 0;

	stmt = table->insert_stmt;
	limit = table->rows_inserted + rows_this_batch;

	for (unsigned int row = table->rows_inserted; row < limit; ++row) {
		int record_id = (int)(table->total_rows - 1U - row);

		random_string(rstr1, STR1_LEN);
		random_string(rstr2, STR2_LEN);
		random_string(rstr3, STR3_LEN);
		random_string(rstr4, STR4_LEN);

		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_bind_int(stmt, 1, record_id);
		sqlite3_bind_text(stmt, 2, rstr1, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 3, rstr2, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 4, rstr3, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 5, rstr4, -1, SQLITE_TRANSIENT);

		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			fprintf(stderr, "Insert failed (table=%u row=%u): %s\n",
				table->table_id, row, sqlite3_errmsg(sqlite3_db_handle(stmt)));
			return -EIO;
		}
	}

	return 0;
}

static int build_table_read_plan(const struct dataset_layout *layout,
				 const struct workload_options *opts,
				 unsigned int reads_per_event,
				 unsigned int **plan_out)
{
	struct zipf_sampler zipf = {};
	bool zipf_initialized = false;
	unsigned int *plan;
	unsigned int table_count;
	unsigned int rng_state;
	double normal_mean;
	double normal_stddev;
	int rc = 0;

	if (!layout || !plan_out)
		return -EINVAL;

	table_count = layout->table_count;
	plan = calloc(table_count ? table_count : 1U, sizeof(unsigned int));
	if (!plan)
		return -ENOMEM;

	if (reads_per_event == 0 || table_count == 0) {
		*plan_out = plan;
		return 0;
	}

	time_t now = time(NULL);
	unsigned int global_seed = opts->seed ? opts->seed : (unsigned int)now;
	rng_state = global_seed;

	if (opts->dist == DIST_ZIPF) {
		rc = init_zipf_sampler(&zipf, table_count, opts->zipf_alpha);
		if (rc != 0) {
			free(plan);
			return rc;
		}
		zipf_initialized = true;
	}

	if (opts->dist == DIST_NORMAL) {
		normal_mean = opts->normal_mean;
		if (normal_mean <= NORMAL_MEAN_SENTINEL || normal_mean >= (double)table_count || normal_mean < 0.0)
			normal_mean = table_count ? ((double)table_count - 1.0) / 2.0 : 0.0;
		normal_stddev = opts->normal_stddev;
		if (normal_stddev <= NORMAL_STDDEV_SENTINEL)
			normal_stddev = table_count ? (double)table_count / 6.0 : 1.0;
	} else {
		normal_mean = 0.0;
		normal_stddev = 1.0;
	}

	for (unsigned int i = 0; i < reads_per_event; ++i) {
		unsigned int table_id = 0;

		switch (opts->dist) {
		case DIST_SEQUENTIAL:
			table_id = table_count ? (i % table_count) : 0U;
			break;
		case DIST_ZIPF:
			table_id = sample_zipf(&zipf, rand_uniform(&rng_state));
			break;
		case DIST_EXPONENTIAL:
			table_id = sample_exponential(table_count, opts->exp_lambda, &rng_state);
			break;
		case DIST_NORMAL:
			table_id = sample_normal(table_count, normal_mean, normal_stddev, &rng_state);
			break;
		case DIST_UNIFORM:
		default:
			table_id = table_count ? (next_rand(&rng_state) % table_count) : 0U;
			break;
		}

		plan[table_id]++;
	}

	if (zipf_initialized)
		destroy_zipf_sampler(&zipf);
	*plan_out = plan;
	return 0;
}

static int run_read_event(unsigned int event_id,
			  const struct dataset_layout *layout,
			  struct table_state *tables,
			  unsigned int table_count,
			  const unsigned int *read_plan,
			  double *table_latency,
			  unsigned long long *table_read_ops,
			  double *elapsed_out)
{
	double start = monotonic_sec();
	sqlite3 *db = NULL;

	(void)layout;

	for (unsigned int tbl = 0; tbl < table_count; ++tbl) {
		unsigned int reads = read_plan ? read_plan[tbl] : 0;
		struct table_state *table = &tables[tbl];
		sqlite3_stmt *stmt = table->select_stmt;
		int lower_bound;
		double table_event_latency = 0.0;

		if (!stmt || reads == 0 || table->rows_inserted == 0)
			continue;

		lower_bound = (int)(table->total_rows - table->rows_inserted);
		if (lower_bound < 0)
			lower_bound = 0;
		db = sqlite3_db_handle(stmt);

		for (unsigned int iter = 0; iter < reads; ++iter) {
			int rc;
			double iter_start, iter_end;

			sqlite3_reset(stmt);
			sqlite3_clear_bindings(stmt);
			sqlite3_bind_int(stmt, 1, lower_bound);

			iter_start = monotonic_sec();
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
				;
			iter_end = monotonic_sec();
			if (rc != SQLITE_DONE) {
				fprintf(stderr, "Read failed on table %u: %s\n",
					table->table_id,
					db ? sqlite3_errmsg(db) : "db handle unavailable");
				return -EIO;
			}
			table_latency[tbl] += iter_end - iter_start;
			table_event_latency += iter_end - iter_start;
			table_read_ops[tbl]++;
		}

		for (unsigned int row = 0; row < table->rows_inserted; ++row) {
			table->row_reads[row] += reads;
			if (table->row_latency)
				table->row_latency[row] += table_event_latency;
		}
	}

	if (elapsed_out)
		*elapsed_out = monotonic_sec() - start;

	printf("[sqlite_init] read_event=%u completed tables=%u elapsed=%.6fs\n",
	       event_id, table_count, elapsed_out ? *elapsed_out : (monotonic_sec() - start));
	return 0;
}

static unsigned int *materialize_row_heat(const struct dataset_layout *layout,
					  const struct table_state *tables)
{
	unsigned int *heat = NULL;

	if (!layout || !tables)
		return NULL;

	heat = calloc(layout->total_rows, sizeof(unsigned int));
	if (!heat)
		return NULL;

	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		const struct table_state *table = &tables[tbl];
		unsigned int base = layout->row_prefix[tbl];

		for (unsigned int row = 0; row < table->total_rows; ++row) {
			unsigned long long row_heat = table->row_reads ? table->row_reads[row] : 0ULL;
			if (row_heat > UINT_MAX)
				row_heat = UINT_MAX;
			heat[base + row] = (unsigned int)row_heat;
		}
	}

	return heat;
}

static int write_row_stats_csv(const char *path, const struct dataset_layout *layout,
			       const struct table_state *tables)
{
	FILE *fp;

	if (!path || !path[0] || !layout || !tables)
		return 0;

	fp = fopen(path, "w");
	if (!fp) {
		perror("fopen row stats");
		return -1;
	}

	fprintf(fp, "table_id,table_name,row_index,logical_row_index,pages,reads,total_latency_sec,avg_latency_sec\n");

	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		const struct table_state *table = &tables[tbl];
		unsigned int logical_base = layout->row_prefix[tbl];
		char table_name[MAX_TABLE_NAME];

		build_table_name(table_name, sizeof(table_name), tbl);

		for (unsigned int row = 0; row < table->total_rows; ++row) {
			unsigned long long reads = table->row_reads ? table->row_reads[row] : 0ULL;
			double total_latency = table->row_latency ? table->row_latency[row] : 0.0;
			double avg_latency = reads ? total_latency / (double)reads : 0.0;
			unsigned int pages = (unsigned int)LPN_PER_ROW;

			fprintf(fp, "%u,%s,%u,%u,%u,%llu,%.9f,%.9f\n",
				tbl, table_name, row, logical_base + row, pages,
				reads, total_latency, avg_latency);
		}
	}

	fclose(fp);
	return 0;
}

static double run_cold_full_read(sqlite3 *db, const struct dataset_layout *layout,
				 unsigned int iterations, double *per_table_out)
{
	unsigned int runs = iterations ? iterations : 1U;
	double total = 0.0;

	if (!db || !layout)
		return 0.0;

	if (per_table_out) {
		for (unsigned int t = 0; t < layout->table_count; ++t)
			per_table_out[t] = 0.0;
	}

	for (unsigned int iter = 0; iter < runs; ++iter) {
		double start = monotonic_sec();

		for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
			char sql[256];
			char table_name[MAX_TABLE_NAME];
			sqlite3_stmt *stmt = NULL;
			int rc;
			double tbl_start, tbl_end;

			/* drop cache before each table for independent cold measurement */
			drop_page_cache();

			build_table_name(table_name, sizeof(table_name), tbl);
			snprintf(sql, sizeof(sql),
				 "SELECT str1,str2,str3,str4 FROM %s ORDER BY id;", table_name);
			rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
			if (rc != SQLITE_OK) {
				fprintf(stderr, "Failed to prepare cold read for %s: %s\n",
					table_name, sqlite3_errmsg(db));
				return 0.0;
			}

			tbl_start = monotonic_sec();
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
				;
			tbl_end = monotonic_sec();

			if (rc != SQLITE_DONE)
				fprintf(stderr, "Cold read failed for %s: %s\n", table_name,
					sqlite3_errmsg(db));
			sqlite3_finalize(stmt);

			if (per_table_out)
				per_table_out[tbl] += tbl_end - tbl_start;
		}

		total += monotonic_sec() - start;
	}

	if (per_table_out && runs > 1) {
		for (unsigned int t = 0; t < layout->table_count; ++t)
			per_table_out[t] /= runs;
	}

	return runs ? total / runs : 0.0;
}

static int write_table_latency_csv(const char *path, const struct dataset_layout *layout,
				   const double *table_latency, const unsigned long long *table_read_ops)
{
	FILE *fp;

	if (!path || !path[0] || !layout || !table_latency || !table_read_ops)
		return 0;

	fp = fopen(path, "w");
	if (!fp) {
		perror("fopen table stats");
		return -1;
	}

	fprintf(fp, "table_id,table_name,read_ops,total_latency_sec,avg_latency_sec\n");
	for (unsigned int tbl = 0; tbl < layout->table_count; ++tbl) {
		char table_name[MAX_TABLE_NAME];
		double total = table_latency[tbl];
		unsigned long long ops = table_read_ops[tbl];
		double avg = ops ? total / (double)ops : 0.0;

		build_table_name(table_name, sizeof(table_name), tbl);
		fprintf(fp, "%u,%s,%llu,%.9f,%.9f\n", tbl, table_name, ops, total, avg);
	}

	fclose(fp);
	return 0;
}

struct cold_tail_entry {
	unsigned int table_id;
	unsigned long long heat_sum;
	double avg_latency;
};

static int cmp_cold_entry(const void *a, const void *b)
{
	const struct cold_tail_entry *ea = a;
	const struct cold_tail_entry *eb = b;

	if (ea->heat_sum < eb->heat_sum)
		return -1;
	if (ea->heat_sum > eb->heat_sum)
		return 1;
	return (ea->table_id > eb->table_id) - (ea->table_id < eb->table_id);
}

static void report_cold_tail_latency(const struct dataset_layout *layout,
				     const struct table_state *tables,
				     const double *table_latency,
				     const unsigned long long *table_read_ops)
{
	unsigned int table_count = layout ? layout->table_count : 0;
	struct cold_tail_entry *entries;
	unsigned int selected = 0;
	double avg_latency = 0.0;

	if (!layout || !tables || !table_latency || !table_read_ops || table_count == 0)
		return;

	entries = calloc(table_count, sizeof(*entries));
	if (!entries)
		return;

	for (unsigned int tbl = 0; tbl < table_count; ++tbl) {
		unsigned long long heat_sum = 0;

		for (unsigned int row = 0; row < tables[tbl].total_rows; ++row)
			heat_sum += tables[tbl].row_reads ? tables[tbl].row_reads[row] : 0ULL;

		entries[tbl].table_id = tbl;
		entries[tbl].heat_sum = heat_sum;
		if (table_read_ops[tbl] > 0)
			entries[tbl].avg_latency = table_latency[tbl] / (double)table_read_ops[tbl];
		else
			entries[tbl].avg_latency = 0.0;
	}

	qsort(entries, table_count, sizeof(*entries), cmp_cold_entry);

	unsigned int tail_count = table_count / 10U;
	if (tail_count == 0 && table_count > 0)
		tail_count = 1;

	for (unsigned int i = 0; i < table_count && selected < tail_count; ++i) {
		if (table_read_ops[entries[i].table_id] == 0)
			continue;
		avg_latency += entries[i].avg_latency;
		selected++;
	}

	if (selected)
		avg_latency /= (double)selected;

	printf("[sqlite_init] coldest_10pct_tables=%u avg_latency=%.6fs\n",
	       selected, selected ? avg_latency : 0.0);
	free(entries);
}

static void drop_dataset_cache(sqlite3 *db, const char *db_path)
{
	if (db) {
		int rc = sqlite3_db_cacheflush(db);

		if (rc != SQLITE_OK)
			fprintf(stderr, "sqlite3_db_cacheflush failed: %s\n", sqlite3_errstr(rc));
		sqlite3_db_release_memory(db);
	}

	if (!db_path || !db_path[0])
		return;

	int fd = open(db_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open db for cache drop");
		return;
	}

#if defined(__linux__)
	int err = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	if (err != 0) {
		errno = err;
		perror("posix_fadvise");
	}
#elif defined(F_NOCACHE)
	int flag = 1;
	if (fcntl(fd, F_NOCACHE, &flag) == -1)
		perror("fcntl F_NOCACHE enable");
	flag = 0;
	if (fcntl(fd, F_NOCACHE, &flag) == -1)
		perror("fcntl F_NOCACHE disable");
#else
	(void)fd;
#endif
	close(fd);
}

static int run_interleaved_init(const struct dataset_layout *layout,
				const struct workload_options *opts,
				unsigned int **heat_out)
{
	sqlite3 *db = NULL;
	struct table_state *tables = NULL;
	unsigned int *active = NULL;
	unsigned int *read_plan = NULL;
	double *table_latency = NULL;
	unsigned long long *table_read_ops = NULL;
	unsigned int table_count = layout->table_count;
	unsigned long long total_rows = 0;
	unsigned long long rows_written = 0;
	unsigned int active_count = 0;
	unsigned int rows_since_commit = 0;
	unsigned int read_events = 0;
	unsigned int interleave_rows = opts->interleave_rows ?
		opts->interleave_rows : DEFAULT_INTERLEAVE_ROWS;
	unsigned int reads_per_event = opts->init_reads_per_event ?
		opts->init_reads_per_event : DEFAULT_READS_PER_EVENT;
	unsigned int rng_state = opts->seed ? opts->seed : (unsigned int)time(NULL);
	char db_path[PATH_MAX];
	char row_stats_path[PATH_MAX];
	char table_stats_path[PATH_MAX];
	double total_read_time = 0.0;
	double cold_read_time = 0.0;
	double *cold_per_table = NULL;
	unsigned int *heat = NULL;
	bool txn_active = false;
	int rc = -1;

	if (!heat_out)
		return -EINVAL;

	if (reads_per_event == 0)
		reads_per_event = DEFAULT_READS_PER_EVENT;

	build_db_path(db_path, sizeof(db_path), opts);
	unlink(db_path);

	rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		return -1;
	}

	rc = sqlite3_exec(db, "PRAGMA journal_mode = off;", NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "journal_mode pragma failed\n");
		goto out;
	}

	rc = sqlite3_exec(db, "PRAGMA synchronous = on;", NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "synchronous pragma failed\n");
		goto out;
	}

	rc = prepare_table_states(db, layout, opts, &tables, &total_rows);
	if (rc != 0)
		goto out;

	build_row_stats_path(row_stats_path, sizeof(row_stats_path), opts);
	build_table_stats_path(table_stats_path, sizeof(table_stats_path), opts);

	active = malloc(table_count * sizeof(*active));
	table_latency = calloc(table_count, sizeof(double));
	table_read_ops = calloc(table_count, sizeof(unsigned long long));
	if (!active || !table_latency || !table_read_ops) {
		rc = -ENOMEM;
		goto out;
	}

	for (unsigned int i = 0; i < table_count; ++i)
		active[i] = i;
	active_count = table_count;

	printf("[sqlite_init] config tables=%u total_rows=%llu row_pages=%u interleave_rows=%u "
	       "read_ops_per_event=%u\n",
	       table_count, total_rows, (unsigned int)LPN_PER_ROW, interleave_rows,
	       reads_per_event);

	rc = build_table_read_plan(layout, opts, reads_per_event, &read_plan);
	if (rc != 0)
		goto out;

	rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to begin transaction\n");
		goto out;
	}
	txn_active = true;

	while (rows_written < total_rows) {
		unsigned int pick;
		struct table_state *table;
		unsigned int remaining_rows;
		unsigned int rows_this_step = 1;

		if (active_count == 0)
			break;

		pick = (active_count == 1) ? 0 : (next_rand(&rng_state) % active_count);
		table = &tables[active[pick]];
		remaining_rows = table->total_rows > table->rows_inserted ?
			(table->total_rows - table->rows_inserted) : 0U;
		if (remaining_rows == 0) {
			active[pick] = active[active_count - 1];
			active_count--;
			continue;
		}
		if (rows_this_step > remaining_rows)
			rows_this_step = remaining_rows;

		rc = insert_rows_batch(table, rows_this_step);
		if (rc != 0)
			goto out;

		table->rows_inserted += rows_this_step;
		rows_written += rows_this_step;
		rows_since_commit += rows_this_step;

		if (table->rows_inserted >= table->total_rows) {
			active[pick] = active[active_count - 1];
			active_count--;
		}

		if (rows_since_commit >= 0x4000U) {
			if (txn_active)
				sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
			txn_active = false;
			sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
			txn_active = true;
			rows_since_commit = 0;
		}

		if ((rows_written && interleave_rows && (rows_written % interleave_rows == 0)) ||
		    rows_written == total_rows) {
			double event_elapsed = 0.0;
			bool restart_txn = rows_written < total_rows;

			if (txn_active)
				sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
			txn_active = false;
			rows_since_commit = 0;
			drop_dataset_cache(db, db_path);

			read_events++;
			rc = run_read_event(read_events, layout, tables, table_count, read_plan,
					    table_latency, table_read_ops, &event_elapsed);
			if (rc != 0)
				goto out;
			total_read_time += event_elapsed;

			if (restart_txn) {
				rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
				if (rc != SQLITE_OK) {
					fprintf(stderr, "Failed to restart transaction\n");
					goto out;
				}
				txn_active = true;
			}
		}
	}

	if (txn_active) {
		sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
		txn_active = false;
	}

	cold_per_table = calloc(table_count ? table_count : 1U, sizeof(double));
	cold_read_time = run_cold_full_read(db, layout, 1, cold_per_table);

	heat = materialize_row_heat(layout, tables);
	if (!heat) {
		rc = -ENOMEM;
		goto out;
	}

	if (write_row_stats_csv(row_stats_path, layout, tables) != 0)
		fprintf(stderr, "Failed to write row stats %s\n", row_stats_path);
	if (write_table_latency_csv(table_stats_path, layout, table_latency, table_read_ops) != 0)
		fprintf(stderr, "Failed to write table stats %s\n", table_stats_path);
	printf("[sqlite_init] row_stats=%s table_stats=%s\n",
	       row_stats_path, table_stats_path);

	report_cold_tail_latency(layout, tables, table_latency, table_read_ops);

	double cold_bytes_mb = (double)layout->total_rows * ROW_PAYLOAD_BYTES / (1024.0 * 1024.0);
	double cold_throughput = cold_read_time > 0.0 ? cold_bytes_mb / cold_read_time : 0.0;

	printf("[sqlite_init] tag=%s tables=%u total_rows=%u read_events=%u "
	       "interleaved_read_time=%.6fs cold_full_read=%.6fs cold_full_read_tp=%.2fMB/s db=%s\n",
	       effective_tag(opts), layout->table_count, layout->total_rows, read_events,
	       total_read_time, cold_read_time, cold_throughput, db_path);

	if (cold_per_table) {
		for (unsigned int tbl = 0; tbl < table_count; ++tbl) {
			char name[MAX_TABLE_NAME];
			double tbl_bytes_mb = (double)layout->rows_per_table[tbl] *
					      ROW_PAYLOAD_BYTES / (1024.0 * 1024.0);
			double tbl_tp = cold_per_table[tbl] > 0.0 ?
					tbl_bytes_mb / cold_per_table[tbl] : 0.0;

			build_table_name(name, sizeof(name), tbl);
			printf("[sqlite_cold_table] table=%s rows=%u time=%.6fs throughput=%.2fMB/s\n",
			       name, layout->rows_per_table[tbl],
			       cold_per_table[tbl], tbl_tp);
		}
	}

	*heat_out = heat;
	heat = NULL;
	rc = 0;

out:
	if (heat)
		free(heat);
	free(cold_per_table);
	free(read_plan);
	free(active);
	free(table_latency);
	free(table_read_ops);
	destroy_table_states(tables, table_count);
	if (db)
		sqlite3_close(db);
	return rc;
}

static int generate_default_traces(const struct workload_options *opts,
				   const struct dataset_layout *layout,
				   const unsigned int *heat,
				   unsigned int derived_reads)
{
	enum distribution_type dists[] = {DIST_ZIPF, DIST_EXPONENTIAL, DIST_NORMAL};
	unsigned int *scratch = NULL;
	size_t heat_bytes;
	int rc = 0;

	if (!layout || layout->total_rows == 0 || !heat)
		return 0;

	heat_bytes = (size_t)layout->total_rows * sizeof(unsigned int);
	scratch = calloc(layout->total_rows, sizeof(unsigned int));
	if (!scratch)
		return -ENOMEM;

	for (size_t i = 0; i < sizeof(dists) / sizeof(dists[0]); ++i) {
		memcpy(scratch, heat, heat_bytes);
		rc = build_trace_with_heat(dists[i], derived_reads, opts, layout, scratch);
		if (rc != 0)
			break;
	}

	free(scratch);
	return rc;
}

static int save_trace_file(const char *path, const struct dataset_layout *layout,
			   const struct workload_options *opts,
			   const struct request_sequence *seq)
{
	FILE *fp;

	if (!path || !path[0])
		return 0;

	fp = fopen(path, "w");
	if (!fp) {
		perror("fopen trace");
		return -1;
	}

	fprintf(fp, "%s\n", TRACE_HEADER_PREFIX);
	fprintf(fp, "table_count=%u\n", layout->table_count);
	fprintf(fp, "total_rows=%u\n", layout->total_rows);
	fprintf(fp, "distribution=%s\n", dist_name(opts->dist));
	fprintf(fp, "reads=%u\n", seq->count);
	fprintf(fp, "seed=%u\n", effective_seed(opts, opts->dist));

	for (unsigned int i = 0; i < seq->count; ++i)
		fprintf(fp, "%u\n", seq->logical_idx[i]);

	fclose(fp);
	return 0;
}

static enum distribution_type parse_dist_name(const char *name)
{
	if (!name)
		return DIST_UNIFORM;
	if (strcasecmp(name, "zipf") == 0)
		return DIST_ZIPF;
	if (strcasecmp(name, "exp") == 0 || strcasecmp(name, "exponential") == 0)
		return DIST_EXPONENTIAL;
	if (strcasecmp(name, "normal") == 0 || strcasecmp(name, "gaussian") == 0)
		return DIST_NORMAL;
	if (strcasecmp(name, "sequential") == 0 || strcasecmp(name, "seq") == 0)
		return DIST_SEQUENTIAL;
	return DIST_UNIFORM;
}

static int load_trace_file(const char *path, const struct dataset_layout *layout,
			   struct request_sequence *seq, enum distribution_type *dist_out,
			   unsigned int *seed_out)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	unsigned int expected_tables = 0;
	unsigned int expected_rows = 0;
	unsigned int expected_reads = 0;
	char dist_buf[32] = "";
	unsigned int seed = 0;
	unsigned int capacity = 0;
	unsigned int count = 0;
	unsigned int *logical = NULL;

	if (!fp) {
		perror("fopen trace");
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *newline;

		if (line[0] == '#')
			continue;

		newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		if (strncmp(line, "table_count=", 12) == 0) {
			expected_tables = (unsigned int)strtoul(line + 12, NULL, 10);
			continue;
		}
		if (strncmp(line, "total_rows=", 11) == 0) {
			expected_rows = (unsigned int)strtoul(line + 11, NULL, 10);
			continue;
		}
		if (strncmp(line, "distribution=", 13) == 0) {
			copy_text(dist_buf, sizeof(dist_buf), line + 13);
			continue;
		}
		if (strncmp(line, "reads=", 6) == 0) {
			expected_reads = (unsigned int)strtoul(line + 6, NULL, 10);
			continue;
		}
		if (strncmp(line, "seed=", 5) == 0) {
			seed = (unsigned int)strtoul(line + 5, NULL, 10);
			continue;
		}

		if (*line == '\0')
			continue;

		if (count == capacity) {
			unsigned int new_cap = capacity ? capacity * 2 : 1024;
			unsigned int *tmp = realloc(logical, new_cap * sizeof(unsigned int));
			if (!tmp) {
				free(logical);
				fclose(fp);
				return -ENOMEM;
			}
			logical = tmp;
			capacity = new_cap;
		}

		logical[count++] = (unsigned int)strtoul(line, NULL, 10);
	}
	fclose(fp);

	if (!logical || count == 0) {
		fprintf(stderr, "Trace %s is empty\n", path);
		free(logical);
		return -EINVAL;
	}

	if (expected_tables && expected_tables != layout->table_count) {
		fprintf(stderr, "Trace table count mismatch\n");
		free(logical);
		return -EINVAL;
	}
	if (expected_rows && expected_rows != layout->total_rows) {
		fprintf(stderr, "Trace total rows mismatch\n");
		free(logical);
		return -EINVAL;
	}
	if (expected_reads && expected_reads != count) {
		fprintf(stderr, "Trace read count mismatch (header=%u actual=%u)\n",
			expected_reads, count);
		free(logical);
		return -EINVAL;
	}

	seq->logical_idx = logical;
	seq->count = count;
	if (dist_out)
		*dist_out = parse_dist_name(dist_buf[0] ? dist_buf : NULL);
	if (seed_out)
		*seed_out = seed;
	return 0;
}

static int write_heatmap_csv(const char *path, const struct dataset_layout *layout,
			     const unsigned int *hits, const double *latency_sums)
{
	FILE *fp;

	fp = fopen(path, "w");
	if (!fp) {
		perror("fopen heatmap");
		return -1;
	}

	fprintf(fp, "logical_index,record_id,hits,total_latency_sec,avg_latency_sec\n");
	for (unsigned int idx = 0; idx < layout->total_rows; ++idx) {
		unsigned int hit = hits[idx];
		double total = latency_sums[idx];
		double avg = hit ? total / hit : 0.0;
		int record_id;

		layout_lookup(layout, idx, NULL, NULL, &record_id);
		fprintf(fp, "%u,%d,%u,%.9f,%.9f\n", idx, record_id, hit, total, avg);
	}

	fclose(fp);
	return 0;
}

static int accumulate_heat(unsigned int *heat, unsigned int total_rows,
			   const struct request_sequence *seq)
{
	for (unsigned int i = 0; i < seq->count; ++i) {
		unsigned int idx = seq->logical_idx[i];
		if (idx < total_rows)
			heat[idx]++;
	}
	return 0;
}

static int build_trace_with_heat(enum distribution_type dist, unsigned int reads,
				 const struct workload_options *opts,
				 const struct dataset_layout *layout,
				 unsigned int *heat)
{
	struct workload_options tmp = *opts;
	struct request_sequence seq = {};
	char trace_path[PATH_MAX];
	int rc;

	tmp.dist = dist;
	tmp.reads = reads;

	rc = fill_request_sequence(&seq, layout, &tmp);
	if (rc != 0)
		return rc;

	rc = accumulate_heat(heat, layout->total_rows, &seq);
	if (rc != 0) {
		free_request_sequence(&seq);
		return rc;
	}

	build_trace_path_for_dist(trace_path, sizeof(trace_path), &tmp, dist);
	rc = save_trace_file(trace_path, layout, &tmp, &seq);
	free_request_sequence(&seq);
	if (rc == 0) {
		printf("[sqlite_init] trace generated: %s\n", trace_path);
		fflush(stdout);
	}
	return rc;
}

static int capture_text_callback(void *ctx, int argc, char **argv, char **azColName)
{
	struct text_slot *slot = ctx;

	(void)argc;
	(void)azColName;

	if (argv && argv[0])
		copy_text(slot->buf, slot->len, argv[0]);

	return 0;
}

static void print_waiting_banner(const struct pragma_snapshot *snap)
{
	printf("Waiting.....\n");
	printf("Waiting.....\n");
	printf("journal_mode = %s\n\n", snap->journal_mode);
	printf("synchronous = %s\n\n", snap->synchronous);
}

static int run_init_mode(const struct workload_options *opts)
{
	struct dataset_layout layout = {};
	unsigned int *heat = NULL;
	unsigned int derived_reads;
	char layout_path[PATH_MAX];
	char heat_path[PATH_MAX];
	int rc;

	rc = dataset_layout_from_options(opts, &layout);
	if (rc != 0) {
		fprintf(stderr, "Failed to build layout (%d)\n", rc);
		return -1;
	}

	build_layout_path(layout_path, sizeof(layout_path), opts);
	build_heat_path(heat_path, sizeof(heat_path), opts);

	derived_reads = resolve_effective_reads(opts, layout.total_rows);

	srand(opts->seed ? opts->seed : (unsigned int)time(NULL));

	rc = run_interleaved_init(&layout, opts, &heat);
	if (rc != 0) {
		fprintf(stderr, "Failed to build dataset\n");
		free(heat);
		dataset_layout_destroy(&layout);
		return rc;
	}

	rc = save_heat_csv(heat_path, heat, layout.total_rows);
	if (rc != 0) {
		fprintf(stderr, "Failed to save heat map\n");
		free(heat);
		dataset_layout_destroy(&layout);
		return rc;
	}

	rc = generate_default_traces(opts, &layout, heat, derived_reads);
	if (rc != 0) {
		fprintf(stderr, "Failed to generate trace files (%d)\n", rc);
		free(heat);
		dataset_layout_destroy(&layout);
		return rc;
	}

	rc = dataset_layout_to_file(layout_path, &layout);
	if (rc != 0) {
		fprintf(stderr, "Failed to persist layout metadata\n");
		free(heat);
		dataset_layout_destroy(&layout);
		return rc;
	}

printf("[sqlite_init] tag=%s rows=%u approx_bytes=%.2f GiB heat=%s\n",
       effective_tag(opts), layout.total_rows,
       (double)(layout.total_rows * ROW_PAYLOAD_BYTES) / (1024.0 * 1024.0 * 1024.0),
       heat_path);

free(heat);
dataset_layout_destroy(&layout);
return 0;
}

static int run_scan_mode(const struct workload_options *opts)
{
	struct dataset_layout layout = {};
	char layout_path[PATH_MAX];
	char db_path[PATH_MAX];
	sqlite3 *db = NULL;
	sqlite3_stmt **stmts = NULL;
	unsigned int scan_iters = opts->scan_iters ? opts->scan_iters : SCAN_ITER_DEFAULT;
	double total_bytes_mb;
	double throughput_sum = 0.0;
	int rc = -1;

	build_layout_path(layout_path, sizeof(layout_path), opts);
	if (dataset_layout_from_file(layout_path, &layout) != 0)
		return -1;

	total_bytes_mb = (double)layout.total_rows * ROW_PAYLOAD_BYTES / (1024.0 * 1024.0);
	build_db_path(db_path, sizeof(db_path), opts);

	rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		rc = -1;
		goto out;
	}

	stmts = calloc(layout.table_count ? layout.table_count : 1U, sizeof(sqlite3_stmt *));
	if (!stmts) {
		fprintf(stderr, "Failed to allocate scan statements\n");
		goto out;
	}

	for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl) {
		char table_name[MAX_TABLE_NAME];
		char scan_sql[256];

		build_table_name(table_name, sizeof(table_name), tbl);
		snprintf(scan_sql, sizeof(scan_sql),
			 "SELECT str1,str2,str3,str4 FROM %s ORDER BY id;", table_name);
		rc = sqlite3_prepare_v2(db, scan_sql, -1, &stmts[tbl], NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "prepare scan failed for %s: %s\n",
				table_name, sqlite3_errmsg(db));
			goto out;
		}
	}

	for (unsigned int iter = 0; iter < scan_iters; ++iter) {
		double start = monotonic_sec();

		for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl) {
			sqlite3_stmt *stmt = stmts[tbl];
			int step_rc;

			sqlite3_reset(stmt);
			while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW)
				;
			if (step_rc != SQLITE_DONE) {
				fprintf(stderr, "scan step error: %s\n", sqlite3_errmsg(db));
				goto out;
			}
		}

		double end = monotonic_sec();
		double elapsed = end - start;
		double throughput_mb = elapsed > 0.0 ? total_bytes_mb / elapsed : 0.0;

		printf("[sqlite_scan] iter=%u bytes=%.2fMB time=%.6fs throughput=%.2fMB/s\n",
		       iter, total_bytes_mb, elapsed, throughput_mb);
		throughput_sum += throughput_mb;
	}

	if (scan_iters)
		printf("[sqlite_scan] average=%.2fMB/s (%u iterations)\n",
		       throughput_sum / scan_iters, scan_iters);

	rc = 0;

out:
	if (stmts) {
		for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl)
			if (stmts[tbl])
				sqlite3_finalize(stmts[tbl]);
		free(stmts);
	}
	if (db)
		sqlite3_close(db);
	dataset_layout_destroy(&layout);
	return rc;
}

static int run_read_mode(const struct workload_options *opts)
{
	struct dataset_layout layout = {};
	struct request_sequence seq = {};
	sqlite3 *db = NULL;
	sqlite3_stmt **stmts = NULL;
	FILE *log_fp = NULL;
	unsigned int *heat_hits = NULL;
	double *heat_latency_sums = NULL;
	double *latencies = NULL;
	unsigned int *table_hits = NULL;
	double *table_latency = NULL;
	struct zipf_sampler zipf = {};
	char db_path[PATH_MAX];
	char layout_path[PATH_MAX];
	unsigned int effective_reads = opts->reads;
	enum distribution_type trace_dist = opts->dist;
	unsigned int trace_seed = effective_seed(opts, opts->dist);
	struct pragma_snapshot snapshot = {};
	struct text_slot journal_slot = { .buf = snapshot.journal_mode, .len = sizeof(snapshot.journal_mode) };
	struct text_slot sync_slot = { .buf = snapshot.synchronous, .len = sizeof(snapshot.synchronous) };
	int rc = -1;

	build_layout_path(layout_path, sizeof(layout_path), opts);
	if (dataset_layout_from_file(layout_path, &layout) != 0)
		goto out;

	if (opts->trace_mode == TRACE_MODE_REPLAY) {
		if (!opts->trace_path || !opts->trace_path[0]) {
			fprintf(stderr, "trace replay requested without --trace-path\n");
			goto out;
		}
		if (load_trace_file(opts->trace_path, &layout, &seq, &trace_dist, &trace_seed) != 0)
			goto out;
		effective_reads = seq.count;
	} else {
		if (!opts->reads_explicit && layout.total_rows > 0)
			effective_reads = layout.total_rows;
		else if (effective_reads == 0)
			effective_reads = DEFAULT_READS;
		if (fill_request_sequence(&seq, &layout, opts) != 0)
			goto out;
	}

	build_db_path(db_path, sizeof(db_path), opts);
	rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		rc = -1;
		goto out;
	}

	if (opts->log_path) {
		log_fp = fopen(opts->log_path, "w");
		if (!log_fp) {
			perror("fopen log");
			goto out;
		}
		fprintf(log_fp, "seq,logical_idx,record_id,latency_sec\n");
	}

	if (opts->heatmap_path) {
		heat_hits = calloc(layout.total_rows, sizeof(unsigned int));
		heat_latency_sums = calloc(layout.total_rows, sizeof(double));
		if (!heat_hits || !heat_latency_sums) {
			fprintf(stderr, "Failed to allocate heatmap buffers\n");
			goto out;
		}
	}

	latencies = calloc(effective_reads, sizeof(double));
	table_hits = calloc(layout.table_count, sizeof(unsigned int));
	table_latency = calloc(layout.table_count, sizeof(double));
	if (!latencies || !table_hits || !table_latency) {
		fprintf(stderr, "Failed to allocate stats buffers\n");
		goto out;
	}

	stmts = calloc(layout.table_count, sizeof(sqlite3_stmt *));
	if (!stmts) {
		fprintf(stderr, "Failed to allocate stmt array\n");
		goto out;
	}

	for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl) {
		char sql[256];
		char name[MAX_TABLE_NAME];

		build_table_name(name, sizeof(name), tbl);
		snprintf(sql, sizeof(sql), "SELECT str1,str2,str3,str4 FROM %s WHERE id=?;", name);
		rc = sqlite3_prepare_v2(db, sql, -1, &stmts[tbl], NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to prepare SELECT for %s\n", name);
			goto out;
		}
	}

	if (opts->human_log) {
		rc = sqlite3_exec(db, "PRAGMA journal_mode;", capture_text_callback, &journal_slot, NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "journal_mode pragma read failed\n");
			goto out;
		}
		rc = sqlite3_exec(db, "PRAGMA synchronous;", capture_text_callback, &sync_slot, NULL);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "synchronous pragma read failed\n");
			goto out;
		}
	}

	for (unsigned int i = 0; i < effective_reads; ++i) {
		unsigned int logical_idx = seq.logical_idx[i];
		unsigned int table_id = 0;
		unsigned int row_idx = 0;
		int record_id = 0;
		double start, end, latency;
		sqlite3_stmt *stmt;

		if (layout_lookup(&layout, logical_idx, &table_id, &row_idx, &record_id) != 0) {
			fprintf(stderr, "Invalid logical idx %u\n", logical_idx);
			goto out;
		}

		stmt = stmts[table_id];
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_bind_int(stmt, 1, record_id);

		if (opts->human_log)
			print_waiting_banner(&snapshot);

		start = monotonic_sec();
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
			;
		end = monotonic_sec();

		if (rc != SQLITE_DONE) {
			fprintf(stderr, "sqlite3_step error: %s\n", sqlite3_errmsg(db));
			goto out;
		}

		latency = end - start;
		latencies[i] = latency;
		table_hits[table_id]++;
		table_latency[table_id] += latency;

		if (log_fp)
			fprintf(log_fp, "%u,%u,%d,%.9f\n", i, logical_idx, record_id, latency);

		if (heat_hits) {
			heat_hits[logical_idx]++;
			heat_latency_sums[logical_idx] += latency;
		}

		if (opts->human_log)
			printf("read %u, time: %.9f\n", i, latency);
	}

	if (opts->trace_mode == TRACE_MODE_RECORD) {
		if (!opts->trace_path || !opts->trace_path[0])
			build_default_trace_path(((struct workload_options *)opts)->trace_path_buf,
						 sizeof(opts->trace_path_buf), opts);

		const char *path = opts->trace_path && opts->trace_path[0]
					   ? opts->trace_path
					   : opts->trace_path_buf;

		if (save_trace_file(path, &layout, opts, &seq) != 0)
			goto out;
	}

	qsort(latencies, effective_reads, sizeof(double), cmp_double);

	double total = 0.0;
	for (unsigned int i = 0; i < effective_reads; ++i)
		total += latencies[i];

	double avg = total / effective_reads;
	double p50 = latencies[effective_reads / 2];
	double p95 = latencies[(unsigned int)(effective_reads * 0.95)];
	double p99 = latencies[(unsigned int)(effective_reads * 0.99)];
	double throughput = total > 0.0 ? effective_reads / total : 0.0;

	if (!opts->suppress_report) {
		printf("[sqlite_read] tag=%s dist=%s reads=%u seed=%u avg=%.9f "
		       "p50=%.9f p95=%.9f p99=%.9f\n",
		       effective_tag(opts), dist_name(trace_dist), effective_reads,
		       trace_seed, avg, p50, p95, p99);
		printf("[sqlite_read] throughput=%.3f ops/sec\n", throughput);

		for (unsigned int tbl = 0; tbl < layout.table_count; ++tbl) {
			char name[MAX_TABLE_NAME];
			double tbl_avg = table_hits[tbl] ? table_latency[tbl] / table_hits[tbl] : 0.0;
			double tbl_tp = table_latency[tbl] > 0.0 ? table_hits[tbl] / table_latency[tbl] : 0.0;

			build_table_name(name, sizeof(name), tbl);
			printf("[sqlite_read_table] table=%s hits=%u avg=%.9f throughput=%.3f ops/sec\n",
			       name, table_hits[tbl], tbl_avg, tbl_tp);
		}
	} else {
		printf("[sqlite_read_warm] tag=%s dist=%s reads=%u completed\n",
		       effective_tag(opts), dist_name(trace_dist), effective_reads);
	}

	if (opts->heatmap_path) {
		if (write_heatmap_csv(opts->heatmap_path, &layout, heat_hits, heat_latency_sums) != 0)
			goto out;
	}

	rc = 0;

out:
	if (rc != 0)
		fprintf(stderr, "read mode failed\n");
	if (log_fp)
		fclose(log_fp);
	if (stmts) {
		for (unsigned int i = 0; i < layout.table_count; ++i)
			if (stmts[i])
				sqlite3_finalize(stmts[i]);
		free(stmts);
	}
	if (db)
		sqlite3_close(db);
	free_request_sequence(&seq);
	free(heat_hits);
	free(heat_latency_sums);
	free(latencies);
	free(table_hits);
	free(table_latency);
	dataset_layout_destroy(&layout);
	return rc;
}

static void configure_options(int argc, char **argv, struct workload_options *opts)
{
	int c;

	memset(opts, 0, sizeof(*opts));
	opts->mode = MODE_INIT;
	opts->dist = DIST_ZIPF;
	opts->reads = DEFAULT_READS;
	opts->zipf_alpha = 1.2;
	opts->exp_lambda = 0.0008;
	opts->seed = 42U;
	opts->zipf_seed = 0;
	opts->exp_seed = 0;
	opts->normal_seed = 0;
	opts->log_path = NULL;
	opts->heatmap_path = NULL;
	opts->human_log = false;
	opts->normal_mean = NORMAL_MEAN_SENTINEL;
	opts->normal_stddev = NORMAL_STDDEV_SENTINEL;
	opts->table_count = DEFAULT_TABLE_COUNT;
	opts->rows_per_table = 0;
	opts->target_bytes = DEFAULT_TARGET_BYTES;
	opts->tag = DEFAULT_TAG;
	opts->trace_dir = TARGET_FOLDER;
	opts->trace_mode = TRACE_MODE_NONE;
	opts->trace_path = NULL;
	opts->trace_path_buf[0] = '\0';
	opts->suppress_report = false;
	opts->scan_iters = SCAN_ITER_DEFAULT;
	opts->reads_explicit = false;
	opts->interleave_rows = DEFAULT_INTERLEAVE_ROWS;
	opts->init_reads_per_event = DEFAULT_READS_PER_EVENT;

	while ((c = getopt_long(argc, argv, "m:d:r:a:l:s:o:H:M:S:Uh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (strcasecmp(optarg, "init") == 0)
				opts->mode = MODE_INIT;
			else if (strcasecmp(optarg, "read") == 0)
				opts->mode = MODE_READ;
			else if (strcasecmp(optarg, "scan") == 0)
				opts->mode = MODE_SCAN;
			else {
				fprintf(stderr, "Unknown mode '%s'\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'd':
			opts->dist = parse_distribution(optarg);
			break;
		case 'r':
			opts->reads = (unsigned int)strtoul(optarg, NULL, 10);
			opts->reads_explicit = true;
			break;
		case 'a':
			opts->zipf_alpha = strtod(optarg, NULL);
			break;
		case 'l':
			opts->exp_lambda = strtod(optarg, NULL);
			break;
		case 's':
			opts->seed = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'o':
			opts->log_path = optarg;
			break;
		case 'H':
			opts->heatmap_path = optarg;
			break;
		case 'M':
			opts->normal_mean = strtod(optarg, NULL);
			break;
		case 'S':
			opts->normal_stddev = strtod(optarg, NULL);
			break;
		case 'U':
			opts->human_log = true;
			break;
		case 1000:
			opts->zipf_seed = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1001:
			opts->exp_seed = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1002:
			opts->normal_seed = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1003:
			opts->table_count = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1004:
			opts->rows_per_table = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 1005:
			opts->target_bytes = parse_size_arg(optarg);
			break;
		case 1006:
			opts->tag = optarg;
			break;
		case 1007:
			opts->trace_dir = optarg;
			break;
		case 1008:
			if (strcasecmp(optarg, "record") == 0)
				opts->trace_mode = TRACE_MODE_RECORD;
			else if (strcasecmp(optarg, "replay") == 0)
				opts->trace_mode = TRACE_MODE_REPLAY;
			else if (strcasecmp(optarg, "none") == 0)
				opts->trace_mode = TRACE_MODE_NONE;
			else {
				fprintf(stderr, "Unknown trace mode '%s'\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 1009:
			opts->trace_path = optarg;
			break;
		case 1010:
			opts->suppress_report = true;
			break;
		case 1011:
			opts->scan_iters = (unsigned int)strtoul(optarg, NULL, 10);
			if (opts->scan_iters == 0)
				opts->scan_iters = SCAN_ITER_DEFAULT;
			break;
		case 1012:
			opts->interleave_rows = (unsigned int)strtoul(optarg, NULL, 10);
			if (opts->interleave_rows == 0)
				opts->interleave_rows = DEFAULT_INTERLEAVE_ROWS;
			break;
		case 1013:
			opts->init_reads_per_event = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'h':
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (opts->mode == MODE_READ && opts->reads == 0 && opts->trace_mode != TRACE_MODE_REPLAY) {
		fprintf(stderr, "reads must be > 0 for read mode\n");
		exit(EXIT_FAILURE);
	}

	if (opts->trace_mode != TRACE_MODE_NONE && (!opts->trace_path || !opts->trace_path[0])) {
		build_default_trace_path(opts->trace_path_buf, sizeof(opts->trace_path_buf), opts);
		opts->trace_path = opts->trace_path_buf;
	}
}

int main(int argc, char **argv)
{
	struct workload_options opts;

	configure_options(argc, argv, &opts);

	if (opts.mode == MODE_INIT)
		return run_init_mode(&opts) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	if (opts.mode == MODE_READ)
		return run_read_mode(&opts) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	if (opts.mode == MODE_SCAN)
		return run_scan_mode(&opts) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	fprintf(stderr, "Unsupported mode\n");
	return EXIT_FAILURE;
}
static void build_db_path(char *buf, size_t len, const struct workload_options *opts)
{
	char filename[128];

	snprintf(filename, sizeof(filename), DB_FILENAME_FMT, effective_tag(opts));
	join_path(buf, len, TARGET_FOLDER, filename);
}

static void build_layout_path(char *buf, size_t len, const struct workload_options *opts)
{
	char filename[128];

	snprintf(filename, sizeof(filename), LAYOUT_FILENAME_FMT, effective_tag(opts));
	join_path(buf, len, RESULT_FOLDER, filename);
}

static const char *trace_suffix(enum distribution_type dist)
{
	switch (dist) {
	case DIST_ZIPF:
		return "zipf";
	case DIST_EXPONENTIAL:
		return "exp";
	case DIST_NORMAL:
		return "normal";
	case DIST_SEQUENTIAL:
		return "sequential";
	case DIST_UNIFORM:
	default:
		return "uniform";
	}
}

static void build_trace_path_for_dist(char *buf, size_t len,
				      const struct workload_options *opts,
				      enum distribution_type dist)
{
	char filename[192];
	const char *dir = opts->trace_dir ? opts->trace_dir : RESULT_FOLDER;

	snprintf(filename, sizeof(filename), TRACE_FILENAME_FMT,
		 effective_tag(opts), trace_suffix(dist));
	join_path(buf, len, dir, filename);
}

static void build_heat_path(char *buf, size_t len, const struct workload_options *opts)
{
	char filename[128];

	snprintf(filename, sizeof(filename), HEAT_FILENAME_FMT, effective_tag(opts));
	join_path(buf, len, RESULT_FOLDER, filename);
}

static void build_row_stats_path(char *buf, size_t len, const struct workload_options *opts)
{
	char filename[128];

	snprintf(filename, sizeof(filename), ROW_STATS_FILENAME_FMT, effective_tag(opts));
	join_path(buf, len, RESULT_FOLDER, filename);
}

static void build_table_stats_path(char *buf, size_t len, const struct workload_options *opts)
{
	char filename[128];

	snprintf(filename, sizeof(filename), TABLE_STATS_FILENAME_FMT, effective_tag(opts));
	join_path(buf, len, RESULT_FOLDER, filename);
}
