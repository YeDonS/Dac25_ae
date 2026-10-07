# Dac25_ae

NVMeVirt-based storage research code for hybrid SLC/QLC SSDs, file fragmentation, die placement, and latency evaluation. The repository contains multiple FTL variants, SQLite and fio workloads, Filebench configurations, and analysis tools.

## Start here

| Need | Entry point |
| --- | --- |
| Hybrid storage overview and evaluation setup | [README_HYBRID_STORAGE.md](README_HYBRID_STORAGE.md) |
| NVMeVirt extension, kernel requirements, and memory reservation | [nvmevirt_DA/README.md](nvmevirt_DA/README.md) |
| Hybrid SSD model configuration | [nvmevirt_DA/HYBRID_SSD_CONFIG.md](nvmevirt_DA/HYBRID_SSD_CONFIG.md) |
| Evaluation workflow | [evaluation/HYBRID_TEST_GUIDE.md](evaluation/HYBRID_TEST_GUIDE.md) |
| Manual evaluation steps | [evaluation/QUICK_MANUAL_STEPS.md](evaluation/QUICK_MANUAL_STEPS.md) |

## Repository map

| Area | Contents |
| --- | --- |
| Root `conv_ftl*.c`, `conv_ftl*.h`, `ssd*.c`, and `ssd*.h` | FTL and SSD timing variants used by the root experiment scripts |
| [build_die.sh](build_die.sh) | Selects FTL variants and builds modules with die-contention timing; supported variant names are documented in the script |
| [nvmevirt_DA/](nvmevirt_DA/) | NVMeVirt module sources, Makefile, configuration, and inherited documentation |
| [evaluation/](evaluation/) | Hybrid-storage evaluation scripts and guides |
| Root `sqlite_*` files | SQLite workloads and fragmentation/placement experiments |
| Root `fio_*` files | fio workload configuration, experiment driver, validation, and summaries |
| Root `fileserver_*` files | Filebench workload configurations and hot/cold access experiments |
| [analyze_latency_run.py](analyze_latency_run.py) | Parses latency-variant logs and mechanism counters |
| [test_analyze_latency_run.py](test_analyze_latency_run.py) | Tests for the latency-log parser |
| [plot_table_die_transition.py](plot_table_die_transition.py) | Die-transition plotting helper |

## Environment and usage

The kernel module and device workloads require a Linux experiment machine, compatible kernel headers, and the toolchain and workload dependencies described in the linked guides. Review the selected SSD model, reserved-memory configuration, device paths, and the variant mapping in `build_die.sh` before running an experiment.

Older guides contain example checkout names such as `fast24_ae` and `nvmevirt_DA`. Resolve those examples against the actual layout above. The root experiment variants and the module under `nvmevirt_DA/` have separate entry points; record which source and build configuration produced each result.

## Origin

The module documentation identifies [NVMeVirt](https://github.com/snu-csl/nvmevirt) as the upstream codebase and describes die-placement changes. See [nvmevirt_DA/README.md](nvmevirt_DA/README.md) and [nvmevirt_DA/LICENSE](nvmevirt_DA/LICENSE) for the existing module attribution and license text.
