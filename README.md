# SCI-SPDK-NVMe: Syscall Interception for SPDK/NVMe

sci-spdk-nvme.so is a library to intercept system call as well as memory allocation call and redirect them to SPDK/NVMe backend interfaces, achieving high IO performance without any modification to existing applications. The source code demonstrates how to intercept syscall/malloc associated with FIO. In short, several system calls from AIO and buffer pool allocation from FIO are intercepted and redirected to corresponding SPDK/NVMe APIs.

The complete procedures to build and run sci-spdk-nvme enabled FIO is provided below. For more technical details please visit my Medium article [Boost NVMe IOPS from 4M to 14.5M via System Call Interception](https://medium.com/@colinzhu/boost-nvme-iops-from-4m-to-14-5m-via-system-call-interception-8e27da4aed9a)

## Build Procedures for Ubuntu (20.04.x and 22.04)
Following procedures have been tested for LTS 20.04.4 and LTS 22.04. Some special tweaks are necessary for 22.04 (see step 4 and 5).

### 1. Installation directories
- Use your home directory ```~/``` (in my case ```/home/czhu/```) to install ```syscall_intercept```, ```spdk```, and ```sci-spdk-nvme``` in the subsequent steps.
- Since root privilege is required to load ```sci-spdk-nvme.so``` (see step 7), optionally you can login as ```root``` and install everything under ```/root/``` directory.
- All the commands described in following steps assume you login as regular user such as ```czhu```.

### 2. Install following packages if not available
```
sudo apt-get install build-essential
sudo apt-get install pkg-config
sudo apt-get install libcapstone-dev
sudo apt-get install pandoc
sudo apt-get install clang
sudo apt-get install git
sudo apt-get install cmake
sudo apt-get install fio
```

### 3. Install syscall_intercept library
- Get source codes:
```
cd ~/
git clone https://github.com/pmem/syscall_intercept.git
```
- Run following commands to build:
```
cd syscall_intercept
mkdir build
cd build/
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
make
sudo make install
```
- Library and header file are installed into ```/usr/local/``` directory.
```
Install the project...
-- Install configuration: "Release"
-- Installing: /usr/local/lib/libsyscall_intercept.so.0.1.0
-- Installing: /usr/local/lib/libsyscall_intercept.so.0
-- Installing: /usr/local/lib/libsyscall_intercept.so
-- Installing: /usr/local/include/libsyscall_intercept_hook_point.h
-- Installing: /usr/local/lib/libsyscall_intercept.a
-- Up-to-date: /usr/local/include/libsyscall_intercept_hook_point.h
-- Installing: /usr/local/lib/pkgconfig/libsyscall_intercept.pc
-- Installing: /usr/local/share/man/man3/libsyscall_intercept.3
```

### 4. Install SPDK
```
cd ~/
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
```
For Ubuntu 22.04, edit file ```~/spdk/scripts/pkgdep/debian.sh``` and change line ```...... libiscsi-dev python libncurses5-dev ......``` to ```...... libiscsi-dev python3 libncurses5-dev ......``` before typing  following commands.
```
sudo ./scripts/pkgdep.sh
./configure
make
```

### 5. Build sci-spdk-nvme.so library
- Download source code via ```git clone https://github.com/StorWav/sci_spdk_nvme.git```.
For Ubuntu 22.04 please make sure sync the latest code. GLIBC v2.35 installed by 22.04 uses SYS_clone3 to create thread instead of SYS_clone, that causes segfault when loading sci-spdk-nvme library. The workaround is to return ENOSYS to SYS_clone3 forcing it to go back SYS_clone.
- Enter ```~/spdk/examples/nvme/``` and copy the entire directory ```sci_spdk_nvme``` here.
- Enter ```~/spdk/examples/nvme/sci_spdk_nvme``` and type ```make```
- Type the following command
```
cc sci_spdk.c -lsyscall_intercept -pthread -fpic -shared -D_GNU_SOURCE -o sci-spdk-nvme.so spdk-nvme.so
```
- Two libraries ```spdk-nvme.so``` and ```sci-spdk-nvme.so``` are generated under directory ```~/spdk/examples/nvme/sci_spdk_nvme```

### 6. Install sci-spdk-nvme.so library
- Make testing directory in your home directory such as ```~/fio-sci-spdk/```
- Enter the directory and make the following two links, assuming SPDK root is in ```~/spdk/```
```
ln -s ~/spdk/examples/nvme/sci_spdk_nvme/spdk-nvme.so spdk-nvme.so
ln -s ~/spdk/examples/nvme/sci_spdk_nvme/sci-spdk-nvme.so sci-spdk-nvme.so
ln -s /usr/local/lib/libsyscall_intercept.so libsyscall_intercept.so.0
```
- Modify FIO config file ```fiocfg``` to reflect the correct NVMe devices discovered in your server:
	* A sample ```fiocfg``` is in ```~/spdk/examples/nvme/sci_spdk_nvme/```.
	* A job must be set for each NVMe drive.
	* ```filename=``` must be correctly set for the NVMe PCIe device, for example, for device 0000:74:00.0 please replace ```:``` with ```_``` and append ```0000_74_00.0``` as device file name that FIO will load.
	* Each job must have one and only one CPU pinned. ```cpus_allowed=``` is expected right after ```filename=```line for the job. Same cpu can be used for multiple jobs.
	* Other tunable parameters are ```rw=```, ```bs=```, ```iodepth=```, ```filesize=```, and ```runtime=```.
	* FIO configuration file must use ```fiocfg``` as the file name, however, you can modify source code to use a different name.
- DO NOT change any other settings in ```fiocfg```, for example, disabling ```thread=1``` will cause FIO to spawn processes for IO operations instead of threads, and sci-spdk-nvme only works with thread scenario.
- Under ```~/fio-sci-spdk/```, create empty device files (such as ```0000_74_00.0```) for each of the NVMe devices.

After the above steps, the deployment directory ```~/fio-sci-spdk/```should look like this, where I have 16 NVMe devices installed:

```
czhu@Ubuntu-T7920:~/fio-sci-spdk$ ls -lrt
lrwxrwxrwx 1 czhu czhu  56 May 18 23:40 spdk-nvme.so -> /home/czhu/spdk/examples/nvme/sci_spdk_nvme/spdk-nvme.so
lrwxrwxrwx 1 czhu czhu  60 May 18 23:40 sci-spdk-nvme.so -> /home/czhu/spdk/examples/nvme/sci_spdk_nvme/sci-spdk-nvme.so
lrwxrwxrwx 1 czhu czhu  38 May 18 23:40 libsyscall_intercept.so.0 -> /usr/local/lib/libsyscall_intercept.so
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_d8_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_d7_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_d6_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_d5_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_a9_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_a8_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_a7_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_a6_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_76_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_75_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_74_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_73_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_1a_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_19_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_18_00.0
-rw-r--r-- 1 czhu czhu   0 May 18 23:42 0000_17_00.0
-rw-r--r-- 1 czhu czhu 996 May 18 23:43 fiocfg
```

### 7. Test with fio 
- Configure SPDK NVMe devices by running ```sudo ~/spdk/scripts/setup.sh```. Root privilege is required to access DMA from user space. See [Direct Memory Access (DMA) From User Space](https://spdk.io/doc/memory.html). If you wish to run with non-privileges please check SPDK user guides.
- Under ```~/fio-sci-spdk/``` or the testing directory created in step 6, run FIO with the following command:
```
sudo LD_LIBRARY_PATH=. LD_PRELOAD=sci-spdk-nvme.so fio fiocfg
```

### 8. Todo and known issues

- Will add scripts to auto generate ```fiocfg``` as well as those NVMe device files ```0000_##_00.0```. The script also attempts to pin each device file with the CPU (```cpus_allowed=```) that resides in the same NUMA node as the NVMe drive.

- At this moment sci-spdk-nvme requires each NVMe namespace residing on dedicated controller, for example, controller ```/dev/nvme0``` must have one and only one namespace ```/dev/nvme0n1```. Will add support for multi-namespaces on the same NVMe controller such as ```/dev/nvme0n2```, etc.


For questions and issues please email me through czhu@nexnt.com

Have fun!


