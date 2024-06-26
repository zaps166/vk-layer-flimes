/*
    MIT License

    Copyright (c) 2020-2021 Błażej Szczygieł

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "ExternalControl.hpp"
#include "FrameLimiter.hpp"

#include <vulkan/vk_layer.h>

#include <shared_mutex>
#include <iostream>
#include <optional>
#include <cctype>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <set>

#ifdef SW
#   include <pthread.h>
#   include <unordered_map>
#   include <cstring>
#endif

#ifndef VK_LAYER_EXPORT
#   define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

using namespace std;

constexpr auto g_enableExternalControlKey = "VK_LAYER_FLIMES_ENABLE_EXTERNAL_CONTROL";
constexpr auto g_externalControlVerboseKey = "VK_LAYER_FLIMES_EXTERNAL_CONTROL_VERBOSE";

constexpr auto g_framerateEnvKey = "VK_LAYER_FLIMES_FRAMERATE";
constexpr auto g_filterEnvKey = "VK_LAYER_FLIMES_FILTER";
constexpr auto g_mipLodBiasEnvKey = "VK_LAYER_FLIMES_MIP_LOD_BIAS";
constexpr auto g_anisotropyEnvKey = "VK_LAYER_FLIMES_MAX_ANISOTROPY";
constexpr auto g_minImageCountEnvKey = "VK_LAYER_FLIMES_MIN_IMAGE_COUNT";
constexpr auto g_presentModeEnvKey = "VK_LAYER_FLIMES_PRESENT_MODE";
constexpr auto g_preferMailboxPresentModeEnvKey = "VK_LAYER_FLIMES_PREFER_MAILBOX_PRESENT_MODE";

static unique_ptr<ExternalControl> g_externalControl;
static bool g_externalControlVerbose = false;

#ifdef SW
    static pthread_t g_drawThr = 0;
    static pair<uintptr_t, uintptr_t> g_drawInfo; // [draw count per image acquire, draw vertices per image acquire]
    static shared_mutex g_drawInfoMutex;
#endif

struct InstanceData
{
    PFN_vkGetInstanceProcAddr getProcAddr = nullptr;

    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;

    set<VkPhysicalDevice> physicalDevices;
};
static map<VkInstance, shared_ptr<InstanceData>> g_instances;
static shared_mutex g_instancesMutex;

struct DeviceData
{
    PFN_vkGetDeviceProcAddr getProcAddr = nullptr;

    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchainKHR = nullptr;
#ifdef SW
    PFN_vkCmdDraw cmdDraw = nullptr;
#endif
    PFN_vkAcquireNextImageKHR acquireNextImageKHR = nullptr;
    PFN_vkAcquireNextImage2KHR acquireNextImage2KHR = nullptr;
    PFN_vkQueuePresentKHR queuePresentKHR = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;

    weak_ptr<InstanceData> instanceData;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    float maxSamplerLodBias = 0.0f;
    float maxSamplerAnisotropy = 1.0f;

    optional<FrameLimiter> frameLimiter;

    vector<VkPresentModeKHR> presentModes;

    optional<VkPresentModeKHR> currentPresentMode;
    bool presentModeChanged = false;

#ifdef SW
    struct
    {
        bool wasLoading = false;
        optional<chrono::time_point<FrameLimiter::frame_clock>> loadedTimePoint;
        bool configPresentModeChanged = false;
    } sw;
#endif
};
static map<VkDevice, shared_ptr<DeviceData>> g_devices;
static map<VkQueue, shared_ptr<DeviceData>> g_queues;
static shared_mutex g_devicesMutex;

static const map<string_view, VkPresentModeKHR> g_presentModes {
    {"IMMEDIATE", VK_PRESENT_MODE_IMMEDIATE_KHR},
    {"MAILBOX", VK_PRESENT_MODE_MAILBOX_KHR},
    {"FIFO", VK_PRESENT_MODE_FIFO_KHR},
    {"FIFO_RELAXED", VK_PRESENT_MODE_FIFO_RELAXED_KHR},
};

struct Config
{
    enum class Filter
    {
        Nearest,
        Trilinear,
    };

#ifdef SW
    bool isSw = false;
#endif

    double framerate = 0.0;

    optional<Filter> filter;
    optional<float> mipLodBias;
    float maxAnisotropy = 0.0f;

    uint32_t minImageCount = 0;
    optional<VkPresentModeKHR> presentMode;
    bool preferMailboxPresentMode = false;
};
static Config g_config = [] {
    Config config;

    cerr << boolalpha << VK_LAYER_FLIMES_NAME << " v" << VK_LAYER_FLIMES_VERSION << " active" << "\n";

#ifdef SW
    config.isSw = (strcasestr(program_invocation_name, "SoulWorker") != nullptr) && (strcasestr(program_invocation_name, ".exe") != nullptr);
#endif

    if (auto env = getenv(g_framerateEnvKey); env && *env)
    {
        config.framerate = atof(env);
        if (config.framerate > 0.0)
            cerr << "  Framerate: " << config.framerate << "\n";
    }

    if (auto env = getenv(g_filterEnvKey); env && *env)
    {
        const map<string_view, Config::Filter> filters {
            {"NEAREST", Config::Filter::Nearest},
            {"TRILINEAR", Config::Filter::Trilinear},
        };

        string filterStr;
        while (*env)
            filterStr.push_back(toupper(*(env++)));

        auto filterIt = filters.find(filterStr);
        if (filterIt != filters.end())
        {
            config.filter = filterIt->second;
            cerr << "  Texture filtering: " << filterIt->first << "\n";
        }
    }
    if (auto env = getenv(g_mipLodBiasEnvKey); env && *env)
    {
        config.mipLodBias = atof(env);
        cerr << "  Mip LOD bias: " << *config.mipLodBias << "\n";
    }
    if (auto env = getenv(g_anisotropyEnvKey); env && *env)
    {
        config.maxAnisotropy = atof(env);
        if (config.maxAnisotropy >= 1.0f)
            cerr << "  Max anisotropy: " << config.maxAnisotropy << "\n";
    }

    if (auto env = getenv(g_minImageCountEnvKey); env && *env)
    {
        config.minImageCount = atoi(env);
        if (config.minImageCount > 0)
            cerr << "  Min image count: " << config.minImageCount << "\n";
    }
    if (auto env = getenv(g_presentModeEnvKey); env && *env)
    {
        string presentModeStr;
        while (*env)
            presentModeStr.push_back(toupper(*(env++)));

        auto modesIt = g_presentModes.find(presentModeStr);
        if (modesIt != g_presentModes.end())
        {
            config.presentMode = modesIt->second;
            cerr << "  Present mode: " << modesIt->first << "\n";
        }
    }
    if (auto env = getenv(g_preferMailboxPresentModeEnvKey); env && *env)
    {
        config.preferMailboxPresentMode = (atoi(env) > 0);
        if (config.preferMailboxPresentMode)
            cerr << "  Prefer MAILBOX present mode\n";
    }

    if (auto env = getenv(g_enableExternalControlKey); env && *env != '0')
    {
        g_externalControl = make_unique<ExternalControl>([](const string &str) {
            optional<VkPresentModeKHR> newPresentMode;
            string_view newPresentModeName;
            if (str == "AUTO")
            {
                newPresentModeName = str;
            }
            else
            {
                auto it = g_presentModes.find(str);
                if (it != g_presentModes.end())
                {
                    newPresentMode = it->second;
                    newPresentModeName = it->first;
                }
            }

            if (!newPresentModeName.empty())
            {
                scoped_lock devicesLock(g_devicesMutex);

                bool changed = false;
                for (auto &&[device, deviceData] : g_devices)
                {
                    if (!deviceData->currentPresentMode)
                        continue;

                    if ((!newPresentMode && g_config.presentMode) || (newPresentMode && deviceData->currentPresentMode != newPresentMode))
                    {
                        deviceData->presentModeChanged = true;
                        changed = true;
                    }
                }

                if (g_externalControlVerbose && (changed || g_config.presentMode != newPresentMode))
                    cerr << VK_LAYER_FLIMES_NAME << " new present mode: " << newPresentModeName << ", recreate swapchain: " << changed << endl;

                g_config.presentMode = newPresentMode;
            }
            else try
            {
                const auto fps = stod(str);
                if (g_config.framerate != fps)
                {
                    if (g_externalControlVerbose)
                        cerr << VK_LAYER_FLIMES_NAME << " new framerate: " << fps << endl;

                    scoped_lock devicesLock(g_devicesMutex);
                    g_config.framerate = fps;
                    for (auto &&[device, deviceData] : g_devices)
                        deviceData->frameLimiter.reset();
                }
            }
            catch (const invalid_argument &)
            {}
        });
    }
    if (auto env = getenv(g_externalControlVerboseKey); env && *env != '0')
    {
        g_externalControlVerbose = true;
    }

    cerr << flush;

    return config;
}();

/**/

static void limitFramerate(DeviceData *deviceData)
{
    if (!deviceData->frameLimiter)
        deviceData->frameLimiter.emplace(g_config.framerate);
    deviceData->frameLimiter->wait();
}

template<typename T>
static T *getLayerCreateInfo(const void *pNext, VkStructureType type)
{
    auto layerCreateInfo = reinterpret_cast<const T *>(pNext);
    while (layerCreateInfo && (layerCreateInfo->sType != type || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        layerCreateInfo = reinterpret_cast<const T *>(layerCreateInfo->pNext);
    return const_cast<T *>(layerCreateInfo);
}

#ifdef SW
static bool isGameLoading(DeviceData *deviceData)
{
    if (!g_config.isSw)
        return false;

    bool isSwLoading = false;
    {
        scoped_lock locker(g_drawInfoMutex);
        if (g_drawThr != 0)
        {
            auto &&[cnt, vert] = g_drawInfo;
            if ((cnt == 1 && vert == 6) || (cnt == 2 && vert == 12) || (cnt == 3 && vert == 15))
            {
                isSwLoading = true;
                deviceData->sw.wasLoading = true;
            }
            cnt = 0;
            vert = 0;
        }
    }
    if (!isSwLoading && deviceData->sw.wasLoading)
    {
        deviceData->sw.wasLoading = false;
        deviceData->sw.loadedTimePoint = FrameLimiter::frame_clock::now();
    }
    if (!isSwLoading && deviceData->sw.loadedTimePoint.has_value())
    {
        const auto msAfterLoading = chrono::duration_cast<chrono::milliseconds>(FrameLimiter::frame_clock::now() - deviceData->sw.loadedTimePoint.value()).count();
        if (msAfterLoading <= 2000)
        {
            // Keep unlocked framerate for a while
            isSwLoading = true;
        }
        else
        {
            deviceData->sw.loadedTimePoint.reset();
        }
    }
    if (isSwLoading
            && deviceData->currentPresentMode.has_value()
            && deviceData->currentPresentMode.value() != VK_PRESENT_MODE_IMMEDIATE_KHR
            && deviceData->currentPresentMode.value() != VK_PRESENT_MODE_MAILBOX_KHR)
    {
        // Disable blocking V-Sync (if enabled)
        bool hasImmediate = false;
        bool hasMailbox = false;
        for (auto &&presentMode : deviceData->presentModes)
        {
            if (presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                hasImmediate = true;
            else if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
                hasMailbox = true;
        }
        if (hasImmediate || hasMailbox)
        {
            g_config.presentMode = hasImmediate
                ? VK_PRESENT_MODE_IMMEDIATE_KHR
                : VK_PRESENT_MODE_MAILBOX_KHR
            ;
            deviceData->presentModeChanged = true;
            deviceData->sw.configPresentModeChanged = true;
        }
    }
    else if (!isSwLoading && deviceData->sw.configPresentModeChanged)
    {
        g_config.presentMode.reset();
        deviceData->presentModeChanged = true;
        deviceData->sw.configPresentModeChanged = false;
    }
    if (isSwLoading)
    {
        for (auto &&[device, deviceData] : g_devices)
            deviceData->frameLimiter.reset();
    }

    return isSwLoading;
}
#endif

template<typename Fn>
static VkResult acquireNextImageCommon(VkDevice device, Fn &&fn)
{
    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    auto deviceData = devicesIt->second.get();

#ifdef SW
    const bool gameLoading = isGameLoading(deviceData);
#endif

    if (deviceData->presentModeChanged)
        return VK_ERROR_OUT_OF_DATE_KHR;

    auto ret = fn(deviceData);
    if (ret == VK_SUCCESS || ret == VK_SUBOPTIMAL_KHR)
    {
#ifdef SW
        if (!gameLoading)
#endif
        {
            limitFramerate(deviceData);
        }
    }

    return ret;
}

/**/

static VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    auto layerInstanceCreateInfo = getLayerCreateInfo<VkLayerInstanceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!layerInstanceCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto getInstanceProcAddr = layerInstanceCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!getInstanceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto createInstanceFn = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstanceFn)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the link info for the next element of the chain
    layerInstanceCreateInfo->u.pLayerInfo = layerInstanceCreateInfo->u.pLayerInfo->pNext;

    if (auto ret = createInstanceFn(pCreateInfo, pAllocator, pInstance); ret != VK_SUCCESS)
        return ret;

    scoped_lock instancesLock(g_instancesMutex);

    auto &instanceData = g_instances[*pInstance];
    instanceData = make_shared<InstanceData>();

    instanceData->getProcAddr = getInstanceProcAddr;

    instanceData->getPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceProperties"));
    instanceData->getPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    instanceData->getPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    instanceData->createDevice = reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(*pInstance, "vkCreateDevice"));
    instanceData->destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(*pInstance, "vkDestroyInstance"));

    auto enumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(getInstanceProcAddr(*pInstance, "vkGetInstanceProcAddr"))(*pInstance, "vkEnumeratePhysicalDevices")
    );
    if (enumeratePhysicalDevices)
    {
        uint32_t physicalDeviceCount = 0;
        enumeratePhysicalDevices(*pInstance, &physicalDeviceCount, nullptr);

        vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
        enumeratePhysicalDevices(*pInstance, &physicalDeviceCount, physicalDevices.data());

        for (auto &&physicalDevice : physicalDevices)
            instanceData->physicalDevices.insert(physicalDevice);
    }

    return VK_SUCCESS;
}
static void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    scoped_lock instancesLock(g_instancesMutex);

    auto instancesIt = g_instances.find(instance);
    if (instancesIt == g_instances.end())
        return;

    if (auto destroyInstance = instancesIt->second->destroyInstance)
        destroyInstance(instance, pAllocator);

    g_instances.erase(instancesIt);
}

/**/

static VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    auto layerDeviceCreateInfo = getLayerCreateInfo<VkLayerDeviceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!layerDeviceCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto instanceData = [&]()->shared_ptr<InstanceData> {
        shared_lock instancesLock(g_instancesMutex);
        for (auto instancesPair : g_instances)
        {
            auto &instanceData = instancesPair.second;
            if (instanceData->physicalDevices.count(physicalDevice) > 0)
                return instanceData;
        }
        return nullptr;
    }();
    if (!instanceData)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto getDeviceProcAddr = layerDeviceCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!getDeviceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!instanceData->createDevice)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the link info for the next element of the chain
    layerDeviceCreateInfo->u.pLayerInfo = layerDeviceCreateInfo->u.pLayerInfo->pNext;

    if (auto ret = instanceData->createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice); ret != VK_SUCCESS)
        return ret;

    scoped_lock devicesLock(g_devicesMutex);

    auto &deviceData = g_devices[*pDevice];
    deviceData = make_shared<DeviceData>();

    deviceData->getProcAddr = getDeviceProcAddr;

    deviceData->createSampler = reinterpret_cast<PFN_vkCreateSampler>(getDeviceProcAddr(*pDevice, "vkCreateSampler"));
    deviceData->createSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(getDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR"));
#ifdef SW
    if (g_config.isSw)
        deviceData->cmdDraw = reinterpret_cast<PFN_vkCmdDraw>(getDeviceProcAddr(*pDevice, "vkCmdDraw"));
#endif
    deviceData->acquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(getDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR"));
    deviceData->acquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(getDeviceProcAddr(*pDevice, "vkAcquireNextImage2KHR"));
    deviceData->queuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(getDeviceProcAddr(*pDevice, "vkQueuePresentKHR"));
    deviceData->destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getDeviceProcAddr(*pDevice, "vkDestroyDevice"));

    auto vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(getDeviceProcAddr(*pDevice, "vkGetDeviceQueue"));
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
    {
        const auto &queueCreateInfo = pCreateInfo->pQueueCreateInfos[i];
        for (uint32_t j = 0; j < queueCreateInfo.queueCount; ++j)
        {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(*pDevice, queueCreateInfo.queueFamilyIndex, j, &queue);
            g_queues[queue] = deviceData;
        }
    }

    deviceData->instanceData = instanceData;

    deviceData->physicalDevice = physicalDevice;

    if (instanceData->getPhysicalDeviceProperties)
    {
        VkPhysicalDeviceProperties physicalDeviceProperties = {};
        instanceData->getPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
        deviceData->maxSamplerLodBias = physicalDeviceProperties.limits.maxSamplerLodBias;
        deviceData->maxSamplerAnisotropy = physicalDeviceProperties.limits.maxSamplerAnisotropy;
    }

    return VK_SUCCESS;
}
static VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    auto deviceData = devicesIt->second.get();

    auto createInfo = *pCreateInfo;

    if (g_config.filter) switch (*g_config.filter)
    {
        case Config::Filter::Nearest:
            createInfo.magFilter = VK_FILTER_NEAREST;
            createInfo.minFilter = VK_FILTER_NEAREST;
            createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            break;
        case Config::Filter::Trilinear:
            createInfo.magFilter = VK_FILTER_LINEAR;
            createInfo.minFilter = VK_FILTER_LINEAR;
            createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            break;
    }
    if (g_config.mipLodBias)
    {
        createInfo.mipLodBias = min(*g_config.mipLodBias, deviceData->maxSamplerLodBias);
    }
    if (g_config.maxAnisotropy >= 1.0f)
    {
        createInfo.anisotropyEnable = VK_TRUE;
        createInfo.maxAnisotropy = min(g_config.maxAnisotropy, deviceData->maxSamplerAnisotropy);
    }

    return deviceData->createSampler(device, &createInfo, pAllocator, pSampler);
}
static VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    auto deviceData = devicesIt->second.get();

    auto instanceData = deviceData->instanceData.lock();
    if (!instanceData)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto createInfo = *pCreateInfo;

    if (instanceData->getPhysicalDeviceSurfacePresentModesKHR)
    {
        uint32_t nPresentModes = 0;
        instanceData->getPhysicalDeviceSurfacePresentModesKHR(deviceData->physicalDevice, createInfo.surface, &nPresentModes, nullptr);

        deviceData->presentModes.clear();
        deviceData->presentModes.resize(nPresentModes);
        instanceData->getPhysicalDeviceSurfacePresentModesKHR(deviceData->physicalDevice, createInfo.surface, &nPresentModes, deviceData->presentModes.data());
    }

    if (g_config.presentMode || g_config.preferMailboxPresentMode)
    {
        for (auto &&supportedPresentMode : deviceData->presentModes)
        {
            if (g_config.presentMode && supportedPresentMode == *g_config.presentMode)
            {
                createInfo.presentMode = *g_config.presentMode;
                break;
            }

            if (g_config.preferMailboxPresentMode && supportedPresentMode == VK_PRESENT_MODE_MAILBOX_KHR && createInfo.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                createInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                if (!g_config.presentMode)
                    break;
            }
        }
    }

    if (g_config.minImageCount > 0 && instanceData->getPhysicalDeviceSurfaceCapabilitiesKHR)
    {
        VkSurfaceCapabilitiesKHR surfaceCapabilities = {};
        if (instanceData->getPhysicalDeviceSurfaceCapabilitiesKHR(deviceData->physicalDevice, createInfo.surface, &surfaceCapabilities) == VK_SUCCESS)
        {
            uint32_t minImageCount = max(g_config.minImageCount, surfaceCapabilities.minImageCount);
            if (surfaceCapabilities.maxImageCount > 0)
                minImageCount = min(minImageCount, surfaceCapabilities.maxImageCount);
            createInfo.minImageCount = minImageCount;
        }
    }

    deviceData->currentPresentMode = createInfo.presentMode;
    deviceData->presentModeChanged = false;

    return deviceData->createSwapchainKHR(device, &createInfo, pAllocator, pSwapchain);
}
#ifdef SW
static void vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    // This function must never be called if "g_config.isSw" is "false"

    {
        const pthread_t currThr = pthread_self();
        scoped_lock locker(g_drawInfoMutex);
        if (g_drawThr == 0)
        {
            char name[16];
            if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && strcmp(name, "dxvk-cs") == 0)
                g_drawThr = currThr;
        }
        if (g_drawThr == currThr)
        {
            g_drawInfo.first += 1;
            g_drawInfo.second += vertexCount;
        }
    }

    shared_lock devicesLock(g_devicesMutex);

    if (g_devices.size() != 1)
        return;

    g_devices.begin()->second->cmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}
#endif
static VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    return acquireNextImageCommon(device, [&](DeviceData *deviceData) {
        return deviceData->acquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    });
}
static VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex)
{
    return acquireNextImageCommon(device, [&](DeviceData *deviceData) {
        return deviceData->acquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    });
}
static VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    shared_lock devicesLock(g_devicesMutex);

    auto queuesIt = g_queues.find(queue);
    if (queuesIt == g_queues.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    auto deviceData = queuesIt->second.get();

    VkBaseOutStructure *backupNextPtr = nullptr;
    VkBaseOutStructure *backupStructPtr = nullptr;

    if (g_config.presentMode || g_config.preferMailboxPresentMode)
    {
        // Prevent setting present mode here when we have forced present mode.
        auto next = reinterpret_cast<VkBaseOutStructure *>(const_cast<void *>(pPresentInfo->pNext));
        auto prev = reinterpret_cast<VkBaseOutStructure *>(const_cast<VkPresentInfoKHR *>(pPresentInfo));
        while (next)
        {
            if (next->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT)
            {
                backupStructPtr = prev;
                backupNextPtr = prev->pNext;
                prev->pNext = next->pNext;
                break;
            }
            prev = next;
            next = next->pNext;
        }
    }

    auto ret = deviceData->queuePresentKHR(queue, pPresentInfo);

    if (backupStructPtr)
        backupStructPtr->pNext = backupNextPtr;

    return ret;
}
static void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    scoped_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return;

    auto deviceData = devicesIt->second.get();

    deviceData->destroyDevice(device, pAllocator);

    for (auto it = g_queues.begin(); it != g_queues.end();)
    {
        if (it->second.get() == deviceData)
            it = g_queues.erase(it);
        else
            ++it;
    }

    g_devices.erase(devicesIt);
}

/**/

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddrFlimes(VkInstance instance, const char *pName);
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddrFlimes(VkDevice device, const char *pName);

static const map<string_view, PFN_vkVoidFunction> g_instanceFunctions = {
    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddrFlimes)},
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
    {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance)},
};
static const map<string_view, PFN_vkVoidFunction> g_deviceFunctions = {
    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddrFlimes)},
    {"vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(vkCreateSampler)},
#ifdef SW
    {"vkCmdDraw", reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw)},
#endif
    {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR)},
    {"vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR)},
    {"vkAcquireNextImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR)},
    {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR)},
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
};

extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddrFlimes(VkInstance instance, const char *pName)
{
    if (auto fnsIt = g_instanceFunctions.find(pName); fnsIt != g_instanceFunctions.end())
        return fnsIt->second;

    if (auto fnsIt = g_deviceFunctions.find(pName); fnsIt != g_deviceFunctions.end())
        return fnsIt->second;

    shared_lock instancesLock(g_instancesMutex);

    auto instancesIt = g_instances.find(instance);
    if (instancesIt == g_instances.end())
        return nullptr;

    return instancesIt->second->getProcAddr(instance, pName);
}
extern "C" VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddrFlimes(VkDevice device, const char *pName)
{
    if (auto fnsIt = g_deviceFunctions.find(pName); fnsIt != g_deviceFunctions.end())
    {
#ifdef SW
        if (fnsIt->second != reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw) || g_config.isSw)
#endif
            return fnsIt->second;
    }

    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return nullptr;

    return devicesIt->second->getProcAddr(device, pName);
}
