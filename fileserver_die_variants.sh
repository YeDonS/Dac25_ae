#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source commonvariables.sh

APPEND_PROFILE="${APPEND_PROFILE:-small}"
VARIANTS="${VARIANTS:-die_base die_no1 die_no2 die_no3 die_all}"
READ_RUNS="${READ_RUNS:-1}"
ACCESS_DIST="${ACCESS_DIST:-none}"
DIST_WARMUP_OPS="${DIST_WARMUP_OPS:-1000}"
DIST_READ_OPS="${DIST_READ_OPS:-1000}"
NORMAL_MEAN="${NORMAL_MEAN:--1}"
NORMAL_STDDEV="${NORMAL_STDDEV:-400}"
NORMAL_SEED="${NORMAL_SEED:-314159}"
ZIPF_SEED="${ZIPF_SEED:-42}"
EXP_SEED="${EXP_SEED:-4242}"
ZIPF_ALPHA="${ZIPF_ALPHA:-1.2}"
EXP_LAMBDA="${EXP_LAMBDA:-0.0008}"

NVMEV_DIR="${SCRIPT_DIR}/../nvmevirt_DA"

case "$APPEND_PROFILE" in
  small)
    APPEND_F="filebench/fileserver_append_small.f"
    READ_TEMPLATE_F="filebench/fileserver_read.f"
    RESULT_PREFIX="fileserver_small"
    ;;
  normal)
    APPEND_F="filebench/fileserver_append.f"
    READ_TEMPLATE_F="filebench/fileserver_read.f"
    RESULT_PREFIX="fileserver"
    ;;
  hotcold)
    APPEND_F="filebench/fileserver_append_hotcold.f"
    READ_TEMPLATE_F="filebench/fileserver_read_hotcold.f"
    WARMUP_TEMPLATE_F="filebench/fileserver_read_hotcold_warm.f"
    RESULT_PREFIX="fileserver_hotcold"
    ;;
  *)
    echo "ERROR: APPEND_PROFILE must be 'small', 'normal', or 'hotcold'" >&2
    exit 1
    ;;
esac

DIE_RESULT_BASE="${RESULT_FOLDER%/}/${RESULT_PREFIX}_die_variants"
TMP_APPEND_F="${SCRIPT_DIR}/filebench/.fileserver_append_runtime.f"
TMP_READ_F="${SCRIPT_DIR}/filebench/.fileserver_read_runtime.f"
TMP_WARMUP_F="${SCRIPT_DIR}/filebench/.fileserver_warmup_runtime.f"
DIST_HELPER="${SCRIPT_DIR}/filebench/fileserver_dist_access.py"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

load_die_module() {
    local variant="$1"
    local ko_path="${NVMEV_DIR}/nvmev_${variant}.ko"

    if [[ ! -f "$ko_path" ]]; then
        echo "ERROR: $ko_path not found. Run build_die.sh first." >&2
        exit 1
    fi

    echo "=== Loading $ko_path (via nvmevstart_on.sh) ==="
    if [[ -f ./nvmev_on.ko ]]; then
        cp ./nvmev_on.ko ./nvmev_on.ko.die_bak
    fi
    cp "$ko_path" ./nvmev_on.ko
    ./nvmevstart_on.sh
    if [[ -f ./nvmev_on.ko.die_bak ]]; then
        mv ./nvmev_on.ko.die_bak ./nvmev_on.ko
    fi
    sleep 1
}

capture_tree_size() {
    local root="$1"
    local out="$2"

    {
        echo "[tree_size]"
        if [[ -d "$root" ]]; then
            du -sh "$root" 2>/dev/null || true
            du -sb "$root" 2>/dev/null || true
            find "$root" -type f 2>/dev/null | wc -l | awk '{print "files=" $1}'
        else
            echo "missing=$root"
        fi
    } >"$out"
}

capture_tree_size_hotcold() {
    local root="$1"
    local out="$2"

    {
        echo "[tree_size]"
        if [[ -d "$root" ]]; then
            du -sh "$root" 2>/dev/null || true
            du -sb "$root" 2>/dev/null || true
            if [[ -d "$root/hot" ]]; then
                echo "[hot]"
                du -sh "$root/hot" 2>/dev/null || true
                du -sb "$root/hot" 2>/dev/null || true
                find "$root/hot" -type f 2>/dev/null | wc -l | awk '{print "files=" $1}'
            fi
            if [[ -d "$root/cold" ]]; then
                echo "[cold]"
                du -sh "$root/cold" 2>/dev/null || true
                du -sb "$root/cold" 2>/dev/null || true
                find "$root/cold" -type f 2>/dev/null | wc -l | awk '{print "files=" $1}'
            fi
        else
            echo "missing=$root"
        fi
    } >"$out"
}

extract_fb_var() {
    local file="$1"
    local var_name="$2"
    awk -F= -v name="$var_name" '
        $0 ~ "^[[:space:]]*set[[:space:]]*\\$" name "[[:space:]]*=" {
            gsub(/[[:space:]]+/, "", $2);
            print $2;
            exit;
        }
    ' "$file"
}

resolve_fb_var() {
    local source_f="$1"
    local var_name="$2"
    local override_name="$3"
    local value=""

    if [[ -n "$override_name" ]]; then
        value="${!override_name}"
    fi
    if [[ -z "$value" ]]; then
        value="$(extract_fb_var "$source_f" "$var_name")"
    fi
    printf '%s' "$value"
}

generate_runtime_workload() {
    local source_f="$1"
    local out_f="$2"
    local role="$3"
    local nfiles filesize meandirwidth hot_files cold_files
    local nthreads hot_threads cold_threads runtime iosize meanappendsize

    nfiles="$(resolve_fb_var "$source_f" "nfiles" "FB_NFILES")"
    filesize="$(resolve_fb_var "$source_f" "filesize" "FB_FILESIZE")"
    meandirwidth="$(resolve_fb_var "$source_f" "meandirwidth" "FB_MEANDIRWIDTH")"
    hot_files="$(resolve_fb_var "$source_f" "hot_files" "FB_HOT_FILES")"
    cold_files="$(resolve_fb_var "$source_f" "cold_files" "FB_COLD_FILES")"

    case "$role" in
        append)
            nthreads="$(resolve_fb_var "$source_f" "nthreads" "FB_APPEND_NTHREADS")"
            hot_threads="$(resolve_fb_var "$source_f" "hot_threads" "FB_APPEND_HOT_THREADS")"
            cold_threads="$(resolve_fb_var "$source_f" "cold_threads" "FB_APPEND_COLD_THREADS")"
            runtime="$(resolve_fb_var "$source_f" "runtime" "FB_APPEND_RUNTIME")"
            iosize="$(resolve_fb_var "$source_f" "iosize" "FB_APPEND_IOSIZE")"
            meanappendsize="$(resolve_fb_var "$source_f" "meanappendsize" "FB_MEANAPPENDSIZE")"
            ;;
        warmup)
            nthreads="$(resolve_fb_var "$source_f" "nthreads" "FB_WARMUP_NTHREADS")"
            hot_threads="$(resolve_fb_var "$source_f" "hot_threads" "FB_WARMUP_HOT_THREADS")"
            cold_threads="$(resolve_fb_var "$source_f" "cold_threads" "FB_WARMUP_COLD_THREADS")"
            runtime="$(resolve_fb_var "$source_f" "runtime" "FB_WARMUP_RUNTIME")"
            iosize="$(resolve_fb_var "$source_f" "iosize" "FB_WARMUP_IOSIZE")"
            meanappendsize=""
            ;;
        read)
            nthreads="$(resolve_fb_var "$source_f" "nthreads" "FB_READ_NTHREADS")"
            hot_threads="$(resolve_fb_var "$source_f" "hot_threads" "FB_READ_HOT_THREADS")"
            cold_threads="$(resolve_fb_var "$source_f" "cold_threads" "FB_READ_COLD_THREADS")"
            runtime="$(resolve_fb_var "$source_f" "runtime" "FB_READ_RUNTIME")"
            iosize="$(resolve_fb_var "$source_f" "iosize" "FB_READ_IOSIZE")"
            meanappendsize=""
            ;;
        *)
            echo "ERROR: unknown workload role '$role'" >&2
            exit 1
            ;;
    esac

    if [[ -z "$filesize" || -z "$meandirwidth" ]]; then
        echo "ERROR: failed to resolve required fileset parameters from $source_f" >&2
        exit 1
    fi

    awk \
        -v nfiles="$nfiles" \
        -v filesize="$filesize" \
        -v meandirwidth="$meandirwidth" \
        -v hot_files="$hot_files" \
        -v cold_files="$cold_files" \
        -v nthreads="$nthreads" \
        -v hot_threads="$hot_threads" \
        -v cold_threads="$cold_threads" \
        -v runtime="$runtime" \
        -v iosize="$iosize" \
        -v meanappendsize="$meanappendsize" '
        BEGIN {
            done_nfiles = 0;
            done_filesize = 0;
            done_meandirwidth = 0;
            done_hot_files = 0;
            done_cold_files = 0;
            done_nthreads = 0;
            done_hot_threads = 0;
            done_cold_threads = 0;
            done_runtime = 0;
            done_iosize = 0;
            done_meanappendsize = 0;
        }
        /^[[:space:]]*set[[:space:]]+\$nfiles=/ {
            print "set $nfiles=" nfiles;
            done_nfiles = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$filesize=/ {
            print "set $filesize=" filesize;
            done_filesize = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$meandirwidth=/ {
            print "set $meandirwidth=" meandirwidth;
            done_meandirwidth = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$hot_files=/ {
            print "set $hot_files=" hot_files;
            done_hot_files = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$cold_files=/ {
            print "set $cold_files=" cold_files;
            done_cold_files = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$nthreads=/ {
            print "set $nthreads=" nthreads;
            done_nthreads = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$hot_threads=/ {
            print "set $hot_threads=" hot_threads;
            done_hot_threads = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$cold_threads=/ {
            print "set $cold_threads=" cold_threads;
            done_cold_threads = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$runtime=/ {
            print "set $runtime=" runtime;
            done_runtime = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$iosize=/ {
            print "set $iosize=" iosize;
            done_iosize = 1;
            next;
        }
        /^[[:space:]]*set[[:space:]]+\$meanappendsize=/ {
            print "set $meanappendsize=" meanappendsize;
            done_meanappendsize = 1;
            next;
        }
        { print }
        END {
            if ((!done_nfiles && nfiles != "") || !done_filesize || !done_meandirwidth)
                exit 2;
            if ((hot_files != "" && !done_hot_files) || (cold_files != "" && !done_cold_files))
                exit 2;
            if ((nthreads != "" && !done_nthreads) ||
                (hot_threads != "" && !done_hot_threads) ||
                (cold_threads != "" && !done_cold_threads) ||
                (runtime != "" && !done_runtime) ||
                (iosize != "" && !done_iosize) ||
                (meanappendsize != "" && !done_meanappendsize))
                exit 2;
        }
    ' "$source_f" >"$out_f" || {
        echo "ERROR: failed to generate runtime workload from $source_f" >&2
        exit 1
    }
}

capture_ftl_views() {
    local variant="$1"
    local out_dir="$2"
    local prefix="${RESULT_FOLDER%/}/${RESULT_PREFIX}"

    if [[ -r /sys/kernel/debug/nvmev/ftl0/die_affinity_stats ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/die_affinity_stats \
           "${prefix}_die_affinity_${variant}.txt" 2>/dev/null || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/lpn_die_change_stats ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/lpn_die_change_stats \
           "${prefix}_lpn_die_change_${variant}.txt" 2>/dev/null || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/page_tier ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/page_tier \
           "${prefix}_page_tier_${variant}.txt" 2>/dev/null || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/access_count ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/access_count \
           "${prefix}_access_count_${variant}.txt" 2>/dev/null || true
    fi
    if [[ -r /sys/kernel/debug/nvmev/ftl0/page_die ]]; then
        cp /sys/kernel/debug/nvmev/ftl0/page_die \
           "${prefix}_page_die_${variant}.txt" 2>/dev/null || true
    fi

    cp "${prefix}_die_affinity_${variant}.txt" "$out_dir/" 2>/dev/null || true
    cp "${prefix}_lpn_die_change_${variant}.txt" "$out_dir/" 2>/dev/null || true
    cp "${prefix}_page_tier_${variant}.txt" "$out_dir/" 2>/dev/null || true
    cp "${prefix}_access_count_${variant}.txt" "$out_dir/" 2>/dev/null || true
    cp "${prefix}_page_die_${variant}.txt" "$out_dir/" 2>/dev/null || true
}

run_distribution_access() {
    local phase="$1"
    local variant="$2"
    local out_dir="$3"
    local root="$4"
    local ops="$5"
    local seed
    local log_path="${RESULT_FOLDER%/}/${RESULT_PREFIX}_${phase}_${variant}.txt"
    local plan_path="${RESULT_FOLDER%/}/${RESULT_PREFIX}_${phase}_${variant}_plan.csv"

    case "$ACCESS_DIST" in
        zipf) seed="$ZIPF_SEED" ;;
        normal) seed="$NORMAL_SEED" ;;
        exp) seed="$EXP_SEED" ;;
        *)
            echo "ERROR: ACCESS_DIST must be one of none|zipf|normal|exp" >&2
            exit 1
            ;;
    esac

    python3 "$DIST_HELPER" \
        --root "$root" \
        --dist "$ACCESS_DIST" \
        --ops "$ops" \
        --seed "$seed" \
        --normal-mean "$NORMAL_MEAN" \
        --normal-stddev "$NORMAL_STDDEV" \
        --zipf-alpha "$ZIPF_ALPHA" \
        --exp-lambda "$EXP_LAMBDA" \
        --plan-out "$plan_path" \
        --label "${phase}-${variant}" | tee "$log_path"

    cp "$log_path" "$out_dir/" 2>/dev/null || true
    cp "$plan_path" "$out_dir/" 2>/dev/null || true
}

run_one_variant() {
    local variant="$1"
    local out_dir="${DIE_RESULT_BASE}/${variant}"
    local append_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_append_${variant}.txt"
    local read_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_read_${variant}.txt"
    local size_log="${RESULT_FOLDER%/}/${RESULT_PREFIX}_size_${variant}.txt"

    mkdir -p "$out_dir"

    echo ""
    echo "================================================================"
    echo "  [FILESERVER-DIE] variant=$variant profile=$APPEND_PROFILE"
    echo "  append=$APPEND_F read-template=$READ_TEMPLATE_F"
    echo "  access_dist=$ACCESS_DIST"
    echo "================================================================"

    drop_caches
    sleep 2

    load_die_module "$variant"

    lsblk
    source setdevice.sh
    sleep 1

    generate_runtime_workload "$APPEND_F" "$TMP_APPEND_F" append

    echo "=== APPEND: $variant ==="
    filebench -f "$TMP_APPEND_F" | tee "$append_log"

    if [[ "$APPEND_PROFILE" == "hotcold" ]]; then
        capture_tree_size_hotcold "${TARGET_FOLDER%/}" "$size_log"
    else
        capture_tree_size "${TARGET_FOLDER%/}/bigfileset" "$size_log"
    fi
    cat "$size_log"

    sleep 5

    : >"$read_log"
    if [[ "$ACCESS_DIST" != "none" ]]; then
        echo "=== DISTRIBUTION WARMUP: $variant ($ACCESS_DIST) ===" | tee -a "$read_log"
        drop_caches
        sleep 2
        run_distribution_access "warmup" "$variant" "$out_dir" "${TARGET_FOLDER%/}" "$DIST_WARMUP_OPS" | tee -a "$read_log"
        capture_ftl_views "${variant}_warmup" "$out_dir"
        drop_caches
        sleep 2
        for reads in $(seq 1 "$READ_RUNS"); do
            echo "=== DISTRIBUTION READ ${reads}/${READ_RUNS}: $variant ($ACCESS_DIST) ===" | tee -a "$read_log"
            drop_caches
            sleep 2
            run_distribution_access "read${reads}" "$variant" "$out_dir" "${TARGET_FOLDER%/}" "$DIST_READ_OPS" | tee -a "$read_log"
        done
    else
    if [[ "$APPEND_PROFILE" == "hotcold" ]]; then
        generate_runtime_workload "$WARMUP_TEMPLATE_F" "$TMP_WARMUP_F" warmup
        echo "=== WARMUP READ: $variant ===" | tee -a "$read_log"
        numactl --cpubind="$NUMADOMAIN" --membind="$NUMADOMAIN" \
            filebench -f "$TMP_WARMUP_F" | tee -a "$read_log"
        capture_ftl_views "${variant}_warmup" "$out_dir"
        drop_caches
        sleep 2
    fi
    generate_runtime_workload "$READ_TEMPLATE_F" "$TMP_READ_F" read
    for reads in $(seq 1 "$READ_RUNS"); do
        echo "=== READ ${reads}/${READ_RUNS}: $variant ===" | tee -a "$read_log"
        drop_caches
        sleep 2
        numactl --cpubind="$NUMADOMAIN" --membind="$NUMADOMAIN" \
            filebench -f "$TMP_READ_F" | tee -a "$read_log"
    done
    fi

    capture_ftl_views "$variant" "$out_dir"

    cp "$append_log" "$out_dir/" 2>/dev/null || true
    cp "$read_log" "$out_dir/" 2>/dev/null || true
    cp "$size_log" "$out_dir/" 2>/dev/null || true

    source resetdevice.sh
    sleep 1
}

echo 0 > /proc/sys/kernel/randomize_va_space
./disablemeta.sh
mkdir -p "$DIE_RESULT_BASE"

for variant in $VARIANTS; do
    run_one_variant "$variant"
done

./enablemeta.sh
rm -f "$TMP_APPEND_F"
rm -f "$TMP_READ_F"
rm -f "$TMP_WARMUP_F"

echo ""
echo "========================================"
echo "  [FILESERVER-DIE] All tests completed."
echo "  Results in: $DIE_RESULT_BASE"
echo "========================================"
