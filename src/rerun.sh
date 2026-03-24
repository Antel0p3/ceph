../src/stop.sh; rm -rf out/ dev/
MDS=0 MON=1 MGR=1 OSD=6 ../src/vstart.sh -n -x --without-dashboard --filestore
./bin/ceph osd erasure-code-profile rm twotone-small   # Remove old if exists
# ./bin/ceph osd erasure-code-profile set twotone-small plugin=twotone k=3 m=2 crush-failure-domain=osd directory=$PWD/lib --force
./bin/ceph osd erasure-code-profile set twotone-small plugin=jerasure technique=reed_sol_van k=3 m=2 crush-failure-domain=osd directory=$PWD/lib --force
./bin/ceph osd pool create ec-twotone-small 64 64 erasure twotone-small

#./bin/ceph osd pool set ec-twotone-small min_size 3          # must be k+1 = 3 for k=2 m=2
#./bin/ceph osd pool set ec-twotone-small size 4
./bin/ceph osd pool application enable ec-twotone-small rados

./bin/ceph osd set norecover
./bin/ceph config set osd debug_osd 20
#./bin/ceph config set osd debug_filestore 10