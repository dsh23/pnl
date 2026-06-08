# Install build dependencies
sudo dnf install -y meson ninja-build python3-pyelftools \
    numactl-devel rdma-core-devel libpcap-devel

Kernel headers must match the running kernel:
sudo dnf install -y kernel-devel-$(uname -r)


# Download DPDK 24.11
wget https://fast.dpdk.org/rel/dpdk-24.11.tar.xz
tar xf dpdk-24.11.tar.xz
cd dpdk-24.11

# Build — sfc PMD is enabled by default
meson setup build --prefix=/usr/local -Dplatform=native
ninja -C build
sudo ninja -C build install
sudo ldconfig

# Verify sfc PMD is present
ls /usr/local/lib64/dpdk/pmds-*/ | grep sfc

# Verify pkg-config picks up the new install
pkg-config --modversion libdpdk

# The Makefile uses pkg-config --libs libdpdk which 
will automatically pick up the source-built install 
at /usr/local as long as ldconfig ran correctly. 

If pkg-config still finds the old system DPDK instead, force it:

PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig make clean make



The build will take a while on a server CPU with many cores — speed it up with:

ninja -C build -j $(nproc)
sudo ninja -C build install -j $(nproc)


After installing, verify the sfc PMD is present before rebuilding pcie_latency:

ls /usr/local/lib64/dpdk/pmds-*/ | grep sfc
# Should show: librte_net_sfc.so.24.11

Then rebuild pcie_latency against the new DPDK:

cd $pnl install location
# Ensure pkg-config finds the source-built DPDK not the system one
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig:$PKG_CONFIG_PATH
make clean
make

Verify the binary links against the right DPDK:
ldd ./pcie_latency | grep dpdk
# Should show path under /usr/local not /usr/lib64

If ldd still shows /usr/lib64/libdpdk.so, the system DPDK is taking precedence. Fix it by adding the local lib path to the linker:

echo /usr/local/lib64 | sudo tee /etc/ld.so.conf.d/dpdk-local.conf
sudo ldconfig

then make again
