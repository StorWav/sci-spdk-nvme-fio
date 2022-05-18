# SCI-SPDK-NVMe: Syscall Interception for SPDK/NVMe

sci-spdk-nvme.so is a useful library to intercept system call as well as memory allocation call and redirect them to SPDK/NVMe interfaces, achieving high IO performance without any modification to existing applications. The source code demonstrates how to intercept syscall/malloc associated with FIO. In short, several system calls from AIO and IO buffer pool allocation from FIO are intercepted and redirected to corresponding SPDK/NVMe APIs.

The complete procedures to build and run sci-spdk-nvme enabled FIO is provided below. For more technical details please visit my Medium article [Boost NVMe IOPS from 4M to 14.5M via System Call Interception](https://medium.com/@colinzhu/boost-nvme-iops-from-4m-to-14-5m-via-system-call-interception-8e27da4aed9a)

## Build Procedures for Ubuntu 20.04

### 1. Turn on root login and always login with root

Root privilege is required to access DMA from user space. See [Direct Memory Access (DMA) From User Space](https://spdk.io/doc/memory.html). If you wish to run with non-privileges please check SPDK user guides.

### 2. Install following packages if not available
```
apt-get install pkg-config libcapstone-dev
apt-get install pandoc
apt-get install clang
```

### 3. Get syscall_intercept library
```
git clone https://github.com/pmem/syscall_intercept.git
```

### 4. Build syscall_intercept (ATTN: must install into /usr/local)
Under ```syscall_intercept``` directory, run followings:
```
mkdir build
cd build/
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
make
make install
```

### 5. Install SPDK
```
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
./scripts/pkgdep.sh
./configure
make
```

### 6. Compile sci-spdk-nvme.so library
- Make directory ```sci_spdk_nvme``` in ```spdk/examples/nvme/```
- Copy all the files into ```spdk/examples/nvme/sci_spdk_nvme```
- Type ```make```
- Type the following command
```
cc sci_spdk.c spdk-nvme.so -lsyscall_intercept -pthread -fpic -shared -D_GNU_SOURCE -o sci-spdk-nvme.so
```

### 7. Deploy sci-spdk-nvme.o library
- Make testing directory such as ```/root/fio-sci-spdk/```
- Enter the directory and make the following three links, assuming SPDK is in ```/root/spdk/```
```
libsyscall_intercept.so.0 -> /usr/local/lib64/libsyscall_intercept.so
spdk-nvme.so -> /root/spdk/examples/nvme/sci-spdk-nvme/spdk-nvme.so
sci-spdk-nvme.so -> /root/spdk/examples/nvme/sci-spdk-nvme/sci-spdk-nvme.so
```
- Modify FIO config file ```fiocfg``` to reflect the correct NVMe devices discovered in your server:
	* A job must be set for each NVMe drive.
	* ```filename=``` must be correctly set for the NVMe PCIe device, for example, for device 0000:74:00.0 please replace ```:``` with ```_``` and append ```0000_74_00.0``` as device file name that FIO will load.
	* Each job must have one and only one CPU pinned. ```cpus_allowed=``` is expected right after ```filename=```line for the job. Same cpu can be used for multiple jobs.
	* Other tunable parameters are ```rw=```, ```bs=```, ```iodepth=```, ```filesize=```, and ```runtime=```.
	* FIO configuration file must use ```fiocfg``` as the file name, however, you can modify source code to use a different name.
- DO NOT change any other settings in ```fiocfg```, for example, disabling ```thread=1``` will cause FIO to spawn processes for IO operations instead of threads, and sci-spdk-nvme only works with thread scenario.
- Create empty device files for each of the NVMe devices.

After the above steps, the deployment directory ```/root/fio-sci-spdk/```should look like this, where I have 16 NVMe devices installed:

```
lrwxrwxrwx 1 root root  55 May 16 18:00 libsyscall_intercept.so.0 -> /usr/local/lib64/libsyscall_intercept.so
lrwxrwxrwx 1 root root  55 May 16 18:00 sci-spdk-nvme.so -> /root/spdk/examples/nvme/sci_spdk_nvme/sci-spdk-nvme.so
lrwxrwxrwx 1 root root  51 May 16 18:00 spdk-nvme.so -> /root/spdk/examples/nvme/sci_spdk_nvme/spdk-nvme.so
-rw-r--r-- 1 root root   0 May 17 14:17 0000_73_00.0
-rw-r--r-- 1 root root   0 May 17 14:17 0000_74_00.0
-rw-r--r-- 1 root root   0 May 17 14:17 0000_75_00.0
-rw-r--r-- 1 root root   0 May 17 14:17 0000_76_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_17_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_18_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_19_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_1a_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_a6_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_a7_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_a8_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_a9_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_d5_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_d6_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_d7_00.0
-rw-r--r-- 1 root root   0 May 17 14:18 0000_d8_00.0
-rw-r--r-- 1 root root 996 May 17 23:28 fiocfg

```

### 8. Test with fio 
- Configure SPDK NVMe devices by running ```scripts/setup.sh``` under SPDK root directory, for example, ```/root/spdk/```
- Under ```/root/fio-sci-spdk/``` or the testing directory created in step 7, run FIO with the following command:
```
LD_PRELOAD=sci-spdk-nvme.so fio fiocfg
```

### 9. TODO

- Will add scripts to auto generate ```fiocfg``` as well as those NVMe device files ```0000_##_00.0```. The script also attempts to pin each device file with the CPU (```cpus_allowed=```) that resides in the same NUMA node as the NVMe drive.

- Backend spdk_nvme.c inherits codes from SPDK's native performance tool ```spdk/examples/nvme/perf/perf.c```. With native perf I can reach 18M IOPS (512-byte random read) and 13M IOPS (4K random read), however, ```LD_PRELOAD=sci-spdk-nvme.so fio fiocfg``` can only reach 14.5M and about 5-6M for 512-byte and 4K respectively. Will dig more to find out if it is something with my implementation or FIO related. I will also try out io_uring to find out if the same issue occurs.


For questions and issues please email me through czhu@nexnt.com

Have fun!


