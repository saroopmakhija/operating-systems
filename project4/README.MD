aIMAN KOLI


cd ~/operating-systems/project4/benchmark
make clean
make


cd ~/operating-systems/project4
./rufs -s /tmp/ssm229/mountdir 

cd ~/operating-systems/project4/benchmark
./test_case

## unmount and start fresh

rm -rf /tmp/ssm229/mountdir/files   # delete old test dir
./test_case                         # run benchmark again


fusermount -u /tmp/ssm229/mountdir   # unmount
rm ~/operating-systems/project4/DISKFILE
cd ~/operating-systems/project4
./rufs -s /tmp/ssm229/mountdir       # remount, mkfs will run


