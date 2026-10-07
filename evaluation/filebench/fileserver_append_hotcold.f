#
# Hot/cold append workload for filebench.
# This phase is intentionally near-uniform per file. Heat is created later
# by warmup reads, not by biased writes during initialization.
#

set $dir=device
set $hot_files=1000
set $cold_files=4000
set $meandirwidth=20
set $filesize=131072
set $hot_threads=2
set $cold_threads=8
set $meanappendsize=16k
set $runtime=30

define fileset name=hotset,path=$dir/hot,size=$filesize,entries=$hot_files,dirwidth=$meandirwidth,prealloc,paralloc
define fileset name=coldset,path=$dir/cold,size=$filesize,entries=$cold_files,dirwidth=$meandirwidth,prealloc,paralloc

define process name=hotappender,instances=1
{
  thread name=hotthread,memsize=10m,instances=$hot_threads
  {
    flowop openfile name=hot_open,filesetname=hotset,fd=1
    flowop appendfile name=hot_append,iosize=$meanappendsize,fd=1
    flowop closefile name=hot_close,fd=1
  }
}

define process name=coldappender,instances=1
{
  thread name=coldthread,memsize=10m,instances=$cold_threads
  {
    flowop openfile name=cold_open,filesetname=coldset,fd=1
    flowop appendfile name=cold_append,iosize=$meanappendsize,fd=1
    flowop closefile name=cold_close,fd=1
  }
}

echo  "File-server hot/cold append init workload loaded"

run $runtime
