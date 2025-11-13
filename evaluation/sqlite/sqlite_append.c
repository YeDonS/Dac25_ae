#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SIZE_1024K   (1024 * 1024)
#define DUMMY_SIZE   (100 * 1024)
#define APPENDCOUNT  (10000)
#define DEFAULT_READS 5000
#define DEFAULT_ZIPF_ALPHA 1.2
#define DEFAULT_EXP_LAMBDA 0.0008
#define DEFAULT_SEED 42U
#define DEFAULT_NORMAL_MEAN ((double)(APPENDCOUNT - 1) / 2.0)
#define DEFAULT_NORMAL_STDDEV ((double)APPENDCOUNT / 6.0)

typedef unsigned int UINT32;

static void *writebuffer1024k;

static const struct option long_opts[] = {
	{"mode", required_argument, NULL, 'm'},
	{"distribution", required_argument, NULL, 'd'},
	{"reads", required_argument, NULL, 'r'},
	{"alpha", required_argument, NULL, 'a'},
	{"lambda", required_argument, NULL, 'l'},
	{"seed", required_argument, NULL, 's'},
	{"log", required_argument, NULL, 'o'},
	{"heatmap", required_argument, NULL, 'H'},
	{"human-log", no_argument, NULL, 'U'},
	{"normal-mean", required_argument, NULL, 'M'},
	{"normal-stddev", required_argument, NULL, 'S'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

enum workload_mode {
	MODE_INIT = 0,
	MODE_READ,
};

enum distribution_type {
	DIST_UNIFORM = 0,
	DIST_ZIPF,
	DIST_EXPONENTIAL,
	DIST_NORMAL,
};

struct workload_options {
	enum workload_mode mode;
	enum distribution_type dist;
	unsigned int reads;
	double zipf_alpha;
	double exp_lambda;
	unsigned int seed;
	const char *log_path;
	const char *heatmap_path;
	bool human_log;
	double normal_mean;
	double normal_stddev;
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

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --mode init\n"
		"  %s --mode read [--distribution zipf|exp|uniform]\n"
		"                  [--reads N] [--alpha A] [--lambda L]\n"
		"                  [--normal-mean MU] [--normal-stddev SIGMA]\n"
		"                  [--seed S] [--log path] [--heatmap path]\n"
		"                  [--human-log]\n",
		prog, prog);
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

static unsigned int next_rand(unsigned int *state)
{
	return rand_r(state);
}

static double rand_uniform(unsigned int *state)
{
	return (double)next_rand(state) / (double)RAND_MAX;
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

static void ensure_write_buffer(void)
{
	if (writebuffer1024k)
		return;

	if (posix_memalign(&writebuffer1024k, getpagesize(), SIZE_1024K) != 0) {
		fprintf(stderr, "posix_memalign failed\n");
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < SIZE_1024K; ++i)
		((unsigned char *)writebuffer1024k)[i] = (unsigned char)(rand() & 0xFF);
}

static void random_string(char *buffer, size_t len)
{
	for (size_t i = 0; i < len; ++i)
		buffer[i] = (char)('a' + (rand() % 26));
	buffer[len] = '\0';
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
		lambda = DEFAULT_EXP_LAMBDA;

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
		stddev = DEFAULT_NORMAL_STDDEV;
	if (mean < 0.0 || mean >= (double)n)
		mean = DEFAULT_NORMAL_MEAN;

	while (1) {
		double z = sample_standard_normal(state);
		long candidate = lround(mean + stddev * z);
		if (candidate >= 0 && candidate < (long)n)
			return (unsigned int)candidate;
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

	fprintf(stderr, "Unknown distribution '%s'\n", arg);
	exit(EXIT_FAILURE);
}

static int capture_text_callback(void *ctx, int argc, char **argv, char **azColName)
{
	struct text_slot *slot = ctx;

	(void)azColName;

	if (argc > 0 && argv[0])
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

static int write_heatmap_csv(const char *path, const unsigned int *hits,
			     const double *latency_sums)
{
	FILE *fp;

	fp = fopen(path, "w");
	if (!fp) {
		perror("fopen heatmap");
		return -1;
	}

	fprintf(fp, "logical_index,record_id,hits,total_latency_sec,avg_latency_sec\n");
	for (unsigned int idx = 0; idx < APPENDCOUNT; ++idx) {
		unsigned int hit = hits[idx];
		double total = latency_sums[idx];
		double avg = hit ? total / hit : 0.0;
		int record_id = (int)(APPENDCOUNT - 1 - idx);

		fprintf(fp, "%u,%d,%u,%.9f,%.9f\n", idx, record_id, hit, total, avg);
	}

	fclose(fp);
	return 0;
}

static void configure_options(int argc, char **argv, struct workload_options *opts)
{
	int c;

	opts->mode = MODE_INIT;
	opts->dist = DIST_ZIPF;
	opts->reads = DEFAULT_READS;
	opts->zipf_alpha = DEFAULT_ZIPF_ALPHA;
	opts->exp_lambda = DEFAULT_EXP_LAMBDA;
	opts->seed = DEFAULT_SEED;
	opts->log_path = NULL;
	opts->heatmap_path = NULL;
	opts->human_log = false;
	opts->normal_mean = DEFAULT_NORMAL_MEAN;
	opts->normal_stddev = DEFAULT_NORMAL_STDDEV;

	while ((c = getopt_long(argc, argv, "m:d:r:a:l:s:o:H:M:S:Uh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (strcasecmp(optarg, "init") == 0)
				opts->mode = MODE_INIT;
			else if (strcasecmp(optarg, "read") == 0)
				opts->mode = MODE_READ;
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
		case 'h':
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (opts->mode == MODE_READ && opts->reads == 0) {
		fprintf(stderr, "reads must be > 0 for read mode\n");
		exit(EXIT_FAILURE);
	}
}

static int run_init_mode(void)
{
	sqlite3 *db = NULL;
	char *err_msg = NULL;
	char dummy_path[1024];
	char db_path[1024];
	char rstr1[4096 + 1];
	char rstr2[4096 + 1];
	char rstr3[4096 + 1];
	char rstr4[4094 + 1];
	unsigned int indexcount = 0;
	int dummy_fd = -1;
	int rc;

	snprintf(dummy_path, sizeof(dummy_path), "%sduymmy.data", TARGET_FOLDER);
	snprintf(db_path, sizeof(db_path), "%stestdb.db", TARGET_FOLDER);

	unlink(db_path);
	unlink(dummy_path);

	ensure_write_buffer();

	rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		goto out_err;
	}

	rc = sqlite3_exec(db, "PRAGMA journal_mode = off;", NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "journal_mode pragma failed: %s\n", err_msg);
		goto out_err;
	}
	sqlite3_free(err_msg);
	err_msg = NULL;

	rc = sqlite3_exec(db, "PRAGMA synchronous = on;", NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "synchronous pragma failed: %s\n", err_msg);
		goto out_err;
	}
	sqlite3_free(err_msg);
	err_msg = NULL;

	rc = sqlite3_exec(db,
			  "CREATE TABLE DB1("
			  "id INT PRIMARY KEY,"
			  "str1 VARCHAR(4096),"
			  "str2 VARCHAR(4096),"
			  "str3 VARCHAR(4096),"
			  "str4 VARCHAR(4094));",
			  NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to create table: %s\n", err_msg);
		goto out_err;
	}
	sqlite3_free(err_msg);
	err_msg = NULL;

	dummy_fd = open(dummy_path, O_CREAT | O_RDWR | O_APPEND, 0666);
	if (dummy_fd < 0) {
		perror("open dummy");
		goto out_err;
	}

	while (indexcount < APPENDCOUNT) {
		int iter = APPENDCOUNT - 1 - (int)indexcount;
		char *query = NULL;

		random_string(rstr1, 4096);
		random_string(rstr2, 4096);
		random_string(rstr3, 4096);
		random_string(rstr4, 4094);

		if (asprintf(&query,
			     "INSERT INTO DB1 VALUES(%d, '%s', '%s', '%s', '%s');",
			     iter, rstr1, rstr2, rstr3, rstr4) < 0) {
			fprintf(stderr, "Failed to allocate insert query\n");
			goto out_err;
		}

		rc = sqlite3_exec(db, query, NULL, NULL, &err_msg);
		free(query);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "Failed to insert record: %s\n", err_msg);
			goto out_err;
		}
		sqlite3_free(err_msg);
		err_msg = NULL;

		if (write(dummy_fd, writebuffer1024k, DUMMY_SIZE) != DUMMY_SIZE) {
			perror("write dummy");
			goto out_err;
		}
		fdatasync(dummy_fd);

		indexcount++;
	}

	printf("Inserted %u rows into DB1\n", indexcount);

	sqlite3_close(db);
	close(dummy_fd);
	return 0;

out_err:
	if (err_msg)
		sqlite3_free(err_msg);
	if (db)
		sqlite3_close(db);
	if (dummy_fd >= 0)
		close(dummy_fd);
	return -1;
}

static const char *dist_name(enum distribution_type dist)
{
	switch (dist) {
	case DIST_UNIFORM:
		return "uniform";
	case DIST_ZIPF:
		return "zipf";
	case DIST_EXPONENTIAL:
		return "exponential";
	default:
		return "unknown";
	}
}

static int run_read_mode(const struct workload_options *opts)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	char *err_msg = NULL;
	char db_path[1024];
	unsigned int rng_state = opts->seed;
	struct zipf_sampler zipf = {};
	FILE *log_fp = NULL;
	double *latencies = NULL;
	unsigned int *heat_hits = NULL;
	double *heat_latency_sums = NULL;
	struct pragma_snapshot snapshot = {};
	int rc;

	if (opts->log_path) {
		log_fp = fopen(opts->log_path, "w");
		if (!log_fp) {
			perror("fopen log");
			return -1;
		}
		fprintf(log_fp, "iteration,logical_index,record_id,latency_sec\n");
	}

	if (opts->dist == DIST_ZIPF) {
		rc = init_zipf_sampler(&zipf, APPENDCOUNT, opts->zipf_alpha);
		if (rc != 0) {
			fprintf(stderr, "Failed to init zipf sampler: %d\n", rc);
			goto out_err;
		}
	}

	snprintf(db_path, sizeof(db_path), "%stestdb.db", TARGET_FOLDER);
	copy_text(snapshot.journal_mode, sizeof(snapshot.journal_mode), "unknown");
	copy_text(snapshot.synchronous, sizeof(snapshot.synchronous), "unknown");

	rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		goto out_err;
	}

	rc = sqlite3_exec(db, "PRAGMA journal_mode = off;", NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "journal_mode pragma failed: %s\n", err_msg);
		goto out_err;
	}
	sqlite3_free(err_msg);
	err_msg = NULL;

	rc = sqlite3_exec(db, "PRAGMA synchronous = off;", NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "synchronous pragma failed: %s\n", err_msg);
		goto out_err;
	}
	sqlite3_free(err_msg);
	err_msg = NULL;

	rc = sqlite3_prepare_v2(db, "SELECT str1 FROM DB1 WHERE id = ?;", -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to prepare select statement: %s\n", sqlite3_errmsg(db));
		goto out_err;
	}

	latencies = calloc(opts->reads, sizeof(double));
	if (!latencies) {
		fprintf(stderr, "Failed to allocate latency buffer\n");
		goto out_err;
	}

	if (opts->heatmap_path) {
		heat_hits = calloc(APPENDCOUNT, sizeof(*heat_hits));
		heat_latency_sums = calloc(APPENDCOUNT, sizeof(*heat_latency_sums));
		if (!heat_hits || !heat_latency_sums) {
			fprintf(stderr, "Failed to allocate heatmap buffers\n");
			goto out_err;
		}
	}

	if (opts->human_log) {
		struct text_slot journal_slot = {
			.buf = snapshot.journal_mode,
			.len = sizeof(snapshot.journal_mode),
		};
		struct text_slot sync_slot = {
			.buf = snapshot.synchronous,
			.len = sizeof(snapshot.synchronous),
		};

		rc = sqlite3_exec(db, "PRAGMA journal_mode;", capture_text_callback, &journal_slot, &err_msg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "journal_mode pragma read failed: %s\n", err_msg);
			goto out_err;
		}
		sqlite3_free(err_msg);
		err_msg = NULL;

		rc = sqlite3_exec(db, "PRAGMA synchronous;", capture_text_callback, &sync_slot, &err_msg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "synchronous pragma read failed: %s\n", err_msg);
			goto out_err;
		}
		sqlite3_free(err_msg);
		err_msg = NULL;
	}

	for (unsigned int i = 0; i < opts->reads; ++i) {
		unsigned int logical_idx;
		int record_id;
		double start, end, latency;

		switch (opts->dist) {
		case DIST_ZIPF:
			logical_idx = sample_zipf(&zipf, rand_uniform(&rng_state));
			break;
		case DIST_EXPONENTIAL:
			logical_idx = sample_exponential(APPENDCOUNT, opts->exp_lambda, &rng_state);
			break;
		case DIST_NORMAL:
			logical_idx = sample_normal(APPENDCOUNT, opts->normal_mean, opts->normal_stddev, &rng_state);
			break;
		case DIST_UNIFORM:
		default:
			logical_idx = next_rand(&rng_state) % APPENDCOUNT;
			break;
		}

		record_id = (int)(APPENDCOUNT - 1 - logical_idx);

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
			goto out_err;
		}

		latency = end - start;
		latencies[i] = latency;

		if (log_fp)
			fprintf(log_fp, "%u,%u,%d,%.9f\n", i, logical_idx, record_id, latency);

		if (opts->heatmap_path) {
			heat_hits[logical_idx]++;
			heat_latency_sums[logical_idx] += latency;
		}

		if (opts->human_log)
			printf("read %u, time: %.9f\n", i, latency);
	}

	qsort(latencies, opts->reads, sizeof(double), cmp_double);

	double total = 0.0;
	for (unsigned int i = 0; i < opts->reads; ++i)
		total += latencies[i];

	double avg = total / opts->reads;
	double p50 = latencies[opts->reads / 2];
	double p95 = latencies[(unsigned int)(opts->reads * 0.95)];
	double p99 = latencies[(unsigned int)(opts->reads * 0.99)];
	double throughput = total > 0.0 ? opts->reads / total : 0.0;

	printf("[sqlite_read] dist=%s reads=%u seed=%u avg=%.9f "
	       "p50=%.9f p95=%.9f p99=%.9f\n",
	       dist_name(opts->dist), opts->reads, opts->seed, avg, p50, p95, p99);
	printf("[sqlite_read] throughput=%.3f ops/sec\n", throughput);

	if (opts->heatmap_path) {
		if (write_heatmap_csv(opts->heatmap_path, heat_hits, heat_latency_sums) != 0)
			goto out_err;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	destroy_zipf_sampler(&zipf);
	free(latencies);
	free(heat_hits);
	free(heat_latency_sums);
	if (log_fp)
		fclose(log_fp);
	return 0;

out_err:
	if (err_msg)
		sqlite3_free(err_msg);
	if (stmt)
		sqlite3_finalize(stmt);
	if (db)
	sqlite3_close(db);
	destroy_zipf_sampler(&zipf);
	free(latencies);
	free(heat_hits);
	free(heat_latency_sums);
	if (log_fp)
		fclose(log_fp);
	return -1;
}

int main(int argc, char **argv)
{
	struct workload_options opts;

	configure_options(argc, argv, &opts);

	if (opts.mode == MODE_INIT)
		return run_init_mode() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	if (opts.mode == MODE_READ)
		return run_read_mode(&opts) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	fprintf(stderr, "Unsupported mode\n");
	return EXIT_FAILURE;
}
