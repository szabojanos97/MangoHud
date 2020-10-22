#include "gpu.h"
#include <inttypes.h>
#include <memory>
#include <functional>
#include <cstring>
#include "nvctrl.h"
#ifdef HAVE_NVML
#include "nvidia_info.h"
#endif

#ifdef HAVE_LIBDRM_AMDGPU
#include "auth.h"
#include <xf86drm.h>
#include <libdrm/amdgpu_drm.h>
#include <libdrm/amdgpu.h>
#include <unistd.h>
#include <fcntl.h>
#endif

struct gpuInfo gpu_info {};
amdgpu_files amdgpu {};
decltype(&getAmdGpuInfo) getAmdGpuInfo_actual = nullptr;

bool checkNvidia(const char *pci_dev){
    bool nvSuccess = false;
#ifdef HAVE_NVML
    nvSuccess = checkNVML(pci_dev) && getNVMLInfo();
#endif
#ifdef HAVE_XNVCTRL
    if (!nvSuccess)
        nvSuccess = checkXNVCtrl();
#endif
#ifdef _WIN32
    if (!nvSuccess)
        nvSuccess = checkNVAPI();
#endif
    return nvSuccess;
}

void getNvidiaGpuInfo(){
#ifdef HAVE_NVML
    if (nvmlSuccess){
        getNVMLInfo();
        gpu_info.load = nvidiaUtilization.gpu;
        gpu_info.temp = nvidiaTemp;
        gpu_info.memoryUsed = nvidiaMemory.used / (1024.f * 1024.f * 1024.f);
        gpu_info.CoreClock = nvidiaCoreClock;
        gpu_info.MemClock = nvidiaMemClock;
        gpu_info.powerUsage = nvidiaPowerUsage / 1000;
        return;
    }
#endif
#ifdef HAVE_XNVCTRL
    if (nvctrlSuccess) {
        getNvctrlInfo();
        gpu_info.load = nvctrl_info.load;
        gpu_info.temp = nvctrl_info.temp;
        gpu_info.memoryUsed = nvctrl_info.memoryUsed / (1024.f);
        gpu_info.CoreClock = nvctrl_info.CoreClock;
        gpu_info.MemClock = nvctrl_info.MemClock;
        gpu_info.powerUsage = 0;
        return;
    }
#endif
#ifdef _WIN32
nvapi_util();
#endif
}

void getAmdGpuInfo(){
    int64_t value = 0;

    if (amdgpu.busy) {
        rewind(amdgpu.busy);
        fflush(amdgpu.busy);
        if (fscanf(amdgpu.busy, "%d", &gpu_info.load) != 1)
            gpu_info.load = 0;
        gpu_info.load = gpu_info.load;
    }

    if (amdgpu.temp) {
        rewind(amdgpu.temp);
        fflush(amdgpu.temp);
        if (fscanf(amdgpu.temp, "%d", &gpu_info.temp) != 1)
            gpu_info.temp = 0;
        gpu_info.temp /= 1000;
    }

    if (amdgpu.vram_total) {
        rewind(amdgpu.vram_total);
        fflush(amdgpu.vram_total);
        if (fscanf(amdgpu.vram_total, "%" PRId64, &value) != 1)
            value = 0;
        gpu_info.memoryTotal = float(value) / (1024 * 1024 * 1024);
    }

    if (amdgpu.vram_used) {
        rewind(amdgpu.vram_used);
        fflush(amdgpu.vram_used);
        if (fscanf(amdgpu.vram_used, "%" PRId64, &value) != 1)
            value = 0;
        gpu_info.memoryUsed = float(value) / (1024 * 1024 * 1024);
    }

    if (amdgpu.core_clock) {
        rewind(amdgpu.core_clock);
        fflush(amdgpu.core_clock);
        if (fscanf(amdgpu.core_clock, "%" PRId64, &value) != 1)
            value = 0;

        gpu_info.CoreClock = value / 1000000;
    }

    if (amdgpu.memory_clock) {
        rewind(amdgpu.memory_clock);
        fflush(amdgpu.memory_clock);
        if (fscanf(amdgpu.memory_clock, "%" PRId64, &value) != 1)
            value = 0;

        gpu_info.MemClock = value / 1000000;
    }

    if (amdgpu.power_usage) {
        rewind(amdgpu.power_usage);
        fflush(amdgpu.power_usage);
        if (fscanf(amdgpu.power_usage, "%" PRId64, &value) != 1)
            value = 0;

        gpu_info.powerUsage = value / 1000000;
    }
}

#ifdef HAVE_LIBDRM_AMDGPU
#define DRM_ATLEAST_VERSION(maj, min) \
    (amdgpu_dev->drm_major > maj || (amdgpu_dev->drm_major == maj && amdgpu_dev->drm_minor >= min))

struct amdgpu_state
{
    amdgpu_device_handle dev;
    int fd;
    uint32_t drm_major, drm_minor;
};

void amdgpu_cleanup(amdgpu_state *state){
    amdgpu_device_deinitialize(state->dev);
    close(state->fd);
}

typedef std::unique_ptr<amdgpu_state, std::function<void(amdgpu_state*)>> amdgpu_ptr;
static amdgpu_ptr amdgpu_dev;

bool amdgpu_open(const char *pci_dev) {

    int fd = drmOpen(NULL, pci_dev); // pci:0000:XX:XX.X
    //int fd = open("/dev/dri/card0", O_RDWR);

    if (fd < 0) {
        perror("MANGOHUD: Failed to open DRM device"); // Gives sensible perror message?
        return false;
    }

    drmVersionPtr ver = drmGetVersion(fd);

    if (!ver) {
        perror("MANGOHUD: Failed to query driver version");
        close(fd);
        return false;
    }

    if (strcmp(ver->name, "amdgpu")) {
        fprintf(stderr, "MANGOHUD: Unsupported driver %s\n", ver->name);
        close(fd);
        drmFreeVersion(ver);
        return false;
    }
    drmFreeVersion(ver);

    if (!authenticate_drm(fd))
        return false;

    uint32_t drm_major, drm_minor;
    amdgpu_device_handle dev;
    if (amdgpu_device_initialize(fd, &drm_major, &drm_minor, &dev)){
        perror("MANGOHUD: Failed to initialize amdgpu device");
        close(fd);
        return false;
    }

    amdgpu_dev = {new amdgpu_state{dev, fd, drm_major, drm_minor}, amdgpu_cleanup};
    return true;
}

void getAmdGpuInfo_libdrm(){
    uint64_t value = 0;
    uint32_t value32 = 0;

    if (!amdgpu_query_info(amdgpu_dev->dev, AMDGPU_INFO_VRAM_USAGE, sizeof(uint64_t), &value))
        gpu_info.memoryUsed = float(value) / (1024 * 1024 * 1024);

    // FIXME probably not correct sensor
    if (!amdgpu_query_info(amdgpu_dev->dev, AMDGPU_INFO_MEMORY, sizeof(uint64_t), &value))
        gpu_info.memoryTotal = float(value) / (1024 * 1024 * 1024);

    if (DRM_ATLEAST_VERSION(3, 11)) {
        if (!amdgpu_query_sensor_info(amdgpu_dev->dev, AMDGPU_INFO_SENSOR_GFX_SCLK, sizeof(uint32_t), &value32))
            gpu_info.CoreClock = value32;

        if (!amdgpu_query_sensor_info(amdgpu_dev->dev, AMDGPU_INFO_SENSOR_GFX_MCLK, sizeof(uint32_t), &value32)) // XXX Doesn't work on APUs
            gpu_info.MemClock = value32;

        if (!amdgpu_query_sensor_info(amdgpu_dev->dev, AMDGPU_INFO_SENSOR_GPU_LOAD, sizeof(uint32_t), &value32))
            gpu_info.load = value32;

        if (!amdgpu_query_sensor_info(amdgpu_dev->dev, AMDGPU_INFO_SENSOR_GPU_TEMP, sizeof(uint32_t), &value32))
            gpu_info.temp = value32 / 1000;

        if (!amdgpu_query_sensor_info(amdgpu_dev->dev, AMDGPU_INFO_SENSOR_GPU_AVG_POWER, sizeof(uint32_t), &value32))
            gpu_info.powerUsage = value32;
    }
}
#endif
