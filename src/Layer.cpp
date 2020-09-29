/*
    MIT License

    Copyright (c) 2020 Błażej Szczygieł

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
#include <thread>
#include <mutex>
#include <map>
#include <set>

using namespace std;

constexpr auto g_enableExternalControlKey = "VK_LAYER_FLIMES_ENABLE_EXTERNAL_CONTROL";

constexpr auto g_framerateEnvKey = "VK_LAYER_FLIMES_FRAMERATE";
constexpr auto g_filterEnvKey = "VK_LAYER_FLIMES_FILTER";
constexpr auto g_mipLodBiasEnvKey = "VK_LAYER_FLIMES_MIP_LOD_BIAS";
constexpr auto g_anisotropyEnvKey = "VK_LAYER_FLIMES_MAX_ANISOTROPY";
constexpr auto g_minImageCountEnvKey = "VK_LAYER_FLIMES_MIN_IMAGE_COUNT";
constexpr auto g_presentModeEnvKey = "VK_LAYER_FLIMES_PRESENT_MODE";

static unique_ptr<ExternalControl> g_externalControl;

struct InstanceData
{
    PFN_vkGetInstanceProcAddr getProcAddr = nullptr;

    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR getPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkCreateDevice createDevice = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;

    VkInstance instance = VK_NULL_HANDLE;

    set<VkPhysicalDevice> physicalDevices;
};
static map<VkInstance, shared_ptr<InstanceData>> g_instances;
static shared_mutex g_instancesMutex;

struct DeviceData
{
    PFN_vkGetDeviceProcAddr getProcAddr = nullptr;

    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchainKHR = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImageKHR = nullptr;
    PFN_vkAcquireNextImage2KHR acquireNextImage2KHR = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;

    weak_ptr<InstanceData> instanceData;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    float maxSamplerLodBias = 0.0f;
    float maxSamplerAnisotropy = 1.0f;

    optional<FrameLimiter> frameLimiter;
};
static map<VkDevice, unique_ptr<DeviceData>> g_devices;
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

    double framerate = 0.0;

    optional<Filter> filter;
    optional<float> mipLodBias;
    float maxAnisotropy = 0.0f;

    uint32_t minImageCount = 0;
    optional<VkPresentModeKHR> presentMode;
};
static Config g_config = [] {
    Config config;

    cout << VK_LAYER_FLIMES_NAME << " v" << VK_LAYER_FLIMES_VERSION << " active" << "\n";

    if (auto env = getenv(g_framerateEnvKey); env && *env)
    {
        config.framerate = atof(env);
        if (config.framerate > 0.0)
            cout << "  Framerate: " << config.framerate << "\n";
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
            cout << "  Texture filtering: " << filterIt->first << "\n";
        }
    }
    if (auto env = getenv(g_mipLodBiasEnvKey); env && *env)
    {
        config.mipLodBias = atof(env);
        cout << "  Mip LOD bias: " << *config.mipLodBias << "\n";
    }
    if (auto env = getenv(g_anisotropyEnvKey); env && *env)
    {
        config.maxAnisotropy = atof(env);
        if (config.maxAnisotropy >= 1.0f)
            cout << "  Max anisotropy: " << config.maxAnisotropy << "\n";
    }

    if (auto env = getenv(g_minImageCountEnvKey); env && *env)
    {
        config.minImageCount = atoi(env);
        if (config.minImageCount > 0)
            cout << "  Min image count: " << config.minImageCount << "\n";
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
            cout << "  Present mode: " << modesIt->first << "\n";
        }
    }

    if (auto env = getenv(g_enableExternalControlKey); env && *env != '0')
    {
        g_externalControl = make_unique<ExternalControl>([](const string &str) { try {
            const auto fps = stod(str);
            if (g_config.framerate != fps)
            {
                cerr << "New framerate: " << fps << endl;

                scoped_lock devicesLock(g_devicesMutex);
                g_config.framerate = fps;
                for (auto &&[device, deviceData] : g_devices)
                    deviceData->frameLimiter.reset();
            }
        } catch (const invalid_argument &) {} });
    }

    cout << flush;

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

template<typename Fn>
static VkResult acquireNextImageCommon(VkDevice device, Fn &&fn)
{
    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return VK_ERROR_INITIALIZATION_FAILED;

    auto deviceData = devicesIt->second.get();

    limitFramerate(deviceData);

    return fn(deviceData);
}

/**/

static VKAPI_CALL VkResult vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
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

    instanceData->getPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    instanceData->getPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(getInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    instanceData->createDevice = reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(*pInstance, "vkCreateDevice"));
    instanceData->destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr(*pInstance, "vkDestroyInstance"));

    instanceData->instance = *pInstance;

    if (auto enumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getInstanceProcAddr(*pInstance, "vkEnumeratePhysicalDevices")))
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
static VKAPI_CALL void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
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

static VKAPI_CALL VkResult vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
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

    auto instance = instanceData->instance;
    auto getInstanceProcAddr = instanceData->getProcAddr;
    auto createDevice = instanceData->createDevice;

    auto getDeviceProcAddr = layerDeviceCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!getDeviceProcAddr)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!createDevice)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Advance the link info for the next element of the chain
    layerDeviceCreateInfo->u.pLayerInfo = layerDeviceCreateInfo->u.pLayerInfo->pNext;

    if (auto ret = createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice); ret != VK_SUCCESS)
        return ret;

    scoped_lock devicesLock(g_devicesMutex);

    auto &deviceData = g_devices[*pDevice];
    deviceData = make_unique<DeviceData>();

    deviceData->getProcAddr = getDeviceProcAddr;

    deviceData->createSampler = reinterpret_cast<PFN_vkCreateSampler>(getDeviceProcAddr(*pDevice, "vkCreateSampler"));
    deviceData->createSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(getDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR"));
    deviceData->acquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(getDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR"));
    deviceData->acquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(getDeviceProcAddr(*pDevice, "vkAcquireNextImage2KHR"));
    deviceData->destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getDeviceProcAddr(*pDevice, "vkDestroyDevice"));

    deviceData->instanceData = instanceData;

    deviceData->physicalDevice = physicalDevice;

    if (auto getPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties")))
    {
        VkPhysicalDeviceProperties physicalDeviceProperties = {};
        getPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
        deviceData->maxSamplerLodBias = physicalDeviceProperties.limits.maxSamplerLodBias;
        deviceData->maxSamplerAnisotropy = physicalDeviceProperties.limits.maxSamplerAnisotropy;
    }

    return VK_SUCCESS;
}
static VKAPI_CALL VkResult vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
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
static VKAPI_CALL VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
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

    if (g_config.presentMode && instanceData->getPhysicalDeviceSurfacePresentModesKHR)
    {
        uint32_t nPresentModes = 0;
        instanceData->getPhysicalDeviceSurfacePresentModesKHR(deviceData->physicalDevice, createInfo.surface, &nPresentModes, nullptr);

        vector<VkPresentModeKHR> presentModes(nPresentModes);
        instanceData->getPhysicalDeviceSurfacePresentModesKHR(deviceData->physicalDevice, createInfo.surface, &nPresentModes, presentModes.data());

        for (auto &&supportedPresentMode : presentModes)
        {
            if (supportedPresentMode == *g_config.presentMode)
            {
                createInfo.presentMode = *g_config.presentMode;
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

    return deviceData->createSwapchainKHR(device, &createInfo, pAllocator, pSwapchain);
}
static VKAPI_CALL VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    return acquireNextImageCommon(device, [&](DeviceData *deviceData) {
        return deviceData->acquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    });
}
static VKAPI_CALL VkResult vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex)
{
    return acquireNextImageCommon(device, [&](DeviceData *deviceData) {
        return deviceData->acquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    });
}
static VKAPI_CALL void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    scoped_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return;

    if (auto destroyDevice = devicesIt->second->destroyDevice)
        destroyDevice(device, pAllocator);

    g_devices.erase(devicesIt);
}

/**/

extern "C" VK_LAYER_EXPORT VKAPI_CALL PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *pName);
extern "C" VK_LAYER_EXPORT VKAPI_CALL PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *pName);

static const map<string_view, PFN_vkVoidFunction> g_instanceFunctions = {
    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr)},

    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance)},
    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice)},
    {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance)},
};
static const map<string_view, PFN_vkVoidFunction> g_deviceFunctions = {
    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)},

    {"vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(vkCreateSampler)},
    {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(vkCreateSwapchainKHR)},
    {"vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR)},
    {"vkAcquireNextImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR)},
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice)},
};

VK_LAYER_EXPORT VKAPI_CALL PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (auto fnsIt = g_instanceFunctions.find(pName); fnsIt != g_instanceFunctions.end())
        return fnsIt->second;

    if (auto fnsIt = g_deviceFunctions.find(pName); fnsIt != g_deviceFunctions.end())
        return fnsIt->second;

    shared_lock instancesLock(g_instancesMutex);

    auto instancesIt = g_instances.find(instance);
    if (instancesIt == g_instances.end())
        return nullptr;

    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(instancesIt->second->getProcAddr)(instance, pName);
}
VK_LAYER_EXPORT VKAPI_CALL PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    auto fnsIt = g_deviceFunctions.find(pName);
    if (fnsIt != g_deviceFunctions.end())
        return fnsIt->second;

    shared_lock devicesLock(g_devicesMutex);

    auto devicesIt = g_devices.find(device);
    if (devicesIt == g_devices.end())
        return nullptr;

    return reinterpret_cast<PFN_vkGetDeviceProcAddr>(devicesIt->second->getProcAddr)(device, pName);
}
