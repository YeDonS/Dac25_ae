#
# Hot/cold warmup read workload for filebench.
# This stage creates read-count-based heat before the measured read phase.
#

set $dir=device
set $hot_files=1000
set $cold_files=4000
set $meandirwidth=20
set $filesize=131072
set $hot_threads=8
set $cold_threads=2
set $iosize=1m
set $runtime=20

define fileset name=hotset,path=$dir/hot,size=$filesize,entries=$hot_files,dirwidth=$meandirwidth,prealloc,readonly,reuse
define fileset name=coldset,path=$dir/cold,size=$filesize,entries=$cold_files,dirwidth=$meandirwidth,prealloc,readonly,reuse

define process name=hotreader,instances=1
{
  thread name=hotthread,memsize=10m,instances=$hot_threads
  {
    flowop openfile name=hot_open,filesetname=hotset,fd=1,directio
    flowop readwholefile name=hot_read,fd=1,iosize=$iosize,directio
    flowop closefile name=hot_close,fd=1
  }
}

define process name=coldreader,instances=1
{
  thread name=coldthread,memsize=10m,instances=$cold_threads
  {
    flowop openfile name=cold_open,filesetname=coldset,fd=1,directio
    flowop readwholefile name=cold_read,fd=1,iosize=$iosize,directio
    flowop closefile name=cold_close,fd=1
  }
}

echo  "File-server hot/cold warmup read workload loaded"

run $runtime
